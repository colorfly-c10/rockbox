/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2016 by Roman Stolyarov
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "system.h"
#include "kernel.h"
#include "logf.h"
#include "audio.h"
#include "sound.h"
#include "pcm.h"
#include "pcm-internal.h"
#include "pcm_sink.h"
#include "cpu.h"
#include "dma-target.h"
#ifdef HAVE_PCM_TARGET_HEADER
#include "pcm-target.h"
#endif

/****************************************************************************
 ** Playback DMA transfer
 **/

static unsigned int play_lock;
static const void* playback_address;

static void sink_dma_postinit(void)
{
    audiohw_postinit();

#ifndef JZ4760_AIC_EXTERNAL_CLOCK
    /* Flush FIFO */
    __aic_flush_tfifo();
#else
    /* With external clocks, a flush requested while AIC is stopped may
     * still be pending when the first DMA burst starts. */
    REG_AIC_CR &= ~AIC_CR_TFLUSH;
    __aic_clear_errors();
#endif
}

static void sink_dma_init(void)
{
    system_enable_irq(DMA_IRQ(DMA_AIC_TX_CHANNEL));

    /* Initialize default register values. */
    audiohw_init();
}

static void sink_set_freq(uint16_t freq)
{
    audiohw_set_frequency(freq);
}

#ifdef JZ4760_DMA_DES8
static bool jz_dma_completion_pending;

/* DMAC fetches descriptors directly from SDRAM: reserve a full cache line.
 * Rockbox submits one PCM fragment at a time, so this descriptor is unlinked. */


static struct jz_dma_desc jz_play_desc CACHEALIGN_ATTR;

static inline void jz_aic_dma_clock_enable(void)
{
    /* Original JZ4760 has a channel-zero clock-gate quirk: Ingenic's own
     * allocator writes bit 1 for global channels 0 and 6. JZ4760B removed
     * this quirk and therefore is not a valid reference for this write. */
    #ifdef JZ4760_AIC_DMA_CLOCK_MASK
    REG_DMAC_DMACKE(DMA_AIC_TX_CHANNEL / HALF_DMA_NUM) =
        JZ4760_AIC_DMA_CLOCK_MASK;
#else
    __dmac_channel_enable_clk(DMA_AIC_TX_CHANNEL);
#endif
}

static inline void jz_dma_channel_reset(void)
{
    const unsigned int controller = DMA_AIC_TX_CHANNEL / HALF_DMA_NUM;
    const unsigned int pending =
        1u << (DMA_AIC_TX_CHANNEL % HALF_DMA_NUM);

    REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) = 0;
    REG_DMAC_DCMD(DMA_AIC_TX_CHANNEL) = 0;
    REG_DMAC_DSAR(DMA_AIC_TX_CHANNEL) = 0;
    REG_DMAC_DTAR(DMA_AIC_TX_CHANNEL) = 0;
    REG_DMAC_DTCR(DMA_AIC_TX_CHANNEL) = 0;
    REG_DMAC_DRSR(DMA_AIC_TX_CHANNEL) = 0;
    REG_DMAC_DDA(DMA_AIC_TX_CHANNEL) = 0;
    REG_DMAC_DMAIPR(controller) &= ~pending;
    REG_DMAC_DMACR(controller) &= ~(DMAC_DMACR_HLT | DMAC_DMACR_AR);
}

static inline void jz_dma_descriptor_start(void)
{
    const unsigned int controller = DMA_AIC_TX_CHANNEL / HALF_DMA_NUM;
    const unsigned int channel = DMA_AIC_TX_CHANNEL % HALF_DMA_NUM;

    REG_DMAC_DMACR(controller) |= DMAC_DMACR_DMAE;
    REG_DMAC_DMADBSR(controller) = 1u << channel;
    REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) |= DMAC_DCCSR_EN;
}
#endif

static inline void set_dma(const void *addr, size_t size)
{
#ifndef JZ4760_DMA_DES8
    int burst_size;
#endif
    logf("%x %d %x", (unsigned int)addr, size, REG_AIC_SR);

    commit_discard_dcache_range(addr, size);

#ifdef JZ4760_DMA_DES8
    unsigned int geometry;
    unsigned int count;

    /* Packed interleaved S16 frames use SWDH32/DWDH16. Select smaller units
     * when either the fragment address or its length lacks burst alignment. */
    if ((((unsigned long)addr | size) & 15) == 0)
    {
        geometry = DMAC_DCMD_DS_16BYTE;
        count = size >> 4;
    }
    else if ((((unsigned long)addr | size) & 3) == 0)
    {
        geometry = DMAC_DCMD_DS_32BIT;
        count = size >> 2;
    }
    else
    {
        geometry = DMAC_DCMD_DS_16BIT;
        count = size >> 1;
    }

    jz_dma_channel_reset();

    jz_play_desc.dcmd = DMAC_DCMD_SAI | DMAC_DCMD_SWDH_32 |
        DMAC_DCMD_DWDH_16 | geometry | DMAC_DCMD_TIE;
    jz_play_desc.dsadr = PHYSADDR((unsigned long)addr);
    jz_play_desc.dtadr = PHYSADDR((unsigned long)AIC_DR);
    jz_play_desc.ddadr = count;
    jz_play_desc.dstrd = 0;
    jz_play_desc.dreqt = DMAC_DRSR_RS_AICOUT;
    jz_play_desc.reserved0 = 0;
    jz_play_desc.reserved1 = 0;
    commit_discard_dcache_range(&jz_play_desc, sizeof(jz_play_desc));

    REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) = DMAC_DCCSR_DES8;
    REG_DMAC_DRSR(DMA_AIC_TX_CHANNEL) = jz_play_desc.dreqt;
    REG_DMAC_DDA(DMA_AIC_TX_CHANNEL) =
        PHYSADDR((unsigned long)&jz_play_desc);
#else
    if(size % 16)
    {
        if(size % 4)
        {
            size /= 2;
            burst_size = DMAC_DCMD_DS_16BIT;
        }
        else
        {
            size /= 4;
            burst_size = DMAC_DCMD_DS_32BIT;
        }
    }
    else
    {
        size /= 16;
        burst_size = DMAC_DCMD_DS_16BYTE;
    }

    REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) = 0;
    REG_DMAC_DSAR(DMA_AIC_TX_CHANNEL)  = PHYSADDR((unsigned long)addr);
    REG_DMAC_DTAR(DMA_AIC_TX_CHANNEL)  = PHYSADDR((unsigned long)AIC_DR);
    REG_DMAC_DTCR(DMA_AIC_TX_CHANNEL)  = size;
    REG_DMAC_DRSR(DMA_AIC_TX_CHANNEL)  = DMAC_DRSR_RS_AICOUT;
    REG_DMAC_DCMD(DMA_AIC_TX_CHANNEL)  = (DMAC_DCMD_SAI | DMAC_DCMD_SWDH_32 | burst_size | DMAC_DCMD_DWDH_16 | DMAC_DCMD_TIE);
    REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) = DMAC_DCCSR_NDES | DMAC_DCCSR_EN;
#endif

    playback_address = addr;
}

static inline void play_dma_callback(void)
{
    const void *start;
    size_t size;

#ifdef PCM_DMA_BUFFER_COMPLETE
    PCM_DMA_BUFFER_COMPLETE();
#endif

    if (pcm_play_dma_complete_callback(PCM_DMAST_OK, &start, &size))
    {
        set_dma(start, size);
#ifdef JZ4760_DMA_DES8
        jz_dma_descriptor_start();
#else
        REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) |= DMAC_DCCSR_EN;
#endif
        pcm_play_dma_status_callback(PCM_DMAST_STARTED);
    }
}

void DMA_CALLBACK(DMA_AIC_TX_CHANNEL)(void) __attribute__ ((section(".icode")));
#ifdef JZ4760_DMA_DES8
void DMA_CALLBACK(DMA_AIC_TX_CHANNEL)(void)
{
    unsigned int state = REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL);

    const unsigned int controller = DMA_AIC_TX_CHANNEL / HALF_DMA_NUM;
    const unsigned int pending =
        1u << (DMA_AIC_TX_CHANNEL % HALF_DMA_NUM);

    /* Acknowledge terminal count before the callback can arm the next
     * transfer. Clearing the controller pending bit prevents a stale parent
     * interrupt from consuming that new buffer immediately. */
    REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) &=
        ~(DMAC_DCCSR_TT | DMAC_DCCSR_CT);
    REG_DMAC_DMAIPR(controller) &= ~pending;

    if (state & (DMAC_DCCSR_AR | DMAC_DCCSR_HLT |
                 DMAC_DCCSR_INV))
    {
        logf("PCM DMA error %08x", state);
        REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) &= ~DMAC_DCCSR_INV;
        pcm_play_dma_complete_callback(PCM_DMAST_ERR_DMA, NULL, NULL);
        return;
    }

    if (state & DMAC_DCCSR_TT)
    {
        /* Descriptor mode raises TT only after the descriptor transfer count
         * has been consumed by AIC requests. Never accept DCCSR bit 1 here. */
        /* Defer callbacks while the PCM core is locked. */
        if (play_lock)
            jz_dma_completion_pending = true;
        else
            play_dma_callback();
    }
}

#else
void DMA_CALLBACK(DMA_AIC_TX_CHANNEL)(void)
{
    if (REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) & DMAC_DCCSR_AR)
    {
        logf("PCM DMA address error");
        REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) &= ~DMAC_DCCSR_AR;
    }

    if (REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) & DMAC_DCCSR_HLT)
    {
        logf("PCM DMA halt");
        REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) &= ~DMAC_DCCSR_HLT;
    }

    if (REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) & DMAC_DCCSR_TT)
    {
        REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) &= ~DMAC_DCCSR_TT;
        play_dma_callback();
    }
}

#endif

static void sink_dma_stop(void)
{
#ifdef PCM_DMA_STOP
    PCM_DMA_STOP();
#endif
    int flags = disable_irq_save();

#ifdef JZ4760_DMA_DES8
    jz_dma_channel_reset();
    jz_dma_completion_pending = false;

#ifdef JZ4760_AIC_EXTERNAL_CLOCK
    /* Retain AIC/ESCLK until audiohw_close() to preserve the slave clock
     * state across pause and restart. */
    REG_AIC_CR &= ~(AIC_CR_TDMS | AIC_CR_ERPL);
    REG_AIC_CR &= ~AIC_CR_TFLUSH;
    __aic_clear_errors();
#else
    __dmac_channel_disable_clk(DMA_AIC_TX_CHANNEL);
    __aic_disable_replay();
#endif
#else
    REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) = (REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) | DMAC_DCCSR_HLT) & ~DMAC_DCCSR_EN;
    __dmac_channel_disable_clk(DMA_AIC_TX_CHANNEL);
    __aic_disable_replay();
#endif

    restore_irq(flags);
}

static void sink_dma_start(const void *addr, size_t size)
{
    sink_dma_stop();

#ifdef JZ4760_DMA_DES8
    jz_aic_dma_clock_enable();
#else
    __dmac_channel_enable_clk(DMA_AIC_TX_CHANNEL);
#endif

    set_dma(addr, size);

#ifdef JZ4760_DMA_DES8
    /* Stock wakes the DAC before arming the descriptor transfer. */
#ifdef PCM_DMA_START
    PCM_DMA_START();
#endif

    int flags = disable_irq_save();

    /* set_dma() installed one unlinked descriptor. TFLUSH must be
     * clear before the first request; stock does not flush at stream start. */
    REG_AIC_CR &= ~AIC_CR_TFLUSH;
    __aic_clear_errors();

    jz_dma_descriptor_start();
    /* Stock active captures have ETUR/EROR enabled at every source depth;
     * they do not select a wider output format. Rockbox PCM remains S16. */
    REG_AIC_CR |= AIC_CR_TDMS;
    REG_AIC_CR |= AIC_CR_ETUR | AIC_CR_EROR;
    REG_AIC_CR |= AIC_CR_ERPL;

    restore_irq(flags);

#ifdef PCM_OUTPUT_STARTED
    PCM_OUTPUT_STARTED();
#endif
#else
    __aic_enable_replay();
    __dmac_channel_enable_irq(DMA_AIC_TX_CHANNEL);
#endif
}

static void sink_lock(void)
{
    int flags = disable_irq_save();

#ifdef JZ4760_DMA_DES8
    ++play_lock;
#else
    if (++play_lock == 1)
        __dmac_channel_disable_irq(DMA_AIC_TX_CHANNEL);
#endif

    restore_irq(flags);
}

static void sink_unlock(void)
{
    int flags = disable_irq_save();

#ifdef JZ4760_DMA_DES8
    if (--play_lock == 0 && jz_dma_completion_pending)
    {
        jz_dma_completion_pending = false;
        play_dma_callback();
    }
#else
    if (--play_lock == 0)
        __dmac_channel_enable_irq(DMA_AIC_TX_CHANNEL);
#endif

    restore_irq(flags);
}

struct pcm_sink builtin_pcm_sink = {
    .caps = {
        .samprs       = hw_freq_sampr,
        .num_samprs   = HW_NUM_FREQ,
        .default_freq = HW_FREQ_DEFAULT,
    },
    .ops = {
        .init     = sink_dma_init,
        .postinit = sink_dma_postinit,
        .set_freq = sink_set_freq,
        .lock     = sink_lock,
        .unlock   = sink_unlock,
        .play     = sink_dma_start,
        .stop     = sink_dma_stop,
    },
};
