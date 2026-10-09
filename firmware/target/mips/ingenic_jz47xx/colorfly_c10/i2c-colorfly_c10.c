/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2016 by Roman Stolyarov
 * Copyright (C) 2026 by Anton Bambura
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
/* C10 has the DAC on I2C0 and the AXP202 on I2C1. PMIC requests from
 * the display, power worker, battery monitor and volume thread share a lock.
 * I2C1 uses the existing JZ4760 interrupt-driven register transfers. */
#include "config.h"
#include "cpu.h"
#include "system.h"
#include "kernel.h"
#include "logf.h"
#include "i2c.h"
#include "i2c-target.h"

#define C10_AXP202_ADDR 0x34
#define I2C_CLK 100000
#define TIMEOUT 100000

/* Existing JZ4760 subaddress transfer; no shared API extension is needed. */
extern int xfer_read_subaddr(unsigned char subaddr, unsigned char device,
                             unsigned char *buf, int length);

#define C10_I2C_CHN 0

static struct mutex c10_i2c0_mutex;
static struct mutex c10_i2c1_mutex;
static bool c10_i2c1_mutex_ready;

static int c10_i2c0_setup(unsigned char address)
{
    int count = __cpm_get_pclk() / I2C_CLK - 23;
    int timeout = TIMEOUT;
    int ignored;

    __i2c_disable(C10_I2C_CHN);
    while (__i2c_is_enable(C10_I2C_CHN) && --timeout)
        udelay(1);
    if (!timeout || count < 0)
        return -1;

    REG_I2C_CTRL(C10_I2C_CHN) = 0x43 | I2C_CTRL_REST;
    REG_I2C_SHCNT(C10_I2C_CHN) = count / 2 + 1;
    REG_I2C_SLCNT(C10_I2C_CHN) = count / 2 + 13 + (count & 1);
    REG_I2C_TAR(C10_I2C_CHN) = address;
    REG_I2C_INTM(C10_I2C_CHN) = 0;
    __i2c_clear_interrupts(ignored, C10_I2C_CHN);
    (void)ignored;

    __i2c_enable(C10_I2C_CHN);
    timeout = TIMEOUT;
    while (__i2c_is_disable(C10_I2C_CHN) && --timeout)
        udelay(1);
    return timeout ? 0 : -1;
}

static bool c10_i2c0_aborted(void)
{
    return (REG_I2C_INTST(C10_I2C_CHN) & I2C_INTST_TXABT) ||
           REG_I2C_TXABRT(C10_I2C_CHN);
}

void c10_i2c0_init(void)
{
    /* Stock registers DAC4398 on I2C0 (PD30/PD31), while the AXP202
     * remains on the interrupt-driven I2C1 controller. */
    __gpio_as_i2c(C10_I2C_CHN);
    __cpm_start_i2c0();
    mutex_init(&c10_i2c0_mutex);
}

int c10_i2c0_write(int device, const unsigned char *buf, int count)
{
    int ignored;
    int result = -1;

    if (count < 1)
        return -1;

    mutex_lock(&c10_i2c0_mutex);
    if (c10_i2c0_setup(device) < 0)
        goto out;

    while (count-- > 0)
    {
        int timeout = TIMEOUT;
        while (!(REG_I2C_STA(C10_I2C_CHN) & I2C_STA_TFNF) && --timeout)
            udelay(1);
        if (!timeout || c10_i2c0_aborted())
            goto error;
        REG_I2C_DC(C10_I2C_CHN) = *buf++;
    }

    int timeout = TIMEOUT;
    while (!(REG_I2C_STA(C10_I2C_CHN) & I2C_STA_TFE) && --timeout)
        udelay(1);
    if (!timeout)
        goto error;

    timeout = TIMEOUT;
    while ((REG_I2C_STA(C10_I2C_CHN) & I2C_STA_MSTACT) && --timeout)
        udelay(1);
    if (!timeout || c10_i2c0_aborted())
        goto error;
    result = 0;
    goto out;

error:
    __i2c_clear_interrupts(ignored, C10_I2C_CHN);
    (void)ignored;
    logf("C10 I2C0 write to %02x failed", device);
out:
    mutex_unlock(&c10_i2c0_mutex);
    return result;
}

void c10_i2c1_init(void)
{
    if (c10_i2c1_mutex_ready)
        return;

    mutex_init(&c10_i2c1_mutex);
    i2c_init();
    c10_i2c1_mutex_ready = true;
}

int c10_axp202_read(unsigned char reg)
{
    int result;

    unsigned char value;

    /* I2C1's interrupt handler uses shared transfer state. Serialize complete
     * C10 PMIC transactions rather than only protecting the subaddress. */
    mutex_lock(&c10_i2c1_mutex);
    result = xfer_read_subaddr(reg, C10_AXP202_ADDR, &value, 1);
    mutex_unlock(&c10_i2c1_mutex);
    return result < 0 ? -1 : value;
}

int c10_axp202_write(unsigned char reg, unsigned char value)
{
    unsigned char data[2] = { reg, value };
    int result;

    mutex_lock(&c10_i2c1_mutex);
    result = i2c_write(C10_AXP202_ADDR, data, sizeof(data));
    mutex_unlock(&c10_i2c1_mutex);
    return result;
}
