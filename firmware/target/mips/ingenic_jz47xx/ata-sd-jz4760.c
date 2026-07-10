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

#include "config.h"
#include "gcc_extensions.h"
#include "cpu.h"
#include "ata-sd-target.h"
#include "dma-target.h"
#include "led.h"
#include "sdmmc.h"
#include "disk.h"
#include "logf.h"
#include "storage.h"
#include "string.h"
#include "panic.h"

#define SD_INTERRUPT 0   // COMPLETELY BROKEN!
#define SD_DMA_ENABLE 1
#ifdef COLORFLY_C10
/* Poll MSC DMA completion. The legacy interrupt path shares one semaphore
 * with command/data-done events, allowing an early event to satisfy the
 * wrong wait during sustained playback. */
#define SD_DMA_INTERRUPT 0
#else
#define SD_DMA_INTERRUPT 1  // HANGS RANDOMLY!
#endif
#ifdef COLORFLY_C10
/* Stock enables MSC low-power mode and writes only START_OP for each command.
 * The controller then starts and stops the card clock around the operation;
 * explicitly combining CLOCK_START with START_OP is not the C10 sequence. */
#define SD_AUTO_CLOCK 1
#else
#define SD_AUTO_CLOCK 1
#endif

#if NUM_DRIVES > 2
#error "JZ4760 SD driver supports NUM_DRIVES <= 2 only"
#endif

static long               last_disk_activity = -1;
static tCardInfo          card[NUM_DRIVES];
static char               active[NUM_DRIVES];

#if defined(CONFIG_STORAGE_MULTI) || defined(HAVE_HOTSWAP)
static int                sd_drive_nr = 0;
#else
#define                   sd_drive_nr 0
#endif

static struct mutex       sd_mtx[NUM_DRIVES];
#if SD_DMA_INTERRUPT || SD_INTERRUPT
static struct semaphore   sd_wakeup[NUM_DRIVES];
#endif

static int                use_4bit[NUM_DRIVES];
static int                use_sbc[NUM_DRIVES];

//#define DEBUG(x...)         logf(x)
#define DEBUG(x, ...)

/* volumes */
#define SD_SLOT_1  0       /* SD card 1 */
#define SD_SLOT_2  1       /* SD card 2 */

#ifdef COLORFLY_C10
/* The C10 wires its removable microSD slot to MSC2 and internal eMMC to
 * MSC0. Match Rockbox's normal convention: internal storage is drive 0 and
 * removable storage is drive 1. */
#define MSC_CHN(n) ((n) == SD_SLOT_1 ? 0 : 2)
#else
#define MSC_CHN(n) (2-n)
#endif

static unsigned int jz_sd_dma_request(const int drive, const bool write)
{
    switch (MSC_CHN(drive)) {
    case 0:
        return write ? DMAC_DRSR_RS_MSC0OUT : DMAC_DRSR_RS_MSC0IN;
    case 1:
        return write ? DMAC_DRSR_RS_MSC1OUT : DMAC_DRSR_RS_MSC1IN;
    default:
        return write ? DMAC_DRSR_RS_MSC2OUT : DMAC_DRSR_RS_MSC2IN;
    }
}

#ifdef COLORFLY_C10
/* The stock C10 kernel uses JZ4760 8-word descriptors for MSC DMA. Keep each
 * descriptor on its own cache line: the DMA engine fetches it directly from
 * SDRAM. The transfer unit is selected separately for every buffer. */
struct c10_msc_dma_desc {
    volatile unsigned int dcmd;
    volatile unsigned int dsadr;
    volatile unsigned int dtadr;
    volatile unsigned int ddadr;
    volatile unsigned int dstrd;
    volatile unsigned int dreqt;
    volatile unsigned int reserved0;
    volatile unsigned int reserved1;
};

static struct c10_msc_dma_desc c10_rx_desc[NUM_DRIVES] CACHEALIGN_ATTR;
static struct c10_msc_dma_desc c10_tx_desc[NUM_DRIVES] CACHEALIGN_ATTR;
static unsigned char c10_emmc_ext_csd[SD_BLOCK_SIZE] CACHEALIGN_ATTR;
static unsigned char c10_emmc_mbr[SD_BLOCK_SIZE] CACHEALIGN_ATTR;
static sector_t c10_emmc_part4_start;
static sector_t c10_emmc_part4_end;

/* jz4760b.h was originally imported without the JZ4760B descriptor-invalid
 * status bit. It is present in both Ingenic headers and the C10 stock kernel. */
#define C10_DMAC_DCCSR_INV (1u << 6)

static unsigned int c10_msc_dma_geometry(const void *buffer,
                                         unsigned int bytes,
                                         unsigned int *count)
{
    const unsigned long address = (unsigned long)buffer;

    /* The stock scatter-DMA builder uses 32-bit descriptors for an
     * unaligned head/tail and larger units only where both the memory
     * address and length have the required alignment. A 64-byte unit at a
     * merely word-aligned Rockbox file buffer makes DMAC halt with AR. */
    if (((address | bytes) & 63) == 0) {
        *count = bytes >> 6;
        return DMAC_DCMD_DS_64BYTE;
    }
    if (((address | bytes) & 31) == 0) {
        *count = bytes >> 5;
        return DMAC_DCMD_DS_32BYTE;
    }
    if (((address | bytes) & 15) == 0) {
        *count = bytes >> 4;
        return DMAC_DCMD_DS_16BYTE;
    }

    *count = bytes >> 2;
    return DMAC_DCMD_DS_32BIT;
}

static void c10_msc_dma_stop(const int channel)
{
    const int controller = channel / HALF_DMA_NUM;
    const unsigned int pending = 1u << (channel % HALF_DMA_NUM);

    /* Match jz_mmc_stop_dma() from the stock kernel.  In particular, do not
     * leave CT/HLT or a terminal-descriptor interrupt pending for the next
     * command: the card can otherwise report UNDERRUN on the following R1. */
    REG_DMAC_DCCSR(channel) = 0;
    REG_DMAC_DCMD(channel) = 0;
    REG_DMAC_DSAR(channel) = 0;
    REG_DMAC_DTAR(channel) = 0;
    REG_DMAC_DTCR(channel) = 0;
    REG_DMAC_DRSR(channel) = 0;
    REG_DMAC_DDA(channel) = 0;
    REG_DMAC_DMAIPR(controller) &= ~pending;
    REG_DMAC_DMACR(controller) &= ~(DMAC_DMACR_HLT | DMAC_DMACR_AR);
}

#endif

#define SD_IRQ_MASK(n)                \
do {                                  \
          REG_MSC_IMASK(n) = 0xffff;  \
          REG_MSC_IREG(n) = 0xffff;   \
} while (0)

/* Error codes */
enum sd_result_t
{
    SD_NO_RESPONSE        = -1,
    SD_NO_ERROR           = 0,
    SD_ERROR_OUT_OF_RANGE,
    SD_ERROR_ADDRESS,
    SD_ERROR_BLOCK_LEN,
    SD_ERROR_ERASE_SEQ,
    SD_ERROR_ERASE_PARAM,
    SD_ERROR_WP_VIOLATION,
    SD_ERROR_CARD_IS_LOCKED,
    SD_ERROR_LOCK_UNLOCK_FAILED,
    SD_ERROR_COM_CRC,
    SD_ERROR_ILLEGAL_COMMAND,
    SD_ERROR_CARD_ECC_FAILED,
    SD_ERROR_CC,
    SD_ERROR_GENERAL,
    SD_ERROR_UNDERRUN,
    SD_ERROR_OVERRUN,
    SD_ERROR_CID_CSD_OVERWRITE,
    SD_ERROR_STATE_MISMATCH,
    SD_ERROR_HEADER_MISMATCH,
    SD_ERROR_TIMEOUT,
    SD_ERROR_CRC,
    SD_ERROR_DRIVER_FAILURE,
    SD_ERROR_ACMD_REJECT,
};

/* Standard MMC/SD clock speeds */
#define MMC_CLOCK_SLOW    400000      /* 400 kHz for initial setup */
#ifdef COLORFLY_C10
/* The stock C10 supplies MSC2 from a 24 MHz parent, so normal and
 * high-speed operation both top out at 24 MHz on this board. */
#define SD_CLOCK_FAST   24000000
#define SD_CLOCK_HIGH   24000000
#else
#define SD_CLOCK_FAST   24000000      /* 24 MHz for SD Cards */
#define SD_CLOCK_HIGH   48000000      /* 48 MHz for SD Cards */
#endif

#ifdef COLORFLY_C10
/* Stock C10 eMMC initialization values, also used by the validated RetroFW
 * MSC0 implementation. The device remains in legacy 4-bit SDR mode. */
#define C10_MMC_SEND_OP_COND          1
#define C10_MMC_SEND_EXT_CSD          8
#define C10_MMC_OCR_ARG               0x40ff8000
#define C10_MMC_RCA                   (1u << 16)
#define C10_MMC_SWITCH_BUS_WIDTH_ARG  0x03b70101
#define C10_MMC_EXT_CSD_SEC_COUNT     212
#endif

/* Proprietary commands, illegal/reserved according to SD Specification 2.00 */
    /* class 1 */
#define SD_READ_DAT_UNTIL_STOP  11   /* adtc [31:0]  dadr       R1  */

    /* class 3 */
#define SD_WRITE_DAT_UNTIL_STOP 20   /* adtc [31:0]  data addr  R1  */

    /* class 4 */
#define SD_PROGRAM_CID          26   /* adtc                    R1  */
#define SD_PROGRAM_CSD          27   /* adtc                    R1  */

    /* class 9 */
#define SD_GO_IRQ_STATE         40   /* bcr                     R5  */

enum sd_rsp_t
{
    RESPONSE_NONE    = 0,
    RESPONSE_R1      = 1,
    RESPONSE_R1B     = 2,
    RESPONSE_R2      = 3,
    RESPONSE_R3      = 4,
    RESPONSE_R4      = 5,
    RESPONSE_R5      = 6,
    RESPONSE_R6      = 7,
    RESPONSE_R7      = 8,
};

/* These are unpacked versions of the actual responses */
struct sd_response_r1
{
    unsigned char  cmd;
    unsigned int   status;
};

struct sd_response_r3
{
    unsigned int ocr;
};

#define SD_CARD_BUSY    0x80000000    /* Card Power up status bit */

struct sd_request
{
    int               cmd;        /* Command to send */
    unsigned int      arg;        /* Argument to send */
    enum sd_rsp_t     rtype;      /* Response type expected */

    /* Data transfer (these may be modified at the low level) */
    unsigned short    nob;        /* Number of blocks to transfer*/
    unsigned short    block_len;  /* Block length */
    unsigned char     *buffer;    /* Data buffer */
    unsigned int      cnt;        /* Data length, for PIO */

    /* Results */
    unsigned char     response[18]; /* Buffer to store response - CRC is optional */
    enum sd_result_t result;
};

/**************************************************************************
 * Utility functions
 **************************************************************************/

#define PARSE_U32(_buf,_index) \
    (((unsigned int)_buf[_index]) << 24) | (((unsigned int)_buf[_index+1]) << 16) | \
        (((unsigned int)_buf[_index+2]) << 8) | ((unsigned int)_buf[_index+3]);

#define PARSE_U16(_buf,_index) \
    (((unsigned short)_buf[_index]) << 8) | ((unsigned short)_buf[_index+1]);

static int sd_unpack_r1(struct sd_request *request, struct sd_response_r1 *r1)
{
    unsigned char *buf = request->response;

    if (request->result)
        return request->result;

    r1->cmd    = buf[0];
    r1->status = PARSE_U32(buf,1);

    DEBUG("sd_unpack_r1: cmd=%d status=%08x", r1->cmd, r1->status);

    if (SD_R1_STATUS(r1->status)) {
        if (r1->status & SD_R1_OUT_OF_RANGE)       return SD_ERROR_OUT_OF_RANGE;
        if (r1->status & SD_R1_ADDRESS_ERROR)      return SD_ERROR_ADDRESS;
        if (r1->status & SD_R1_BLOCK_LEN_ERROR)    return SD_ERROR_BLOCK_LEN;
        if (r1->status & SD_R1_ERASE_SEQ_ERROR)    return SD_ERROR_ERASE_SEQ;
        if (r1->status & SD_R1_ERASE_PARAM)        return SD_ERROR_ERASE_PARAM;
        if (r1->status & SD_R1_WP_VIOLATION)       return SD_ERROR_WP_VIOLATION;
        //if (r1->status & SD_R1_CARD_IS_LOCKED)     return SD_ERROR_CARD_IS_LOCKED;
        if (r1->status & SD_R1_LOCK_UNLOCK_FAILED) return SD_ERROR_LOCK_UNLOCK_FAILED;
        if (r1->status & SD_R1_COM_CRC_ERROR)      return SD_ERROR_COM_CRC;
        if (r1->status & SD_R1_ILLEGAL_COMMAND)    return SD_ERROR_ILLEGAL_COMMAND;
        if (r1->status & SD_R1_CARD_ECC_FAILED)    return SD_ERROR_CARD_ECC_FAILED;
        if (r1->status & SD_R1_CC_ERROR)           return SD_ERROR_CC;
        if (r1->status & SD_R1_ERROR)              return SD_ERROR_GENERAL;
        if (r1->status & SD_R1_UNDERRUN)           return SD_ERROR_UNDERRUN;
        if (r1->status & SD_R1_OVERRUN)            return SD_ERROR_OVERRUN;
        if (r1->status & SD_R1_CSD_OVERWRITE)      return SD_ERROR_CID_CSD_OVERWRITE;
    }

    if (buf[0] != request->cmd)
        return SD_ERROR_HEADER_MISMATCH;

    /* This should be last - it's the least dangerous error */

    return SD_NO_ERROR;
}

static int sd_unpack_r6(struct sd_request *request, struct sd_response_r1 *r1, unsigned long *rca)
{
    unsigned char *buf = request->response;

    if (request->result)
        return request->result;

    if (buf[0] != 0x03)
        return SD_ERROR_HEADER_MISMATCH;

    *rca = PARSE_U16(buf,1);  /* Save RCA returned by the SD Card */

    *(buf+1) = 0;
    *(buf+2) = 0;

    return sd_unpack_r1(request, r1);
}

static int sd_unpack_r3(struct sd_request *request, struct sd_response_r3 *r3)
{
    unsigned char *buf = request->response;

    if (request->result) return request->result;

    r3->ocr = PARSE_U32(buf,1);
    DEBUG("sd_unpack_r3: ocr=%08x", r3->ocr);

    if (buf[0] != 0x3f)
        return SD_ERROR_HEADER_MISMATCH;

    return SD_NO_ERROR;
}

/* Stop the MMC clock and wait while it happens */
static inline int jz_sd_stop_clock(const int drive)
{
    register int timeout = 1000;

    //DEBUG("stop MMC clock");
#if SD_AUTO_CLOCK
    REG_MSC_LPM(MSC_CHN(drive)) = 0; /* disable auto clock stop */
#endif

    /* only stop if necessary */
    if (!(REG_MSC_STAT(MSC_CHN(drive)) & MSC_STAT_CLK_EN))
       return SD_NO_ERROR;

    REG_MSC_STRPCL(MSC_CHN(drive)) = MSC_STRPCL_CLOCK_CONTROL_STOP;

    while (timeout && (REG_MSC_STAT(MSC_CHN(drive)) & MSC_STAT_CLK_EN))
    {
        timeout--;
        if (timeout == 0)
        {
            logf("Timeout on stop clock waiting");
            return SD_ERROR_TIMEOUT;
        }
        udelay(1);
    }

    //DEBUG("clock off time is %d microsec", timeout);
    return SD_NO_ERROR;
}

/* Start the MMC clock and operation */
static inline int jz_sd_start_clock(const int drive)
{
    int reg = MSC_STRPCL_START_OP;
#if !SD_AUTO_CLOCK
    reg |= (REG_MSC_STAT(MSC_CHN(drive)) & MSC_STAT_CLK_EN) ? 0 : MSC_STRPCL_CLOCK_CONTROL_START;
#endif
    REG_MSC_STRPCL(MSC_CHN(drive)) = reg;
    return SD_NO_ERROR;
}

static int jz_sd_check_status(const int drive, struct sd_request *request)
{
    (void)request;
    unsigned int status = REG_MSC_STAT(MSC_CHN(drive));

    /* A response timeout cannot safely enter Rockbox's synchronous data
     * phase: unlike Linux, this driver has no asynchronous request state
     * machine to unwind a command that never produced a usable response. */
    if (status & (MSC_STAT_TIME_OUT_RES | MSC_STAT_TIME_OUT_READ))
    {
        logf("%d SD timeout, MSC_STAT 0x%x CMD %d", drive, status,
               request->cmd);
        return SD_ERROR_TIMEOUT;
    }

    /* Checking for CRC error */
    if (status &
        (MSC_STAT_CRC_READ_ERROR | MSC_STAT_CRC_WRITE_ERROR |
         MSC_STAT_CRC_RES_ERR))
    {
#ifdef COLORFLY_C10
        /* JZ4760's MMC path can falsely report response CRC for CID/CSD.
         * The working C10 RetroFW path ignores precisely these R2 errors;
         * retain all data CRC and all other response CRC checks. */
        if (drive == SD_SLOT_1 && request->rtype == RESPONSE_R2 &&
            (status & (MSC_STAT_CRC_READ_ERROR | MSC_STAT_CRC_WRITE_ERROR |
                       MSC_STAT_CRC_RES_ERR)) == MSC_STAT_CRC_RES_ERR)
            return SD_NO_ERROR;
#endif
        logf("SD CRC error, MSC_STAT 0x%x", status);
        return SD_ERROR_CRC;
    }

    /* Checking for FIFO empty */
    /*if(status & MSC_STAT_DATA_FIFO_EMPTY && request->rtype != RESPONSE_NONE)
    {
        logf("SD FIFO empty, MSC_STAT 0x%x", status);
        return SD_ERROR_UNDERRUN;
    }*/

    return SD_NO_ERROR;
}

/* Obtain response to the command and store it to response buffer */
static void jz_sd_get_response(const int drive, struct sd_request *request)
{
    int i;
    unsigned char *buf;
    unsigned short data;

    if (request->result != SD_NO_RESPONSE)
        return;

    DEBUG("fetch response for request %d, cmd %d", request->rtype,
          request->cmd);
    buf = request->response;
    request->result = SD_NO_ERROR;

    switch (request->rtype) {
        case RESPONSE_R1:
        case RESPONSE_R1B:
        case RESPONSE_R3:
        case RESPONSE_R4:
        case RESPONSE_R5:
        case RESPONSE_R6:
        case RESPONSE_R7:
            data = REG_MSC_RES(MSC_CHN(drive));
            buf[0] = (data >> 8) & 0xff;
            buf[1] = data & 0xff;
            data = REG_MSC_RES(MSC_CHN(drive));
            buf[2] = (data >> 8) & 0xff;
            buf[3] = data & 0xff;
            data = REG_MSC_RES(MSC_CHN(drive));
            buf[4] = data & 0xff;

            DEBUG("%d request %d, response [%02x %02x %02x %02x %02x]", drive,
                  request->rtype, buf[0], buf[1], buf[2],
                  buf[3], buf[4]);
            break;
        case RESPONSE_R2:
            for (i = 0; i < 16; i += 2)
            {
                data = REG_MSC_RES(MSC_CHN(drive));
                buf[i] = (data >> 8) & 0xff;
                buf[i + 1] = data & 0xff;
            }
            DEBUG("request %d, response []", request->rtype);
            break;
        case RESPONSE_NONE:
            DEBUG("No response");
            break;

        default:
            DEBUG("unhandled response type for request %d",
                  request->rtype);
            break;
    }
}

#if SD_DMA_ENABLE
static bool jz_sd_use_dma(const struct sd_request *req)
{
#ifdef COLORFLY_C10
    return ((unsigned long)req->buffer & 0x3) == 0 && req->cnt >= 512 &&
           (req->cnt & 63) == 0;
#else
    return ((unsigned long)req->buffer & 0x3) == 0 && req->cnt >= 512;
#endif
}

static int jz_sd_transmit_data_dma(const int drive, struct sd_request *req);
static void jz_sd_receive_data_dma_start(const int drive,
                                         struct sd_request *req);
static int jz_sd_receive_data_dma(const int drive, struct sd_request *req);
#endif

static int jz_sd_receive_data(const int drive, struct sd_request *req)
{
    unsigned int nob = req->nob;
    unsigned int wblocklen = (unsigned int) (req->block_len + 3) >> 2;    /* length in word */
    unsigned char *buf = req->buffer;
    unsigned int *wbuf = (unsigned int *) buf;
    unsigned int waligned = (((unsigned int) buf & 0x3) == 0);    /* word aligned ? */
    unsigned int stat, data, cnt;

#if SD_DMA_ENABLE
    /* Use DMA if we can */
    /* Parenthesize the alignment test: equality has higher precedence than
     * bitwise AND, so `ptr & 3 == 0` is always false and silently disables
     * DMA. This matters on the C10 because PIO cannot reliably service MSC2
     * while high-rate AIC DMA interrupts are active. */
    if (jz_sd_use_dma(req))
        return jz_sd_receive_data_dma(drive, req);
#endif

    for (; nob >= 1; nob--)
    {
        long deadline = current_tick + (HZ * 65);

        do {
            stat = REG_MSC_STAT(MSC_CHN(drive));

            if (stat & MSC_STAT_TIME_OUT_READ)
                return SD_ERROR_TIMEOUT;
            else if (stat & MSC_STAT_CRC_READ_ERROR)
                return SD_ERROR_CRC;
            else if ((stat & MSC_STAT_DATA_FIFO_AFULL) ||
                     !(stat & MSC_STAT_DATA_FIFO_EMPTY))
                /* Ready to read data */
                break;

            yield();
        } while (TIME_BEFORE(current_tick, deadline));

        /* Read data from RXFIFO. It could be FULL or PARTIAL FULL */
        DEBUG("Receive Data = %d", wblocklen);
        cnt = wblocklen;
        while (cnt)
        {
            if (REG_MSC_STAT(MSC_CHN(drive)) & MSC_STAT_DATA_FIFO_EMPTY)
            {
                if (TIME_AFTER(current_tick, deadline))
                    return SD_ERROR_TIMEOUT;
                continue;
            }

            data = REG_MSC_RXFIFO(MSC_CHN(drive));
            if (waligned)
                *wbuf++ = data;
            else
            {
                *buf++ = (unsigned char) (data >> 0);
                *buf++ = (unsigned char) (data >> 8);
                *buf++ = (unsigned char) (data >> 16);
                *buf++ = (unsigned char) (data >> 24);
            }
            cnt--;
        }
    }

    return SD_NO_ERROR;
}

static int jz_sd_transmit_data(const int drive, struct sd_request *req)
{
    unsigned int nob = req->nob;
    unsigned int wblocklen = (unsigned int) (req->block_len + 3) >> 2;    /* length in word */
    unsigned char *buf = req->buffer;
    unsigned int *wbuf = (unsigned int *) buf;
    unsigned int waligned = (((unsigned int) buf & 0x3) == 0);    /* word aligned ? */
    unsigned int stat, data, cnt;

#if SD_DMA_ENABLE
    /* Use DMA if we can */
    if (jz_sd_use_dma(req))
        return jz_sd_transmit_data_dma(drive, req);
#endif

    for (; nob >= 1; nob--)
    {
        long deadline = current_tick + (HZ * 65);

        do {
            stat = REG_MSC_STAT(MSC_CHN(drive));

            if (stat &
                (MSC_STAT_CRC_WRITE_ERROR |
                 MSC_STAT_CRC_WRITE_ERROR_NOSTS))
                return SD_ERROR_CRC;
            else if (!(stat & MSC_STAT_DATA_FIFO_FULL))
                /* Ready to write data */
                break;

            yield();
        } while (TIME_BEFORE(current_tick, deadline));

        /* Write data to TXFIFO */
        cnt = wblocklen;
        while (cnt)
        {
            if (REG_MSC_STAT(MSC_CHN(drive)) & MSC_STAT_DATA_FIFO_FULL)
            {
                if (TIME_AFTER(current_tick, deadline))
                    return SD_ERROR_TIMEOUT;
                continue;
            }

            if (waligned)
                REG_MSC_TXFIFO(MSC_CHN(drive)) = *wbuf++;
            else
            {
                data = *buf++;
                data |= *buf++ << 8;
                data |= *buf++ << 16;
                data |= *buf++ << 24;
                REG_MSC_TXFIFO(MSC_CHN(drive)) = data;
            }

            cnt--;
        }
    }

    return SD_NO_ERROR;
}

#if SD_DMA_ENABLE
static void jz_sd_receive_data_dma_start(const int drive,
                                         struct sd_request *req)
{
    /* Stock arms read DMA before starting the MSC command. This also keeps
     * the FIFO from filling while the destination cache is invalidated. */
    discard_dcache_range(req->buffer, req->cnt);

    /* setup dma channel */
#ifdef COLORFLY_C10
    c10_msc_dma_stop(DMA_SD_RX_CHANNEL(drive));
    struct c10_msc_dma_desc *desc = &c10_rx_desc[drive];
    unsigned int count;
    unsigned int geometry = c10_msc_dma_geometry(req->buffer, req->cnt,
                                                  &count);

    /* Use one unlinked 8-word descriptor. DMAC0 remains masked at INTC;
     * Rockbox polls TT and acknowledges the terminal interrupt pending bit. */
    desc->dcmd = DMAC_DCMD_DAI | DMAC_DCMD_SWDH_32 |
                 DMAC_DCMD_DWDH_32 | geometry | DMAC_DCMD_TIE;
    desc->dsadr = PHYSADDR(MSC_RXFIFO(MSC_CHN(drive)));
    desc->dtadr = PHYSADDR((unsigned long)req->buffer);
    desc->ddadr = count;
    desc->dstrd = 0;
    desc->dreqt = jz_sd_dma_request(drive, false);
    desc->reserved0 = 0;
    desc->reserved1 = 0;
    commit_discard_dcache_range(desc, sizeof(*desc));

    REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(drive)) = DMAC_DCCSR_DES8;
    REG_DMAC_DRSR(DMA_SD_RX_CHANNEL(drive)) = desc->dreqt;
    REG_DMAC_DDA(DMA_SD_RX_CHANNEL(drive)) = PHYSADDR((unsigned long)desc);
    REG_DMAC_DMADBSR(DMA_SD_RX_CHANNEL(drive) / HALF_DMA_NUM) =
        1u << (DMA_SD_RX_CHANNEL(drive) % HALF_DMA_NUM);
    REG_DMAC_DMACR(DMA_SD_RX_CHANNEL(drive) / HALF_DMA_NUM) |=
        DMAC_DMACR_DMAE | DMAC_DMACR_FMSC;
    REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(drive)) |= DMAC_DCCSR_EN;
#else
    REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(drive)) = 0;
    REG_DMAC_DSAR(DMA_SD_RX_CHANNEL(drive)) = PHYSADDR(MSC_RXFIFO(MSC_CHN(drive))); /* DMA source addr */
    REG_DMAC_DTAR(DMA_SD_RX_CHANNEL(drive)) = PHYSADDR((unsigned long)req->buffer); /* DMA dest addr */
    /* Direct/non-descriptor mode follows Ingenic's generic JZ4760 driver and
     * counts 32-bit units. Stock uses 64-byte units only in its linked
     * descriptor path; using that encoding directly left the C10 DMA idle. */
    REG_DMAC_DTCR(DMA_SD_RX_CHANNEL(drive)) = (req->cnt + 3) >> 2;
    REG_DMAC_DRSR(DMA_SD_RX_CHANNEL(drive)) =
        jz_sd_dma_request(drive, false);

    REG_DMAC_DCMD(DMA_SD_RX_CHANNEL(drive)) =
#if SD_DMA_INTERRUPT
        DMAC_DCMD_TIE | /* Enable DMA interrupt */
#endif
        DMAC_DCMD_DAI | DMAC_DCMD_SWDH_32 | DMAC_DCMD_DWDH_32 |
        DMAC_DCMD_DS_32BIT;
    REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(drive)) = DMAC_DCCSR_EN | DMAC_DCCSR_NDES;
#endif
}

static int jz_sd_receive_data_dma(const int drive, struct sd_request *req)
{

    /* wait for dma completion */
#if SD_DMA_INTERRUPT
    semaphore_wait(&sd_wakeup[drive], TIMEOUT_BLOCK);
#else
    {
        long deadline = current_tick + HZ * 5;
        unsigned int state;
        do {
            state = REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(drive));
#ifdef COLORFLY_C10
            /* TT is authoritative even if a controller-wide sticky flag was
             * left by another channel. Channel INV/AR/HLT are actual faults. */
            if (state & DMAC_DCCSR_TT)
                break;
            if (state & (C10_DMAC_DCCSR_INV | DMAC_DCCSR_AR |
                         DMAC_DCCSR_HLT)) {
                c10_msc_dma_stop(DMA_SD_RX_CHANNEL(drive));
#else
            unsigned int controller = DMA_SD_RX_CHANNEL(drive) / HALF_DMA_NUM;
            if ((state & DMAC_DCCSR_AR) ||
                (REG_DMAC_DMACR(controller) &
                 (DMAC_DMACR_AR | DMAC_DMACR_HLT))) {
                REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(drive)) = 0;
#endif
                return SD_ERROR_DRIVER_FAILURE;
            }
            unsigned int stat = REG_MSC_STAT(MSC_CHN(drive));
            if (stat & MSC_STAT_TIME_OUT_READ) {
#ifdef COLORFLY_C10
                c10_msc_dma_stop(DMA_SD_RX_CHANNEL(drive));
#else
                REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(drive)) = 0;
#endif
                return SD_ERROR_TIMEOUT;
            }
            if (stat & MSC_STAT_CRC_READ_ERROR) {
#ifdef COLORFLY_C10
                c10_msc_dma_stop(DMA_SD_RX_CHANNEL(drive));
#else
                REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(drive)) = 0;
#endif
                return SD_ERROR_CRC;
            }
#ifndef COLORFLY_C10
            /* Direct mode is complete when the programmed count reaches
             * zero. The C10 can report MSC DATA_TRAN_DONE without latching
             * DCCSR.TT when this channel is polled with interrupts disabled. */
            if (REG_DMAC_DTCR(DMA_SD_RX_CHANNEL(drive)) == 0)
                break;
#endif
            yield();
        } while (TIME_BEFORE(current_tick, deadline));

#ifdef COLORFLY_C10
        if (!(REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(drive)) & DMAC_DCCSR_TT)) {
#else
        if (REG_DMAC_DTCR(DMA_SD_RX_CHANNEL(drive)) != 0) {
#endif
#ifdef COLORFLY_C10
            c10_msc_dma_stop(DMA_SD_RX_CHANNEL(drive));
#else
            REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(drive)) = 0;
#endif
            return SD_ERROR_TIMEOUT;
        }
    }
#endif

    /* clear status and disable channel */
#ifdef COLORFLY_C10
    c10_msc_dma_stop(DMA_SD_RX_CHANNEL(drive));
#else
    REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(drive)) = 0;
#endif

    /* DMA completion only describes the memory transfer. The MSC can still
     * report a card-side timeout or CRC failure, as the stock driver's data
     * completion path does. */
    unsigned int stat = REG_MSC_STAT(MSC_CHN(drive));
    if (stat & MSC_STAT_TIME_OUT_READ)
        return SD_ERROR_TIMEOUT;
    if (stat & MSC_STAT_CRC_READ_ERROR)
        return SD_ERROR_CRC;

    /* flush dcache again */
    discard_dcache_range(req->buffer, req->cnt);

    return SD_NO_ERROR;
}

static int jz_sd_transmit_data_dma(const int drive, struct sd_request *req)
{
    /* flush dcache */
    commit_discard_dcache_range(req->buffer, req->cnt);

    /* setup dma channel */
#ifdef COLORFLY_C10
    c10_msc_dma_stop(DMA_SD_TX_CHANNEL(drive));
    struct c10_msc_dma_desc *desc = &c10_tx_desc[drive];
    unsigned int count;
    unsigned int geometry = c10_msc_dma_geometry(req->buffer, req->cnt,
                                                  &count);

    desc->dcmd = DMAC_DCMD_SAI | DMAC_DCMD_SWDH_32 |
                 DMAC_DCMD_DWDH_32 | geometry | DMAC_DCMD_TIE;
    desc->dsadr = PHYSADDR((unsigned long)req->buffer);
    desc->dtadr = PHYSADDR(MSC_TXFIFO(MSC_CHN(drive)));
    desc->ddadr = count;
    desc->dstrd = 0;
    desc->dreqt = jz_sd_dma_request(drive, true);
    desc->reserved0 = 0;
    desc->reserved1 = 0;
    commit_discard_dcache_range(desc, sizeof(*desc));

    REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(drive)) = DMAC_DCCSR_DES8;
    REG_DMAC_DRSR(DMA_SD_TX_CHANNEL(drive)) = desc->dreqt;
    REG_DMAC_DDA(DMA_SD_TX_CHANNEL(drive)) = PHYSADDR((unsigned long)desc);
    REG_DMAC_DMADBSR(DMA_SD_TX_CHANNEL(drive) / HALF_DMA_NUM) =
        1u << (DMA_SD_TX_CHANNEL(drive) % HALF_DMA_NUM);
    REG_DMAC_DMACR(DMA_SD_TX_CHANNEL(drive) / HALF_DMA_NUM) |=
        DMAC_DMACR_DMAE | DMAC_DMACR_FMSC;
    REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(drive)) |= DMAC_DCCSR_EN;
#else
    REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(drive)) = 0;
    REG_DMAC_DSAR(DMA_SD_TX_CHANNEL(drive)) = PHYSADDR((unsigned long) req->buffer); /* DMA source addr */
    REG_DMAC_DTAR(DMA_SD_TX_CHANNEL(drive)) = PHYSADDR(MSC_TXFIFO(MSC_CHN(drive)));  /* DMA dest addr */
    REG_DMAC_DTCR(DMA_SD_TX_CHANNEL(drive)) = (req->cnt + 3) >> 2;
    REG_DMAC_DRSR(DMA_SD_TX_CHANNEL(drive)) =
        jz_sd_dma_request(drive, true);

    REG_DMAC_DCMD(DMA_SD_TX_CHANNEL(drive)) =
#if SD_DMA_INTERRUPT
        DMAC_DCMD_TIE | /* Enable DMA interrupt */
#endif
        DMAC_DCMD_SAI | DMAC_DCMD_SWDH_32 | DMAC_DCMD_DWDH_32 |
        DMAC_DCMD_DS_32BIT;
    REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(drive)) = DMAC_DCCSR_EN | DMAC_DCCSR_NDES;
#endif

    /* wait for dma completion */
#if SD_DMA_INTERRUPT
    semaphore_wait(&sd_wakeup[drive], TIMEOUT_BLOCK);
#else
    {
        long deadline = current_tick + HZ * 5;
        unsigned int state;
        do {
            state = REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(drive));
#ifdef COLORFLY_C10
            if (state & DMAC_DCCSR_TT)
                break;
            if (state & (C10_DMAC_DCCSR_INV | DMAC_DCCSR_AR |
                         DMAC_DCCSR_HLT)) {
                c10_msc_dma_stop(DMA_SD_TX_CHANNEL(drive));
#else
            unsigned int controller = DMA_SD_TX_CHANNEL(drive) / HALF_DMA_NUM;
            if ((state & DMAC_DCCSR_AR) ||
                (REG_DMAC_DMACR(controller) &
                 (DMAC_DMACR_AR | DMAC_DMACR_HLT))) {
                REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(drive)) = 0;
#endif
                return SD_ERROR_DRIVER_FAILURE;
            }
#ifndef COLORFLY_C10
            if (REG_DMAC_DTCR(DMA_SD_TX_CHANNEL(drive)) == 0)
                break;
#endif
            yield();
        } while (TIME_BEFORE(current_tick, deadline));

#ifdef COLORFLY_C10
        if (!(REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(drive)) & DMAC_DCCSR_TT)) {
#else
        if (REG_DMAC_DTCR(DMA_SD_TX_CHANNEL(drive)) != 0) {
#endif
#ifdef COLORFLY_C10
            c10_msc_dma_stop(DMA_SD_TX_CHANNEL(drive));
#else
            REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(drive)) = 0;
#endif
            return SD_ERROR_TIMEOUT;
        }
    }
#endif

    /* clear status and disable channel */
#ifdef COLORFLY_C10
    c10_msc_dma_stop(DMA_SD_TX_CHANNEL(drive));
#else
    REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(drive)) = 0;
#endif

    unsigned int stat = REG_MSC_STAT(MSC_CHN(drive));
    if (stat & MSC_STAT_TIME_OUT_READ)
        return SD_ERROR_TIMEOUT;
    if (stat & (MSC_STAT_CRC_WRITE_ERROR |
                MSC_STAT_CRC_WRITE_ERROR_NOSTS))
        return SD_ERROR_CRC;

    return SD_NO_ERROR;
}

#if SD_DMA_INTERRUPT
void DMA_CALLBACK(DMA_SD_RX_CHANNEL0)(void)
{
    if (REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(SD_SLOT_1)) & DMAC_DCCSR_AR)
    {
        REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(SD_SLOT_1)) &= ~DMAC_DCCSR_AR;
        panicf("SD RX DMA address error");
    }

    if (REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(SD_SLOT_1)) & DMAC_DCCSR_HLT)
    {
        REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(SD_SLOT_1)) &= ~DMAC_DCCSR_HLT;
        panicf("SD RX DMA halt");
    }

    if (REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(SD_SLOT_1)) & DMAC_DCCSR_TT)
    {
        REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(SD_SLOT_1)) &= ~DMAC_DCCSR_TT;
        //sd_rx_dma_callback();
        semaphore_release(&sd_wakeup[SD_SLOT_1]);
    }
}

void DMA_CALLBACK(DMA_SD_RX_CHANNEL1)(void)
{
    if (REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(SD_SLOT_2)) & DMAC_DCCSR_AR)
    {
        REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(SD_SLOT_2)) &= ~DMAC_DCCSR_AR;
        panicf("SD RX DMA address error");
    }

    if (REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(SD_SLOT_2)) & DMAC_DCCSR_HLT)
    {
        REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(SD_SLOT_2)) &= ~DMAC_DCCSR_HLT;
        panicf("SD RX DMA halt");
    }

    if (REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(SD_SLOT_2)) & DMAC_DCCSR_TT)
    {
        REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(SD_SLOT_2)) &= ~DMAC_DCCSR_TT;
        //sd_rx_dma_callback();
        semaphore_release(&sd_wakeup[SD_SLOT_2]);
    }
}

void DMA_CALLBACK(DMA_SD_TX_CHANNEL0)(void)
{
    if (REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(SD_SLOT_1)) & DMAC_DCCSR_AR)
    {
        REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(SD_SLOT_1)) &= ~DMAC_DCCSR_AR;
        panicf("SD TX DMA address error");
    }

    if (REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(SD_SLOT_1)) & DMAC_DCCSR_HLT)
    {
        REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(SD_SLOT_1)) &= ~DMAC_DCCSR_HLT;
        panicf("SD TX DMA halt");
    }

    if (REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(SD_SLOT_1)) & DMAC_DCCSR_TT)
    {
        REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(SD_SLOT_1)) &= ~DMAC_DCCSR_TT;
        //sd_tx_dma_callback();
        semaphore_release(&sd_wakeup[SD_SLOT_1]);
    }
}

void DMA_CALLBACK(DMA_SD_TX_CHANNEL1)(void)
{
    if (REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(SD_SLOT_2)) & DMAC_DCCSR_AR)
    {
        REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(SD_SLOT_2)) &= ~DMAC_DCCSR_AR;
        panicf("SD TX DMA address error");
    }

    if (REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(SD_SLOT_2)) & DMAC_DCCSR_HLT)
    {
        REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(SD_SLOT_2)) &= ~DMAC_DCCSR_HLT;
        panicf("SD TX DMA halt");
    }

    if (REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(SD_SLOT_2)) & DMAC_DCCSR_TT)
    {
        REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL(SD_SLOT_2)) &= ~DMAC_DCCSR_TT;
        //sd_tx_dma_callback();
	semaphore_release(&sd_wakeup[SD_SLOT_2]);
    }
}
#endif                /* SD_DMA_INTERRUPT */
#endif                /* SD_DMA_ENABLE */

#ifndef HAVE_ADJUSTABLE_CPU_FREQ
#define cpu_frequency __cpm_get_pllout2()
#endif

static inline unsigned int jz_sd_calc_clkrt(const int drive, unsigned int rate)
{
    unsigned int clkrt = 0;
    /* MSCCDR encodes divisor-minus-one.  Using the raw field made C10's
     * 48 MHz MSC source look like 72 MHz, producing clkrt=8; CLKRT only has
     * three rate bits, so that wrapped to 0 and clocked card init at full rate.
     */
    unsigned int clk_src = __cpm_get_mscclk();

    if (!card[drive].sd2plus && rate > SD_CLOCK_FAST)
        rate = SD_CLOCK_FAST;

    while (rate < clk_src)
    {
        clkrt++;
        clk_src >>= 1;
    }
    return clkrt;
}

/* Set the MMC clock frequency */
static void jz_sd_set_clock(const int drive, unsigned int rate)
{
    int clkrt;

    clkrt = jz_sd_calc_clkrt(drive, rate);
    REG_MSC_CLKRT(MSC_CHN(drive)) = clkrt;

    DEBUG("set clock to %u Hz clkrt=%d", rate, clkrt);
}

/********************************************************************************************************************
** Name:      int jz_sd_exec_cmd()
** Function:  send command to the card, and get a response
** Input:     struct sd_request *req: SD request
** Output:    0:  right        >0:  error code
********************************************************************************************************************/
static int jz_sd_exec_cmd(const int drive, struct sd_request *request)
{
#ifdef COLORFLY_C10
    /* The stock C10 kernel binary ORs 0x5000 into every CMDAT. These are the
     * 16-word receive/transmit FIFO thresholds used by its MSC DMA path. */
    unsigned int cmdat = MSC_CMDAT_RTRG_EQUALT_16 |
                         MSC_CMDAT_TTRG_LESS_16;
#else
    unsigned int cmdat = 0;
#endif
    int retval;
#if !SD_INTERRUPT
    long deadline = current_tick + (HZ * 5);
#endif
    bool has_data = (request->buffer != NULL);
#if SD_DMA_ENABLE
    bool rx_dma_armed = false;
#endif

    /* Indicate we have no result yet */
    request->result = SD_NO_RESPONSE;

#if !SD_AUTO_CLOCK
    /* Stop clock when programming things */
    jz_sd_stop_clock(drive); /* stop SD clock */
#endif

    /* Generic handling for all requests with data rx/tx */
    if (has_data) {
        cmdat |= MSC_CMDAT_DATA_EN;
#if SD_DMA_ENABLE
        /* Keep the controller's DMA mode in lockstep with the software path.
         * An unaligned buffer falls back to PIO and must not set DMA_EN. */
        if (jz_sd_use_dma(request))
            cmdat |= MSC_CMDAT_DMA_EN;
#endif
    }

    /* mask all interrupts and clear status */
    SD_IRQ_MASK(MSC_CHN(drive));

#if SD_INTERRUPT
    /* unmask interrupts we care about */
    REG_MSC_IMASK(MSC_CHN(drive)) = ~(MSC_IMASK_END_CMD_RES | MSC_IMASK_DATA_TRAN_DONE | MSC_IMASK_PRG_DONE);
#endif

    /* Per-command type handling */
    switch (request->cmd)
    {
            /* bc - broadcast - no response */
        case SD_GO_IDLE_STATE:
            cmdat |= MSC_CMDAT_INIT; /* Initialization sequence sent prior to command */
            /* Intentional Fallthrough */

        case SD_SET_DSR:
            break;

            /* bcr - broadcast with response */
        case SD_APP_OP_COND:
        case SD_ALL_SEND_CID:
        case SD_GO_IRQ_STATE:
            break;

            /* adtc - addressed with data transfer */
        case SD_SEND_SCR:
        case SD_READ_DAT_UNTIL_STOP:
        case SD_READ_SINGLE_BLOCK:
        case SD_SWITCH_FUNC:
            /* These READ data */
            break;

        case SD_READ_MULTIPLE_BLOCK:
#ifdef COLORFLY_C10
            cmdat |= MSC_CMDAT_SEND_AS_STOP;
#endif
            break;

        case SD_WRITE_DAT_UNTIL_STOP:
        case SD_WRITE_BLOCK:
        case SD_PROGRAM_CID:
        case SD_PROGRAM_CSD:
                /* These WRITE data */
            cmdat |= MSC_CMDAT_WRITE;
            break;

        case SD_WRITE_MULTIPLE_BLOCK:
#ifdef COLORFLY_C10
            cmdat |= MSC_CMDAT_SEND_AS_STOP;
#endif
            cmdat |= MSC_CMDAT_WRITE;
            break;

//        case SD_LOCK_UNLOCK:  // can be a read or write, needs busy too.

        case SD_STOP_TRANSMISSION:
            // cmdat |= MSC_CMDAT_STOP_ABORT;
            break;

        /* ac - no data transfer */
        default:
            break;
    }

    /* Set response type */
    switch (request->rtype)
    {
        case RESPONSE_NONE:
            break;
        case RESPONSE_R1B:
            cmdat |= MSC_CMDAT_BUSY;
            /* FALLTHRU */
        case RESPONSE_R1:
            cmdat |= MSC_CMDAT_RESPONSE_R1;
            break;
        case RESPONSE_R2:
            cmdat |= MSC_CMDAT_RESPONSE_R2;
            break;
        case RESPONSE_R3:
            cmdat |= MSC_CMDAT_RESPONSE_R3;
            break;
        case RESPONSE_R4:
            cmdat |= MSC_CMDAT_RESPONSE_R4;
            break;
        case RESPONSE_R5:
            cmdat |= MSC_CMDAT_RESPONSE_R5;
            break;
        case RESPONSE_R6:
            cmdat |= MSC_CMDAT_RESPONSE_R6;
            break;
        case RESPONSE_R7:
            cmdat |= MSC_CMDAT_RESPONSE_R7;
            break;
        default:
            break;
    }

    /* use 4-bit bus width when possible */
    if (use_4bit[drive])
        cmdat |= MSC_CMDAT_BUS_WIDTH_4BIT;

    /* Set block length and nob if there is data */
    if (has_data) {
        REG_MSC_BLKLEN(MSC_CHN(drive)) = request->block_len;
        REG_MSC_NOB(MSC_CHN(drive)) = request->nob;
    }

    /* Set command index */
    REG_MSC_CMD(MSC_CHN(drive)) = request->cmd;

    /* Set argument */
    REG_MSC_ARG(MSC_CHN(drive)) = request->arg;

    /* Set command */
    REG_MSC_CMDAT(MSC_CHN(drive)) = cmdat;

    DEBUG("%d Send cmd %d cmdat: %x arg: %x resp %d", drive, request->cmd,
          cmdat, request->arg, request->rtype);

    /* Stock jz_mmc_execute_cmd reloads the response timeout for every
     * command. A controller reset establishes the same value initially,
     * but commands are allowed to update it. */
    REG_MSC_RESTO(MSC_CHN(drive)) = 0xff;

#if SD_DMA_ENABLE
    /* The stock C10 driver arms read DMA before launching the command. Write
     * DMA is armed after the response, when the MSC starts requesting data. */
    if (has_data && !(cmdat & MSC_CMDAT_WRITE) && jz_sd_use_dma(request)) {
        jz_sd_receive_data_dma_start(drive, request);
        rx_dma_armed = true;
    }
#endif

    /* Start SD clock and send command to card */
    jz_sd_start_clock(drive);

    /* Wait for command completion */
#if SD_INTERRUPT
    semaphore_wait(&sd_wakeup[drive], HZ * 5);
#else
#ifdef COLORFLY_C10
    /* Stock waits on the controller status, including terminal error bits;
     * IREG is an interrupt latch and is not its command-completion source. */
    while (!(REG_MSC_STAT(MSC_CHN(drive)) &
             (MSC_STAT_END_CMD_RES | MSC_STAT_TIME_OUT_RES |
              MSC_STAT_CRC_RES_ERR)))
#else
    while (!(REG_MSC_IREG(MSC_CHN(drive)) & MSC_IREG_END_CMD_RES))
#endif
    {
        if (TIME_AFTER(current_tick, deadline)) {
#if SD_DMA_ENABLE
            if (rx_dma_armed)
#ifdef COLORFLY_C10
                c10_msc_dma_stop(DMA_SD_RX_CHANNEL(drive));
#else
                REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(drive)) = 0;
#endif
#endif
            return SD_ERROR_TIMEOUT;
        }
        yield();
    }
#endif

    /* Check for status */
    retval = jz_sd_check_status(drive, request);
    REG_MSC_IREG(MSC_CHN(drive)) = MSC_IREG_END_CMD_RES;    /* clear flag */
    if (retval) {
#if SD_DMA_ENABLE
        if (rx_dma_armed)
#ifdef COLORFLY_C10
            c10_msc_dma_stop(DMA_SD_RX_CHANNEL(drive));
#else
            REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL(drive)) = 0;
#endif
#endif
        return retval;
    }

    /* Complete command with no response */
    if (request->rtype == RESPONSE_NONE)
        return SD_NO_ERROR;

    /* Get response */
    jz_sd_get_response(drive, request);

    /* Start data operation */
    if (has_data) {
        if (cmdat & MSC_CMDAT_WRITE) {
            retval = jz_sd_transmit_data(drive, request);
        } else {
            retval = jz_sd_receive_data(drive, request);
        }
        if (retval)
            return retval;

#if SD_INTERRUPT
        semaphore_wait(&sd_wakeup[drive], HZ * 5);
#else
        /* Wait for Data Done */
        deadline = current_tick + (HZ * 5);
#ifdef COLORFLY_C10
        /* Stock's synchronous path verifies DATA_TRAN_DONE in STAT. IREG is
         * only the latch cleared after the terminal controller state. */
        while (!(REG_MSC_STAT(MSC_CHN(drive)) & MSC_STAT_DATA_TRAN_DONE)) {
#else
        while (!(REG_MSC_IREG(MSC_CHN(drive)) & MSC_IREG_DATA_TRAN_DONE)) {
#endif
            retval = jz_sd_check_status(drive, request);
            if (retval)
                return retval;
            if (TIME_AFTER(current_tick, deadline))
                return SD_ERROR_TIMEOUT;
            yield();
        }
        REG_MSC_IREG(MSC_CHN(drive)) = MSC_IREG_DATA_TRAN_DONE;    /* clear status */
#endif

#ifdef COLORFLY_C10
        if (cmdat & MSC_CMDAT_SEND_AS_STOP) {
            deadline = current_tick + (HZ * 5);
            while (!(REG_MSC_STAT(MSC_CHN(drive)) &
                     (MSC_STAT_AUTO_CMD_DONE | MSC_STAT_TIME_OUT_READ |
                      MSC_STAT_CRC_READ_ERROR | MSC_STAT_CRC_WRITE_ERROR))) {
                if (TIME_AFTER(current_tick, deadline))
                    return SD_ERROR_TIMEOUT;
                yield();
            }

            retval = jz_sd_check_status(drive, request);
            if (retval)
                return retval;
            REG_MSC_IREG(MSC_CHN(drive)) = MSC_IREG_AUTO_CMD_DONE;
            REG_MSC_CMDAT(MSC_CHN(drive)) &= ~MSC_CMDAT_SEND_AS_STOP;
        }
#endif

    }

    /* Wait for Prog Done event if necessary */
    if (cmdat & (MSC_CMDAT_BUSY | MSC_CMDAT_WRITE))
    {
#if SD_INTERRUPT
        semaphore_wait(&sd_wakeup[drive], HZ * 5);
#else
        deadline = current_tick + (HZ * 5);
        while (!(REG_MSC_IREG(MSC_CHN(drive)) & MSC_IREG_PRG_DONE)) {
            retval = jz_sd_check_status(drive, request);
            if (retval)
                return retval;
            if (TIME_AFTER(current_tick, deadline))
                return SD_ERROR_TIMEOUT;
            yield();
        }
        REG_MSC_IREG(MSC_CHN(drive)) = MSC_IREG_PRG_DONE;    /* clear status */
#endif
    }

    /* Command completed */
#if !SD_AUTO_CLOCK
    jz_sd_stop_clock(drive); /* stop SD clock */
#endif

    return SD_NO_ERROR;    /* return successfully */
}

/*******************************************************************************************************************
** Name:      int sd_chkcard()
** Function:  check whether card is insert entirely
** Input:     NULL
** Output:    1: insert entirely    0: not insert entirely
********************************************************************************************************************/
#if !defined(COLORFLY_C10) || defined(HAVE_HOTSWAP)
static int jz_sd_chkcard(const int drive)
{
    return (__gpio_get_pin((drive == SD_SLOT_1) ? PIN_SD1_CD : PIN_SD2_CD) == 0 ? 1 : 0);
}
#endif

/* MSC interrupt handlers */
#if SD_INTERRUPT
void MSC2(void)  /* SD_SLOT_1 */
{
    DEBUG("MSC2 interrupt");

    if (REG_MSC_IREG(MSC_CHN(SD_SLOT_1)) & MSC_IREG_END_CMD_RES) {
        REG_MSC_IREG(MSC_CHN(SD_SLOT_1)) = MSC_IREG_END_CMD_RES;    /* clear flag */
        semaphore_release(&sd_wakeup[SD_SLOT_1]);
    }
    if (REG_MSC_IREG(MSC_CHN(SD_SLOT_1)) & MSC_IREG_PRG_DONE) {
        REG_MSC_IREG(MSC_CHN(SD_SLOT_1)) = MSC_IREG_PRG_DONE;    /* clear flag */
        semaphore_release(&sd_wakeup[SD_SLOT_1]);
    }
    if (REG_MSC_IREG(MSC_CHN(SD_SLOT_1)) & MSC_IREG_DATA_TRAN_DONE) {
        REG_MSC_IREG(MSC_CHN(SD_SLOT_1)) = MSC_IREG_DATA_TRAN_DONE;    /* clear flag */
        semaphore_release(&sd_wakeup[SD_SLOT_1]);
    }
}

/* MSC interrupt handlers */
void MSC1(void) /* SD_SLOT_2 */
{
    DEBUG("MSC1 interrupt");
    if (REG_MSC_IREG(MSC_CHN(SD_SLOT_2)) & MSC_IREG_END_CMD_RES) {
        REG_MSC_IREG(MSC_CHN(SD_SLOT_2)) = MSC_IREG_END_CMD_RES;    /* clear flag */
        semaphore_release(&sd_wakeup[SD_SLOT_2]);
    }
    if (REG_MSC_IREG(MSC_CHN(SD_SLOT_2)) & MSC_IREG_PRG_DONE) {
        REG_MSC_IREG(MSC_CHN(SD_SLOT_2)) = MSC_IREG_PRG_DONE;    /* clear flag */
        semaphore_release(&sd_wakeup[SD_SLOT_2]);
    }
    if (REG_MSC_IREG(MSC_CHN(SD_SLOT_2)) & MSC_IREG_DATA_TRAN_DONE) {
        REG_MSC_IREG(MSC_CHN(SD_SLOT_2)) = MSC_IREG_DATA_TRAN_DONE;    /* clear flag */
        semaphore_release(&sd_wakeup[SD_SLOT_2]);
    }
}
#endif

#ifdef HAVE_HOTSWAP
static void sd_gpio_setup_irq(const int drive, bool inserted)
{
#ifdef COLORFLY_C10
    /* Only drive 1 is removable. Stock uses active-low PB20 and switches
     * between rising-edge removal and falling-edge insertion interrupts. */
    if (drive != SD_SLOT_2)
        return;
#endif

    int pin = (drive == SD_SLOT_1) ? PIN_SD1_CD : PIN_SD2_CD;
    int irq = (drive == SD_SLOT_1) ? IRQ_SD1_CD : IRQ_SD2_CD;
    if(inserted)
        __gpio_as_irq_rise_edge(pin);
    else
        __gpio_as_irq_fall_edge(pin);
    system_enable_irq(irq);
}
#endif

/*******************************************************************************************************************
** Name:      void sd_hardware_init()
** Function:  initialize the hardware condiction that access sd card
** Input:     NULL
** Output:    NULL
********************************************************************************************************************/
static void jz_sd_hardware_init(const int drive)
{
    const int channel = MSC_CHN(drive);

#ifdef COLORFLY_C10
    /* C10 stock Linux/U-Boot use a 24 MHz MSC parent clock. The standard
     * helper programs MSCCDR's divisor-minus-one field correctly. */
    __cpm_select_msc_clk(1);
#endif

    switch (channel) {
    case 0:
        __cpm_start_msc0();
        break;
    case 1:
        __cpm_start_msc1();
        break;
    default:
        __cpm_start_msc2();
        break;
    }
#ifdef HAVE_HOTSWAP
    sd_gpio_setup_irq(drive, jz_sd_chkcard(drive));
#endif
    __msc_reset(channel);  /* reset mmc/sd controller */

    /* Match stock jz_mmc_reset(): reset leaves these controller defaults
     * unspecified, and its request engine assumes the following values. */
    REG_MSC_LPM(channel) = MSC_SET_LPM;
    SD_IRQ_MASK(channel);
    REG_MSC_RDTO(channel) = 0xffffffff;
    REG_MSC_RESTO(channel) = 0xff;
#if SD_AUTO_CLOCK
#ifndef COLORFLY_C10
    REG_MSC_STRPCL(channel) = MSC_STRPCL_CLOCK_CONTROL_START; /* Enable clocks */
#endif
#else
    jz_sd_stop_clock(drive); /* stop SD clock */
#endif
    jz_sd_set_clock(drive, MMC_CLOCK_SLOW); /* Drop to lowest speed */
}

static void sd_send_cmd(const int drive, struct sd_request *request, int cmd, unsigned int arg,
                         unsigned short nob, unsigned short block_len,
                         enum sd_rsp_t rtype, unsigned char* buffer)
{
    int retval;

    request->cmd = cmd;
    request->arg = arg;
    request->rtype = rtype;
    request->nob = nob;
    request->block_len = block_len;
    request->buffer = buffer;
    request->cnt = nob * block_len;
    memset(&request->response, 0xa5, sizeof(request->response));
    request->result = SD_NO_RESPONSE;

    retval = jz_sd_exec_cmd(drive, request);
    if (retval)
        request->result = retval;

}

static void sd_simple_cmd(const int drive, struct sd_request *request, int cmd, unsigned int arg,
                           enum sd_rsp_t rtype)
{
    sd_send_cmd(drive, request, cmd, arg, 0, 0, rtype, NULL);
}

#ifdef COLORFLY_C10
static int c10_mmc_init_card(const int drive)
{
    struct sd_request request = {0};
    struct sd_response_r1 r1;
    struct sd_response_r3 r3;
    long deadline;
    int retval;

    sd_simple_cmd(drive, &request, SD_GO_IDLE_STATE, 0, RESPONSE_NONE);

    deadline = current_tick + HZ;
    do {
        sd_simple_cmd(drive, &request, C10_MMC_SEND_OP_COND,
                      C10_MMC_OCR_ARG, RESPONSE_R3);
        retval = sd_unpack_r3(&request, &r3);
        if (retval)
            return retval;

        card[drive].ocr = r3.ocr;
        if (r3.ocr & SD_CARD_BUSY)
            break;

        sleep(HZ / 100);
    } while (TIME_BEFORE(current_tick, deadline));

    if (!(card[drive].ocr & SD_CARD_BUSY))
        return SD_ERROR_TIMEOUT;

    sd_simple_cmd(drive, &request, SD_ALL_SEND_CID, 0, RESPONSE_R2);
    if (request.result)
        return request.result;
    for (int i = 0; i < 4; i++)
        card[drive].cid[i] = PARSE_U32(request.response, 1 + i * 4);

    card[drive].rca = C10_MMC_RCA;
    sd_simple_cmd(drive, &request, SD_SEND_RELATIVE_ADDR,
                  card[drive].rca, RESPONSE_R1);
    retval = sd_unpack_r1(&request, &r1);
    if (retval)
        return retval;

    sd_simple_cmd(drive, &request, SD_SEND_CSD,
                  card[drive].rca, RESPONSE_R2);
    if (request.result)
        return request.result;
    for (int i = 0; i < 4; i++)
        card[drive].csd[i] = PARSE_U32(request.response, 1 + i * 4);

    sd_simple_cmd(drive, &request, SD_SELECT_CARD,
                  card[drive].rca, RESPONSE_R1B);
    retval = sd_unpack_r1(&request, &r1);
    if (retval)
        return retval;

    /* MMC SWITCH: write EXT_CSD BUS_WIDTH=1, selecting four data lines.
     * This exact argument is present in the stock-compatible C10 path. */
    sd_simple_cmd(drive, &request, SD_SWITCH_FUNC,
                  C10_MMC_SWITCH_BUS_WIDTH_ARG, RESPONSE_R1B);
    retval = sd_unpack_r1(&request, &r1);
    if (retval)
        return retval;
    use_4bit[drive] = 2;

    memset(c10_emmc_ext_csd, 0, sizeof(c10_emmc_ext_csd));
    sd_send_cmd(drive, &request, C10_MMC_SEND_EXT_CSD, 0, 1,
                sizeof(c10_emmc_ext_csd), RESPONSE_R1,
                c10_emmc_ext_csd);
    retval = sd_unpack_r1(&request, &r1);
    if (retval)
        return retval;

    card[drive].numblocks =
        (sector_t)c10_emmc_ext_csd[C10_MMC_EXT_CSD_SEC_COUNT] |
        ((sector_t)c10_emmc_ext_csd[C10_MMC_EXT_CSD_SEC_COUNT + 1] << 8) |
        ((sector_t)c10_emmc_ext_csd[C10_MMC_EXT_CSD_SEC_COUNT + 2] << 16) |
        ((sector_t)c10_emmc_ext_csd[C10_MMC_EXT_CSD_SEC_COUNT + 3] << 24);
    if (card[drive].numblocks == 0)
        return SD_ERROR_DRIVER_FAILURE;

    card[drive].blocksize = SD_BLOCK_SIZE;
    /* The shared transfer path uses sd2plus to select sector addressing.
     * EXT_CSD SEC_COUNT eMMC devices use the same 512-byte sector argument. */
    card[drive].sd2plus = 1;
    jz_sd_set_clock(drive, SD_CLOCK_FAST);

    /* The first three eMMC partitions contain the stock boot chain and
     * system. Validate the on-device MBR and remember the exact partition-4
     * interval; every later eMMC write is rejected unless it stays wholly
     * inside this FAT32 partition. */
    c10_emmc_part4_start = 0;
    c10_emmc_part4_end = 0;
    memset(c10_emmc_mbr, 0, sizeof(c10_emmc_mbr));
    sd_send_cmd(drive, &request, SD_READ_SINGLE_BLOCK, 0, 1,
                sizeof(c10_emmc_mbr), RESPONSE_R1, c10_emmc_mbr);
    retval = sd_unpack_r1(&request, &r1);
    if (retval)
        return retval;

    const unsigned int entry = 446 + 3 * 16;
    const unsigned int type = c10_emmc_mbr[entry + 4];
    const sector_t start =
        (sector_t)c10_emmc_mbr[entry + 8] |
        ((sector_t)c10_emmc_mbr[entry + 9] << 8) |
        ((sector_t)c10_emmc_mbr[entry + 10] << 16) |
        ((sector_t)c10_emmc_mbr[entry + 11] << 24);
    const sector_t size =
        (sector_t)c10_emmc_mbr[entry + 12] |
        ((sector_t)c10_emmc_mbr[entry + 13] << 8) |
        ((sector_t)c10_emmc_mbr[entry + 14] << 16) |
        ((sector_t)c10_emmc_mbr[entry + 15] << 24);

    if (c10_emmc_mbr[510] != 0x55 || c10_emmc_mbr[511] != 0xaa ||
        (type != PARTITION_TYPE_FAT32 &&
         type != PARTITION_TYPE_FAT32_LBA) ||
        start == 0 || size == 0 || start >= card[drive].numblocks ||
        size > card[drive].numblocks - start)
        return SD_ERROR_DRIVER_FAILURE;

    c10_emmc_part4_start = start;
    c10_emmc_part4_end = start + size;
    card[drive].initialized = 1;

    logf("eMMC ready: %lu sectors",
         (unsigned long)card[drive].numblocks);
    return SD_NO_ERROR;
}
#endif

static int sd_exec_acmd(const int drive, struct sd_request *request, int cmd, unsigned int arg)
{
    struct sd_response_r1 r1 = {0};
    int retval;

    sd_simple_cmd(drive, request, SD_APP_CMD, card[drive].rca, RESPONSE_R1);
    retval = sd_unpack_r1(request, &r1);
    if (retval)
        return retval;

    if (!(r1.status & SD_R1_APP_CMD)) {
        logf("ACMD not accepted\n");
        return SD_ERROR_ACMD_REJECT;
    }

    sd_simple_cmd(drive, request, cmd, arg, RESPONSE_R1);
    return sd_unpack_r1(request, &r1);
}

static int sd_exec_acmd_buf(const int drive, struct sd_request *request, int cmd, unsigned int arg, enum sd_rsp_t rtype, unsigned short nob, unsigned short block_len, unsigned char *buffer)
{
    struct sd_response_r1 r1;
    int retval;

    sd_simple_cmd(drive, request, SD_APP_CMD, card[drive].rca, RESPONSE_R1);
    retval = sd_unpack_r1(request, &r1);
    if (retval)
        return retval;

    if (!(r1.status & SD_R1_APP_CMD)) {
        logf("ACMD not accepted\n");
        return SD_ERROR_ACMD_REJECT;
    }

    sd_send_cmd(drive, request, cmd, arg,
                nob, block_len, rtype, buffer);
    return SD_NO_ERROR;
}

#define SD_INIT_DOING   0
#define SD_INIT_PASSED  1
#define SD_INIT_FAILED  2
static int sd_init_card_state(const int drive, struct sd_request *request)
{
    struct sd_response_r1 r1;
    struct sd_response_r3 r3;
    int retval, i, ocr = 0x40300000; /* SDXC, 3.2-3.4v */

#ifdef HAVE_SDUC
    ocr |= 0x08000000; /* Add on SDUC */
#endif

    switch (request->cmd)
    {
        case SD_GO_IDLE_STATE: /* No response to parse */
            sd_simple_cmd(drive, request, SD_SEND_IF_COND, 0x1AA, RESPONSE_R1);
            break;

        case SD_SEND_IF_COND:
            retval = sd_unpack_r1(request, &r1);
//            if (retval)
//                return SD_INIT_FAILED;

            retval = sd_exec_acmd_buf(drive, request, SD_APP_OP_COND, ocr,
                                      RESPONSE_R3, 0, 0, NULL);
            if (retval)
                return SD_INIT_FAILED;
            break;
        case SD_APP_OP_COND:
            retval = sd_unpack_r3(request, &r3);
            if (retval)
                return SD_INIT_FAILED;

            DEBUG("%d: read ocr value = 0x%08x", drive, r3.ocr);
            card[drive].ocr = r3.ocr;

            // XXX for SDUC, check that B27+B30 of OCR are set.
            if(!(r3.ocr & SD_CARD_BUSY || ocr == 0))
            {
                /* Re-issue AP_OP_COND */
                sleep(HZ / 100);
                retval = sd_exec_acmd_buf(drive, request, SD_APP_OP_COND, ocr,
                                          RESPONSE_R3, 0, 0, NULL);
                if (retval)
                    return SD_INIT_FAILED;
            }
            else
            {
                sd_simple_cmd(drive, request, SD_ALL_SEND_CID, 0, RESPONSE_R2);
            }
            break;
        case SD_ALL_SEND_CID:
            if (request->result)
                return SD_INIT_FAILED;

            for(i=0; i<4; i++)
                card[drive].cid[i] = ((request->response[1+i*4]<<24) | (request->response[2+i*4]<<16) |
                               (request->response[3+i*4]<< 8) | request->response[4+i*4]);

            logf("%d CID: %08lx%08lx%08lx%08lx", drive, card[drive].cid[0], card[drive].cid[1], card[drive].cid[2], card[drive].cid[3]);
            sd_simple_cmd(drive, request, SD_SEND_RELATIVE_ADDR, 0, RESPONSE_R6);
            break;

        case SD_SEND_RELATIVE_ADDR:
            retval = sd_unpack_r6(request, &r1, &card[drive].rca);
            DEBUG("sd_init_card_state: Get RCA from SD: 0x%04lx Status: %x", card[drive].rca, r1.status);
            card[drive].rca = card[drive].rca << 16;
            if (retval)
            {
                logf("sd_init_card_state: unable to SET_RELATIVE_ADDR error=%d",
                      retval);
                return SD_INIT_FAILED;
            }

            sd_simple_cmd(drive, request, SD_SEND_CSD, card[drive].rca, RESPONSE_R2);
            break;

        case SD_SEND_CSD:
            if (request->result)
                return SD_INIT_FAILED;
            for(i=0; i<4; i++)
                card[drive].csd[i] = ((request->response[1+i*4]<<24) | (request->response[2+i*4]<<16) |
                               (request->response[3+i*4]<< 8) | request->response[4+i*4]);

            sd_parse_csd(&card[drive]);

            logf("%d CSD: %08lx%08lx%08lx%08lx", drive, card[drive].csd[0], card[drive].csd[1], card[drive].csd[2], card[drive].csd[3]);

#if 0  // XXX SCR is not working yet
            retval = sd_exec_acmd_buf(drive, request, SD_SEND_SCR, 0,
                                      RESPONSE_R1,
                                      1, 8, &request->response[5]);
            if (!retval)
                retval = sd_unpack_r1(request, &r1);
            // XXX check retval?
            break;
        case SD_SEND_SCR:
//            if (request->result)
//                return SD_INIT_FAILED;
            for(i=0; i<2; i++)
                card[drive].scr[i] = ((request->buffer[0+i*4]<<24) | (request->buffer[1+i*4]<<16) |
                                      (request->buffer[2+i*4]<< 8) | request->buffer[3+i*4]);

            DEBUG("%d SCR: %08lx%08lx", drive, card[drive].scr[0], card[drive].scr[1]);

            use_sbc[drive] = card[drive].scr[0] & 2;
#endif

            logf("SD card %d is ready", drive);
            jz_sd_set_clock(drive, SD_CLOCK_FAST);
            return SD_INIT_PASSED;
        default:
            logf("sd_init_card_state: error!  Illegal last cmd %d", request->cmd);
            return SD_INIT_FAILED;
    }

    return SD_INIT_DOING;
}

static int sd_switch(const int drive, struct sd_request *request, int mode, int group,
              unsigned char value, unsigned char * resp)
{
    unsigned int arg;

    mode = !!mode;
    value &= 0xF;
    arg = (mode << 31 | 0x00FFFFFF);
    if (mode) {
        group--;
        arg &= ~(0xF << (group * 4));
        arg |= value << (group * 4);
    }
    sd_send_cmd(drive, request, SD_SWITCH_FUNC, arg, 1, 64, RESPONSE_R1, resp);

    return 0;
}

/*
 * Fetches and decodes switch information
 */
static int sd_read_switch(const int drive, struct sd_request *request)
{
    unsigned int status[64 / 4];

    memset((unsigned char *)status, 0, 64);
    sd_switch(drive, request, 0, 0, 1, (unsigned char*) status);

    if (((unsigned char *)status)[13] & 0x02)
        return 0;
    else
        return 1;
}

/*
 * Test if the card supports high-speed mode and, if so, switch to it.
 */
static int sd_switch_hs(const int drive, struct sd_request *request)
{
    unsigned int status[64 / 4];

    sd_switch(drive, request, 1, 1, 1, (unsigned char*) status);
    return 0;
}

static int sd_select_card(const int drive)
{
    struct sd_request request = {0};
    struct sd_response_r1 r1;
    int retval;

    sd_simple_cmd(drive, &request, SD_SELECT_CARD, card[drive].rca,
               RESPONSE_R1B);
    retval = sd_unpack_r1(&request, &r1);
    if (retval)
        return retval;

    if (card[drive].sd2plus)
    {
        retval = sd_read_switch(drive, &request);
        if (!retval)
        {
            sd_switch_hs(drive, &request);
            jz_sd_set_clock(drive, SD_CLOCK_HIGH);
        }
    }

    bool sup_4bit = true; // XXX check against SCR

    if (sup_4bit) {
        retval = sd_exec_acmd(drive, &request, SD_SET_BUS_WIDTH, 2);
        if (retval)
            return retval;
        use_4bit[drive] = 2;
        DEBUG("Use 4-bit bus width");
    }
    retval = sd_exec_acmd(drive, &request, SD_SET_CLR_CARD_DETECT, 0);
    if (retval)
        return retval;

    card[drive].initialized = 1;

    return 0;
}

static inline bool card_detect_target(const int drive)
{
#ifdef COLORFLY_C10
    /* eMMC is soldered down; stock Linux confirms PB20 is active-low card
     * detect for the removable MSC2 microSD slot. */
    return drive == SD_SLOT_1 || jz_sd_chkcard(drive);
#else
    return (jz_sd_chkcard(drive) == 1);
#endif
}

static int __sd_init_device(const int drive)
{
    int retval = 0;
    long deadline;
    struct sd_request init_req;

    /* Initialise card data as blank */
    memset(&card[drive], 0, sizeof(tCardInfo));

    use_4bit[drive] = 0;
    use_sbc[drive] = 0;
    active[drive] = 0;

#ifdef COLORFLY_C10
    if (drive == SD_SLOT_2) {
        /* Reassert stock lepus_tf_power_on() for every retry, then leave the
         * removable-card rail stable before issuing its first command. */
        REG_GPIO_PXDATC(1) = (1u << 30);
        sleep(HZ / 50);
    }
#endif

    /* reset mmc/sd controller */
    jz_sd_hardware_init(drive);

    /* Don't bother if card isn't present */
    if (!card_detect_target(drive)) {
        return 0;
    }

#ifdef COLORFLY_C10
    if (drive == SD_SLOT_1) {
        retval = c10_mmc_init_card(drive);
        __cpm_stop_msc0();
        return retval;
    }
#endif

    /* Stock JZ4760B U-Boot stops any inherited transfer before CMD0. */
#ifdef COLORFLY_C10
    sd_simple_cmd(drive, &init_req, SD_STOP_TRANSMISSION, 0, RESPONSE_R1B);
#endif

    /* CMD0 starts the card's protocol state machine. */
    sd_simple_cmd(drive, &init_req, SD_GO_IDLE_STATE, 0,     RESPONSE_NONE);

    sleep(HZ/2); /* Give the card/controller some rest */

    deadline = current_tick + HZ;
    do {
       retval = sd_init_card_state(drive, &init_req);
    } while (TIME_BEFORE(current_tick, deadline) && (retval == SD_INIT_DOING));

    if (retval == SD_INIT_PASSED) {
        retval = sd_select_card(drive);
    } else {
        retval = -1;
    }

    if (drive == SD_SLOT_1) {
#ifdef COLORFLY_C10
        __cpm_stop_msc0();
#else
        __cpm_stop_msc2(); /* disable SD1 clock */
#endif
    } else {
#ifndef COLORFLY_C10
        __cpm_stop_msc1(); /* disable SD2 clock */
#endif
    }

    return retval;
}

int sd_init(void)
{
    static bool inited = false;

    sd_init_gpio();     /* init GPIO */

#if SD_DMA_ENABLE
#ifdef COLORFLY_C10
    /* Plain JZ4760 DMACKE is a write command and reads back as zero. The
     * stock C10 uses DMAC0 channel 2 for both MSC2 directions. Drive 0 uses
     * channel 3 for MSC0, keeping both storage paths off AIC's controller. */
    REG_DMAC_DMACKE(0) = (1u << DMA_SD_RX_CHANNEL(SD_SLOT_1)) |
                         (1u << DMA_SD_RX_CHANNEL(SD_SLOT_2));
#else
    __dmac_channel_enable_clk(DMA_SD_RX_CHANNEL(SD_SLOT_1));
    __dmac_channel_enable_clk(DMA_SD_RX_CHANNEL(SD_SLOT_2));
    __dmac_channel_enable_clk(DMA_SD_TX_CHANNEL(SD_SLOT_1));
    __dmac_channel_enable_clk(DMA_SD_TX_CHANNEL(SD_SLOT_2));
#endif
#endif

    if(!inited)
    {
#ifdef COLORFLY_C10
        __cpm_stop_msc1();   /* C10 uses MSC2 and MSC0 */
#else
        __cpm_stop_msc0();   /* We don't use MSC0 */
#endif

#if SD_DMA_INTERRUPT || SD_INTERRUPT
        semaphore_init(&sd_wakeup[SD_SLOT_1], 1, 0);
        semaphore_init(&sd_wakeup[SD_SLOT_2], 1, 0);
#endif
        mutex_init(&sd_mtx[SD_SLOT_1]);
        mutex_init(&sd_mtx[SD_SLOT_2]);

#if SD_INTERRUPT
        system_enable_irq(IRQ_MSC2);
        system_enable_irq(IRQ_MSC1);
#endif

#if SD_DMA_ENABLE && SD_DMA_INTERRUPT
        system_enable_irq(DMA_IRQ(DMA_SD_RX_CHANNEL(SD_SLOT_1)));
        system_enable_irq(DMA_IRQ(DMA_SD_RX_CHANNEL(SD_SLOT_2)));
        system_enable_irq(DMA_IRQ(DMA_SD_TX_CHANNEL(SD_SLOT_1)));
        system_enable_irq(DMA_IRQ(DMA_SD_TX_CHANNEL(SD_SLOT_2)));
#endif

        inited = true;
    }

    for (int drive = 0; drive < NUM_DRIVES; drive++) {
        mutex_lock(&sd_mtx[drive]);
        __sd_init_device(drive);
        mutex_unlock(&sd_mtx[drive]);
    }

    return 0;
}

tCardInfo* card_get_info_target(const int drive)
{
    return &card[drive];
}

static inline void sd_start_transfer(const int drive)
{
    mutex_lock(&sd_mtx[drive]);
    switch (MSC_CHN(drive)) {
    case 0:
        __cpm_start_msc0();
        break;
    case 1:
        __cpm_start_msc1();
        break;
    default:
        __cpm_start_msc2();
        break;
    }

    active[drive] = 1;
    led(active[SD_SLOT_1] || active[SD_SLOT_2]);
}

static inline void sd_stop_transfer(const int drive)
{
    active[drive] = 0;
    led(active[SD_SLOT_1] || active[SD_SLOT_2]);

    if (drive == SD_SLOT_1) {
#ifdef COLORFLY_C10
        __cpm_stop_msc0();
#else
        __cpm_stop_msc2();
#endif
    } else {
#ifndef COLORFLY_C10
        __cpm_stop_msc1();
#endif
    }
    mutex_unlock(&sd_mtx[drive]);
}

int sd_transfer_sectors(IF_MD(const int drive,) sector_t start, int count, void* buf, bool write)
{
    struct sd_request request = {0};
    struct sd_response_r1 r1;
    int retval = -1;
#ifndef HAVE_MULTIDRIVE
    const int drive = 0;
#endif

#ifdef COLORFLY_C10
    /* Writes to internal storage must remain inside validated FAT32
     * partition 4. This also protects the MBR and stock boot/system
     * partitions when the raw eMMC drive is exported over USB. */
    if (write && drive == SD_SLOT_1) {
        if (count < 1 || c10_emmc_part4_start == 0 ||
            start < c10_emmc_part4_start ||
            start >= c10_emmc_part4_end ||
            (sector_t)count > c10_emmc_part4_end - start)
            return -1;
    }
#endif

    sd_start_transfer(drive);

    if (!card_detect_target(drive) || count < 1)
        goto err;

    /* Retry card setup before using numblocks; a failed early init leaves it zero. */
    if (card[drive].initialized == 0 && __sd_init_device(drive))
        goto err;

    if ((start + count) > card[drive].numblocks)
        goto err;

    sd_simple_cmd(drive, &request, SD_SEND_STATUS, card[drive].rca, RESPONSE_R1);
    if ((retval = sd_unpack_r1(&request, &r1)))
        goto err;

    sd_simple_cmd(drive, &request, SD_SET_BLOCKLEN, SD_BLOCK_SIZE, RESPONSE_R1);
    if ((retval = sd_unpack_r1(&request, &r1)))
        goto err;

    if (use_sbc[drive] && count > 1) {
        sd_simple_cmd(drive, &request, SD_SET_BLOCK_COUNT, count, RESPONSE_R1);
        if ((retval = sd_unpack_r1(&request, &r1)))
            goto err;
    }

#ifdef STORAGE_64BIT_SECTOR
    // XXX check card[drive].ocr, b27+b30 == 11
    if (card[drive].numblocks > 0xffffffffULL) {
        uint32_t upper = start >> 32;
        sd_simple_cmd(drive, &request, SD_UC_ADDRESS_EXTENSION, upper, RESPONSE_R1);
        if ((retval = sd_unpack_r1(&request, &r1)))
            goto err;
    }
#endif

    sd_send_cmd(drive, &request,
                (count > 1) ?
                (write ? SD_WRITE_MULTIPLE_BLOCK : SD_READ_MULTIPLE_BLOCK) :
                (write ? SD_WRITE_BLOCK : SD_READ_SINGLE_BLOCK),
                card[drive].sd2plus ? start : (start * SD_BLOCK_SIZE),
                count, SD_BLOCK_SIZE, RESPONSE_R1, buf);
    retval = sd_unpack_r1(&request, &r1);

#ifdef COLORFLY_C10
    /* Successful multiblock requests are terminated by the MSC controller's
     * stop command. A failed transfer still falls back to an explicit CMD12
     * below, exactly like the stock driver's error path. */
    bool manual_stop = false;
#else
    bool manual_stop = !use_sbc[drive] && count > 1;
#endif
    if (retval || manual_stop) {
        int transfer_retval = retval;
        sd_simple_cmd(drive, &request, SD_STOP_TRANSMISSION, 0, RESPONSE_R1B);
        retval = sd_unpack_r1(&request, &r1);
        if (transfer_retval)
            retval = transfer_retval;
        else if (!write && retval == SD_ERROR_OUT_OF_RANGE)
            retval = 0;
    }

err:
    last_disk_activity = current_tick;
    sd_stop_transfer(drive);

    return retval;
}

int sd_read_sectors(IF_MD(int drive,) sector_t start, int count, void* buf)
{
  return sd_transfer_sectors(IF_MD(drive,) start, count, buf, false);
}

int sd_write_sectors(IF_MD(int drive,) sector_t start, int count, const void* buf)
{
  return sd_transfer_sectors(IF_MD(drive,) start, count, (void*)buf, true);
}

long sd_last_disk_activity(void)
{
    return last_disk_activity;
}

int sd_spinup_time(void)
{
    return 0;
}

void sd_enable(bool on)
{
    (void)on;
}

bool sd_disk_is_active(void)
{
    for (int drive = 0; drive < NUM_DRIVES; drive++) {
        if (active[drive])
            return true;
    }

    return false;
}

int sd_soft_reset(void)
{
    return 0;
}

#ifdef HAVE_HOTSWAP
bool sd_removable(IF_MD_NONVOID(const int drive))
{
#ifdef COLORFLY_C10
    return drive == SD_SLOT_2;
#else
#ifdef HAVE_MULTIDRIVE
    (void)drive;
#endif
    return true;
#endif
}

static int sd_oneshot_callback(struct timeout *tmo)
{
    int slot = (int) tmo->data;
    int state = card_detect_target(slot);

    /* This is called only if the state was stable for 300ms - check state
     * and post appropriate event. */
    queue_broadcast(state ? SYS_HOTSWAP_INSERTED : SYS_HOTSWAP_EXTRACTED,
                    sd_drive_nr + slot);

    sd_gpio_setup_irq(slot, state);

    return 0;
}

/* called on insertion/removal interrupt */
void GPIO_SD1_CD(void)
{
    static struct timeout sd1_oneshot;
    timeout_register(&sd1_oneshot, sd_oneshot_callback, (3*HZ/10), SD_SLOT_1);
}

void GPIO_SD2_CD(void)
{
    static struct timeout sd2_oneshot;
    timeout_register(&sd2_oneshot, sd_oneshot_callback, (3*HZ/10), SD_SLOT_2);
}

bool sd_present(IF_MD_NONVOID(const int drive))
{
#ifndef HAVE_MULTIDRIVE
    const int drive = 0;
#endif
    return card_detect_target(drive);
}
#endif

#ifdef CONFIG_STORAGE_MULTI
int sd_num_drives(int first_drive)
{
    sd_drive_nr = first_drive;
    return NUM_DRIVES;
}
#endif /* CONFIG_STORAGE_MULTI */

int sd_event(long id, intptr_t data)
{
    int rc = 0;

    switch (id)
    {
#ifdef HAVE_HOTSWAP
    case SYS_HOTSWAP_INSERTED:
    case SYS_HOTSWAP_EXTRACTED:
        /* Force card init for new card, re-init for re-inserted one or
         * clear if the last attempt to init failed with an error. */
        mutex_lock(&sd_mtx[data]); /* lock-out card activity */
        card[data].initialized = 0;
	if (id == SYS_HOTSWAP_INSERTED)
            __sd_init_device(data);
        mutex_unlock(&sd_mtx[data]);
        break;
#endif /* HAVE_HOTSWAP */
    default:
        rc = storage_event_default_handler(id, data, last_disk_activity,
                                           STORAGE_SD);
        break;
    }

    return rc;
}
