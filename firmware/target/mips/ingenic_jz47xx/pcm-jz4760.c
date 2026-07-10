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
#ifdef COLORFLY_C10
#include "colorfly_c10/codec-colorfly-c10.h"
#endif

/* Missing from Rockbox's compatibility header, present on JZ4760/JZ4760B. */
#define DMAC_DCCSR_INV_COMPAT (1u << 6)

/****************************************************************************
 ** Playback DMA transfer
 **/

static unsigned int play_lock;
static const void* playback_address;

static void sink_dma_postinit(void)
{
    audiohw_postinit();

#ifndef COLORFLY_C10
    /* Flush FIFO */
    __aic_flush_tfifo();
#else
    /* The stock C10 path leaves TFLUSH clear while the AIC is stopped.
     * Setting it with the controller disabled can leave the first DMA burst
     * racing a pending flush when playback starts. */
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

#ifdef COLORFLY_C10
static bool c10_dma_completion_pending;

/* Stock C10 playback and the working C10 MSC2 path both use JZ4760 8-word
 * descriptors. Keep this on its own cache line because DMAC fetches it
 * directly from SDRAM. Rockbox submits one PCM fragment at a time, so the
 * descriptor is intentionally unlinked. */
struct c10_pcm_dma_desc {
    volatile unsigned int dcmd;
    volatile unsigned int dsadr;
    volatile unsigned int dtadr;
    volatile unsigned int ddadr;
    volatile unsigned int dstrd;
    volatile unsigned int dreqt;
    volatile unsigned int reserved0;
    volatile unsigned int reserved1;
};

static struct c10_pcm_dma_desc c10_play_desc CACHEALIGN_ATTR;

static inline void c10_aic_dma_clock_enable(void)
{
    /* Original JZ4760 has a channel-zero clock-gate quirk: Ingenic's own
     * allocator writes bit 1 for global channels 0 and 6. JZ4760B removed
     * this quirk and therefore is not a valid reference for this write. */
    REG_DMAC_DMACKE(1) = 0x2;
}

static inline void c10_dma_channel_reset(void)
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

static inline void c10_dma_descriptor_start(void)
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
#ifndef COLORFLY_C10
    int burst_size;
    unsigned int command;
#endif
    logf("%x %d %x", (unsigned int)addr, size, REG_AIC_SR);

    commit_discard_dcache_range(addr, size);

#ifdef COLORFLY_C10
    unsigned int geometry;
    unsigned int count;

    /* Rockbox supplies packed interleaved S16 frames. Live stock playback
     * uses SWDH32/DWDH16 and 16-byte units (DCMD 0x00802303); unlike the
     * generic jz_set_alsa_dma helper, that is the geometry actually observed
     * on C10 channel 6. Use a smaller transfer unit only for an unexpectedly
     * unaligned fragment. */
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

    c10_dma_channel_reset();

    c10_play_desc.dcmd = DMAC_DCMD_SAI | DMAC_DCMD_SWDH_32 |
        DMAC_DCMD_DWDH_16 | geometry | DMAC_DCMD_TIE;
    c10_play_desc.dsadr = PHYSADDR((unsigned long)addr);
    c10_play_desc.dtadr = PHYSADDR((unsigned long)AIC_DR);
    c10_play_desc.ddadr = count;
    c10_play_desc.dstrd = 0;
    c10_play_desc.dreqt = DMAC_DRSR_RS_AICOUT;
    c10_play_desc.reserved0 = 0;
    c10_play_desc.reserved1 = 0;
    commit_discard_dcache_range(&c10_play_desc, sizeof(c10_play_desc));

    REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) = DMAC_DCCSR_DES8;
    REG_DMAC_DRSR(DMA_AIC_TX_CHANNEL) = c10_play_desc.dreqt;
    REG_DMAC_DDA(DMA_AIC_TX_CHANNEL) =
        PHYSADDR((unsigned long)&c10_play_desc);
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

    command = DMAC_DCMD_SAI | DMAC_DCMD_SWDH_32 | burst_size |
              DMAC_DCMD_DWDH_16 | DMAC_DCMD_TIE;

    REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) = 0;
    REG_DMAC_DRSR(DMA_AIC_TX_CHANNEL)  = DMAC_DRSR_RS_AICOUT;
    REG_DMAC_DSAR(DMA_AIC_TX_CHANNEL)  = PHYSADDR((unsigned long)addr);
    REG_DMAC_DTAR(DMA_AIC_TX_CHANNEL)  = PHYSADDR((unsigned long)AIC_DR);
    REG_DMAC_DTCR(DMA_AIC_TX_CHANNEL)  = size;
    REG_DMAC_DCMD(DMA_AIC_TX_CHANNEL)  = command;
    REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) = DMAC_DCCSR_NDES | DMAC_DCCSR_EN;
#endif

    playback_address = addr;
}

static inline void play_dma_callback(void)
{
    const void *start;
    size_t size;

#ifdef COLORFLY_C10
    c10_audio_dma_buffer_complete();
#endif

    if (pcm_play_dma_complete_callback(PCM_DMAST_OK, &start, &size))
    {
        set_dma(start, size);
#ifdef COLORFLY_C10
        c10_dma_descriptor_start();
#else
        REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) |= DMAC_DCCSR_EN;
#endif
        pcm_play_dma_status_callback(PCM_DMAST_STARTED);
    }
}

void DMA_CALLBACK(DMA_AIC_TX_CHANNEL)(void) __attribute__ ((section(".icode")));
void DMA_CALLBACK(DMA_AIC_TX_CHANNEL)(void)
{
    unsigned int state = REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL);

#ifdef COLORFLY_C10
    const unsigned int controller = DMA_AIC_TX_CHANNEL / HALF_DMA_NUM;
    const unsigned int pending =
        1u << (DMA_AIC_TX_CHANNEL % HALF_DMA_NUM);

    /* Acknowledge terminal count before the callback can arm the next
     * transfer. Clearing the controller pending bit prevents a stale parent
     * interrupt from consuming that new buffer immediately. */
    REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) &=
        ~(DMAC_DCCSR_TT | DMAC_DCCSR_CT);
    REG_DMAC_DMAIPR(controller) &= ~pending;
#endif

    if (state & (DMAC_DCCSR_AR | DMAC_DCCSR_HLT |
                 DMAC_DCCSR_INV_COMPAT))
    {
        logf("PCM DMA error %08x", state);
#ifndef COLORFLY_C10
        REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) &=
            ~(DMAC_DCCSR_AR | DMAC_DCCSR_HLT | DMAC_DCCSR_INV_COMPAT);
#else
        REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) &= ~DMAC_DCCSR_INV_COMPAT;
#endif
        pcm_play_dma_complete_callback(PCM_DMAST_ERR_DMA, NULL, NULL);
        return;
    }

    if (state & DMAC_DCCSR_TT)
    {
        /* Descriptor mode raises TT only after the descriptor transfer count
         * has been consumed by AIC requests. Never accept DCCSR bit 1 here. */
#ifndef COLORFLY_C10
        REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) &=
            ~(DMAC_DCCSR_TT | DMAC_DCCSR_CT);
#endif
#ifdef COLORFLY_C10
        /* Defer callbacks while the PCM core is locked. */
        if (play_lock)
            c10_dma_completion_pending = true;
        else
            play_dma_callback();
#else
        play_dma_callback();
#endif
    }
}

static void sink_dma_stop(void)
{
#ifdef COLORFLY_C10
    c10_audio_stop_playback();
#endif
    int flags = disable_irq_save();

#ifdef COLORFLY_C10
    c10_dma_channel_reset();
    c10_dma_completion_pending = false;

    /* Match stock endpoint stop: stop requests and replay, but retain AIC and
     * ESCLK until audiohw_close(). Repeatedly disabling the controller loses
     * the external-clocked state between pause and restart. */
    REG_AIC_CR &= ~(AIC_CR_TDMS | AIC_CR_ERPL);
    REG_AIC_CR &= ~AIC_CR_TFLUSH;
    __aic_clear_errors();
#else
    REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) = (REG_DMAC_DCCSR(DMA_AIC_TX_CHANNEL) | DMAC_DCCSR_HLT) & ~DMAC_DCCSR_EN;
    __aic_disable_replay();
#endif

#ifdef COLORFLY_C10
    /* Stock keeps the reserved AIC DMA clock enabled after allocation. */
#else
    __dmac_channel_disable_clk(DMA_AIC_TX_CHANNEL);
#endif

    restore_irq(flags);
}

static void sink_dma_start(const void *addr, size_t size)
{
    sink_dma_stop();

#ifdef COLORFLY_C10
    c10_aic_dma_clock_enable();
#else
    __dmac_channel_enable_clk(DMA_AIC_TX_CHANNEL);
#endif

    set_dma(addr, size);

#ifdef COLORFLY_C10
    /* Stock wakes the DAC before arming the descriptor transfer. */
    c10_audio_start_playback();

    int flags = disable_irq_save();

    /* set_dma() installed one unlinked descriptor. TFLUSH must be
     * clear before the first request; stock does not flush at stream start. */
    REG_AIC_CR &= ~AIC_CR_TFLUSH;
    __aic_clear_errors();

    c10_dma_descriptor_start();
    /* Stock active captures have ETUR/EROR enabled at every source depth;
     * they do not select a wider output format. Rockbox PCM remains S16. */
    REG_AIC_CR |= AIC_CR_TDMS;
    REG_AIC_CR |= AIC_CR_ETUR | AIC_CR_EROR;
    REG_AIC_CR |= AIC_CR_ERPL;

    restore_irq(flags);

    c10_audio_output_started();
#else
    __aic_enable_replay();
    __dmac_channel_enable_irq(DMA_AIC_TX_CHANNEL);
#endif
}

static void sink_lock(void)
{
    int flags = disable_irq_save();

#ifdef COLORFLY_C10
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

#ifdef COLORFLY_C10
    if (--play_lock == 0 && c10_dma_completion_pending)
    {
        c10_dma_completion_pending = false;
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
