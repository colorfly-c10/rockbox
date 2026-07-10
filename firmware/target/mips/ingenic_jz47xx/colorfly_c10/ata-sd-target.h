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

/* Stock Linux registers MSC0 and MSC2. Rockbox drive 0 is internal eMMC on
 * MSC0; drive 1 is the external TF/microSD slot on MSC2.
 */

/* Stock lepus_tf_status() reads GPIOB20 active-low for TF/microSD detect.
 * The addresses/masks below are verified against stock lepus_tf_gpio_init().
 */
#define PIN_SD1_CD    (32*0+29)
#define IRQ_SD1_CD    GPIO_IRQ(PIN_SD1_CD)
#define GPIO_SD1_CD   GPIO29

#define PIN_TF_POWER  (32*1+30)

#define PIN_SD2_CD    (32*1+20)
#define IRQ_SD2_CD    GPIO_IRQ(PIN_SD2_CD)
#define GPIO_SD2_CD   GPIO52

static inline void sd_init_gpio(void)
{
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
