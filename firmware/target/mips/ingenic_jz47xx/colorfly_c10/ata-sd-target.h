/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__\/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 by Anton Bambura
 ****************************************************************************/

#ifndef ATA_SD_TARGET_H
#define ATA_SD_TARGET_H

#include "cpu.h"
#include "system.h"
#include "storage.h"

/* Stock Linux registers MSC0 and MSC2. Rockbox drive 0 is internal eMMC on
 * MSC0; drive 1 is the external TF/microSD slot on MSC2.
 */

/* Stock lepus_tf_status() reads GPIOB20 active-low for TF/microSD detect.
 * The addresses/masks below are verified against stock lepus_tf_gpio_init().
 */
#define PIN_TF_POWER  (32*1+30)

#define PIN_SD2_CD    (32*1+20)
#define IRQ_SD2_CD    GPIO_IRQ(PIN_SD2_CD)
#define GPIO_SD2_CD   GPIO52

/* Poll descriptor completion rather than sharing the legacy command/DMA
 * semaphore. C10 maps drive 0 to MSC0 and drive 1 to MSC2. */
#define MSC_CHN(drive) ((drive) == 0 ? 0 : 2)
#define SD_CLOCK_HIGH 24000000
#define SD_MMC_DRIVE(drive) ((drive) == 0)
#define SD_MMC_R2_CRC_QUIRK
#define SD_KEEP_CONTROLLER_CLOCK(drive) ((drive) == 1)
#define SD_CARD_REMOVABLE(drive) ((drive) == 1)
#define SD_CARD_PRESENT(drive) ((drive) == 0 || !__gpio_get_pin(PIN_SD2_CD))
#define SD_PRE_INIT c10_sd_pre_init
#define SD_GPIO_SETUP_IRQ c10_sd_gpio_setup_irq
#define SD_VALIDATE_CARD c10_sd_validate_card
#define SD_WRITE_ALLOWED c10_sd_write_allowed

int c10_sd_validate_card(int drive, sector_t blocks,
                        int (*read_mbr)(int, unsigned char *));
bool c10_sd_write_allowed(int drive, sector_t start, int count);

static inline void c10_sd_pre_init(int drive)
{
    if (drive == 1) {
        /* Reassert stock TF power on every retry and let its rail settle. */
        REG_GPIO_PXDATC(1) = (1u << 30);
        sleep(HZ / 50);
    }
    /* Stock uses the 24 MHz MSC parent clock for both controllers. */
    __cpm_select_msc_clk(1);
}

static inline void c10_sd_gpio_setup_irq(int drive, bool inserted)
{
    if (drive != 1)
        return;
    if (inserted)
        __gpio_as_irq_rise_edge(PIN_SD2_CD);
    else
        __gpio_as_irq_fall_edge(PIN_SD2_CD);
    system_enable_irq(IRQ_SD2_CD);
}

static inline void sd_init_gpio(void)
{
    __cpm_stop_msc1(); /* This board does not use MSC1. */
    __gpio_as_msc0_pa_4bit();

    /* Stock lepus_tf_gpio_init() selects PE20..PE23 and PE28..PE29 for MSC2.
     * This is 0x30f00000, not the PE16..PE19/PE24..PE25 group used earlier.
     */
    REG_GPIO_PXFUNS(4) = 0x30f00000;
    REG_GPIO_PXTRGS(4) = 0x30f00000;
    REG_GPIO_PXSELC(4) = 0x30f00000;
    REG_GPIO_PXPES(4)  = 0x30f00000;

    /* These sideband writes match the stock binary instruction-for-instruction.
     * They must use PXFUNC (clear) rather than PXFUNS: both sideband pins are
     * ordinary GPIOs, whereas the PE MSC2 pins above are alternate-function.
     * PB30 is active-low TF power/control; PB20 is active-low card detect.
     */
    REG_GPIO_PXFUNC(1) = (1 << 30);
    REG_GPIO_PXSELC(1) = (1 << 30);
    REG_GPIO_PXDIRS(1) = (1 << 30);
    REG_GPIO_PXPES(1)  = (1 << 30);
    REG_GPIO_PXDATS(1) = (1 << 30); /* stock init leaves TF power off */

    REG_GPIO_PXFUNC(1) = (1 << 20);
    REG_GPIO_PXSELC(1) = (1 << 20);
    REG_GPIO_PXDIRC(1) = (1 << 20);
    REG_GPIO_PXPEC(1)  = (1 << 20);
}

#endif
