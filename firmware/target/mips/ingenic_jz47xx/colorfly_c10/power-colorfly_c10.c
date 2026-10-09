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

#include "config.h"
#include "cpu.h"
#include "i2c-target.h"
#include "power.h"
#include "system.h"

/* The stock kernel registers the AXP202 on I2C1 at this seven-bit address. */
#define C10_AXP202_INPUT_STATUS     0x00
#define C10_AXP202_SHUTDOWN_CONTROL 0x32
#define C10_AXP202_VBUS_PRESENT     (1u << 5)
#define C10_AXP202_BAT_CHARGING     (1u << 2)
#define C10_AXP202_POWER_OFF        0xcc

unsigned int power_input_status(void)
{
    int status = c10_axp202_read(C10_AXP202_INPUT_STATUS);

    if (status < 0)
        return POWER_INPUT_NONE;

    return (status & C10_AXP202_VBUS_PRESENT) ?
        POWER_INPUT_USB_CHARGER : POWER_INPUT_NONE;
}

void power_init(void)
{
    /* LCD and battery monitoring share the stock I2C1 AXP202 connection. */
    c10_i2c1_init();
}

bool charging_state(void)
{
    int status = c10_axp202_read(C10_AXP202_INPUT_STATUS);

    return status >= 0 && (status & C10_AXP202_BAT_CHARGING);
}

void power_off(void)
{
    /* Stock powers the C10 off through the AXP202; SoC RTC hibernate alone
     * leaves the player powered indefinitely. */
    c10_axp202_write(C10_AXP202_SHUTDOWN_CONTROL,
                     C10_AXP202_POWER_OFF);
    while (1);
}
