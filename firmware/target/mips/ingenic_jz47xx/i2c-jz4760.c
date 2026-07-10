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
#include "system.h"
#include "cpu.h"
#include "logf.h"
#include "i2c.h"
#include "kernel.h"

#define I2C_CHN   1
#define I2C_CLK   100000

#define I2C_READ  1
#define I2C_WRITE 0

#define I2C_M_RD  1
#define I2C_M_WR  2

#define TIMEOUT   100000

#ifdef COLORFLY_C10
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

int c10_i2c0_read_reg(int device, unsigned char reg, unsigned char *value)
{
    int ignored;
    int timeout;
    int result = -1;

    if (!value)
        return -1;

    mutex_lock(&c10_i2c0_mutex);
    if (c10_i2c0_setup(device) < 0)
        goto out;

    /* Queue the subaddress and read command together. With RESTART enabled,
     * the JZ4760 controller emits the repeated START used by SMBus byte-data
     * reads before the write phase can terminate. */
    REG_I2C_DC(C10_I2C_CHN) = reg;
    REG_I2C_DC(C10_I2C_CHN) = I2C_READ << 8;

    timeout = TIMEOUT;
    while (!(REG_I2C_STA(C10_I2C_CHN) & I2C_STA_RFNE) && --timeout)
    {
        if (c10_i2c0_aborted())
            goto error;
        udelay(1);
    }
    if (!timeout)
        goto error;

    *value = REG_I2C_DC(C10_I2C_CHN) & 0xff;
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
    logf("C10 I2C0 read from %02x failed", device);
out:
    mutex_unlock(&c10_i2c0_mutex);
    return result;
}
#endif

static char i2c_rwflags;
static int i2c_ctrl_rest = 0;
static unsigned char *msg_buf;
static int cmd_cnt;
static volatile int cmd_flag;
static int r_cnt;
static unsigned char i2c_subaddr = 0;

/*
 * I2C bus protocol basic routines
 */

/* Interrupt handler */
void I2C1(void)
{
    int timeout = TIMEOUT;

    if (__i2c_abrt_7b_addr_nack(I2C_CHN)) {
        int ret;
        cmd_flag = -1;
        __i2c_clear_interrupts(ret,I2C_CHN);
        REG_I2C_INTM(I2C_CHN) = 0x0;
	(void)ret;
        return;
    }

    /* first byte,when length > 1 */
    if (cmd_flag == 0 && cmd_cnt > 1) {
        cmd_flag = 1;
        if (i2c_rwflags == I2C_M_RD) {
            REG_I2C_DC(I2C_CHN) = I2C_READ << 8;
        } else {
            REG_I2C_DC(I2C_CHN) = (I2C_WRITE << 8) | *msg_buf++;
        }
        cmd_cnt--;
    }

    if (i2c_rwflags == I2C_M_RD) {
        if (REG_I2C_STA(I2C_CHN) & I2C_STA_RFNE) {
            *msg_buf++ = REG_I2C_DC(I2C_CHN) & 0xff;
            r_cnt--;
        }

        REG_I2C_DC(I2C_CHN) = I2C_READ << 8;
    } else {
        REG_I2C_DC(I2C_CHN) = (I2C_WRITE << 8) | *msg_buf++;
    }

    cmd_cnt--;

    if (!(cmd_cnt)) {
        REG_I2C_INTM(I2C_CHN) = 0x0;
        cmd_flag = 2;
        if (i2c_rwflags == I2C_M_RD){
            while (r_cnt > 2) {
                if ((REG_I2C_STA(I2C_CHN) & I2C_STA_RFNE) && timeout) {
                    *msg_buf++ = REG_I2C_DC(I2C_CHN) & 0xff;
                    r_cnt--;
                }
                if (!(timeout--)) {
                    cmd_flag = -1;
                    return;
                }
            }
        }
    }

    return;
}

static int i2c_set_clk(int i2c_clk)
{
    int dev_clk = __cpm_get_pclk();
    int count = 0;

    if (i2c_clk < 0 || i2c_clk > 400000)
        goto Set_clk_err;

    count = dev_clk/i2c_clk - 23;
    if (count < 0)
        goto Set_clk_err;

    if (i2c_clk <= 100000) {
        REG_I2C_CTRL(I2C_CHN) = 0x43 | i2c_ctrl_rest;      /* standard speed mode*/
        if (count%2 == 0) {
            REG_I2C_SHCNT(I2C_CHN) = count/2 + 6 - 5;
            REG_I2C_SLCNT(I2C_CHN) = count/2 + 8 + 5;
        } else {
            REG_I2C_SHCNT(I2C_CHN) = count/2 + 6 -5;
            REG_I2C_SLCNT(I2C_CHN) = count/2 + 8 +5 + 1;
        }
    } else {
        REG_I2C_CTRL(I2C_CHN) = 0x45 | i2c_ctrl_rest;       /* high speed mode*/
        if (count%2 == 0) {
            REG_I2C_FHCNT(I2C_CHN) = count/2 + 6;
            REG_I2C_FLCNT(I2C_CHN) = count/2 + 8;
        } else {
            REG_I2C_FHCNT(I2C_CHN) = count/2 + 6;
            REG_I2C_FLCNT(I2C_CHN) = count/2 + 8 + 1;
        }
    }
    return 0;

Set_clk_err:

    logf("i2c set sclk faild,i2c_clk=%d,dev_clk=%d.\n",i2c_clk,dev_clk);
    return -1;
}

static int i2c_disable(void)
{
    int timeout = TIMEOUT;

    __i2c_disable(I2C_CHN);
    while(__i2c_is_enable(I2C_CHN) && (timeout > 0)) {
        udelay(1);
        timeout--;
    }
    if(timeout)
        return 0;
    else
        return 1;
}

static int i2c_enable(void)
{
    int timeout = TIMEOUT;

    __i2c_enable(I2C_CHN);
    while(__i2c_is_disable(I2C_CHN) && (timeout > 0)) {
        udelay(1);
        timeout--;
    }
    if(timeout)
        return 0;
    else
        return 1;
}

static void i2c_init_as_master(unsigned char address)
{
    if(i2c_disable())
        logf("i2c not disable\n");

    i2c_set_clk(I2C_CLK);

    REG_I2C_TAR(I2C_CHN) = address;  /* slave id needed write only once */
    REG_I2C_INTM(I2C_CHN) = 0x0; /* unmask all interrupts */
    REG_I2C_TXTL(I2C_CHN) = 0x1;

    if(i2c_enable())
        logf("i2c not enable\n");
}

int xfer_read_subaddr(unsigned char subaddr, unsigned char device, unsigned char *buf, int length)
{
    int timeout,r_i = 0;

    cmd_cnt = length;
    r_cnt = length;
    msg_buf = buf;
    i2c_rwflags = I2C_M_RD;
    i2c_ctrl_rest = I2C_CTRL_REST;
    i2c_init_as_master(device);

    REG_I2C_DC(I2C_CHN) = (I2C_WRITE << 8) | subaddr;

    cmd_flag = 0;
    REG_I2C_INTM(I2C_CHN) = 0x10;
    timeout = TIMEOUT;
    while (cmd_flag != 2 && --timeout) {
        if (cmd_flag == -1) {
            r_i = 1;
            goto R_dev_err;
        }
        udelay(10);
    }
    if (!timeout) {
        r_i = 4;
        goto R_timeout;
    }

    while (r_cnt) {
        while (!(REG_I2C_STA(I2C_CHN) & I2C_STA_RFNE)) {
            if ((cmd_flag == -1) ||
                (REG_I2C_INTST(I2C_CHN) & I2C_INTST_TXABT) ||
                REG_I2C_TXABRT(I2C_CHN)) {
                int ret;
                r_i = 2;
                __i2c_clear_interrupts(ret,I2C_CHN);
		(void)ret;
                goto R_dev_err;
            }
        }
        *msg_buf++ = REG_I2C_DC(I2C_CHN) & 0xff;
        r_cnt--;
    }

    timeout = TIMEOUT;
    while ((REG_I2C_STA(I2C_CHN) & I2C_STA_MSTACT) && --timeout)
        udelay(10);
    if (!timeout){
        r_i = 3;
        goto R_timeout;
    }

    return 0;

R_dev_err:
R_timeout:

    i2c_init_as_master(device);
    if (r_i == 1) {
        logf("Read i2c device 0x%2x failed in r_i = %d :device no ack.\n",device,r_i);
    } else if (r_i == 2) {
        logf("Read i2c device 0x%2x failed in r_i = %d :i2c abort.\n",device,r_i);
    } else if (r_i == 3) {
        logf("Read i2c device 0x%2x failed in r_i = %d :waite master inactive timeout.\n",device,r_i);
    } else if (r_i == 4) {
        logf("Read i2c device 0x%2x failed in r_i = %d.\n",device,r_i);
    } else {
        logf("Read i2c device 0x%2x failed in r_i = %d.\n",device,r_i);
    }
    return -1;
}

#ifdef COLORFLY_C10
int c10_i2c1_read_reg(int device, unsigned char reg, unsigned char *value)
{
    int result;

    if (!value)
        return -1;

    /* I2C1's interrupt handler uses shared transfer state. Serialize complete
     * C10 PMIC transactions rather than only protecting the subaddress. */
    mutex_lock(&c10_i2c1_mutex);
    result = xfer_read_subaddr(reg, device, value, 1);
    mutex_unlock(&c10_i2c1_mutex);
    return result;
}

int c10_i2c1_write_reg(int device, unsigned char reg, unsigned char value)
{
    unsigned char data[2] = { reg, value };
    int result;

    mutex_lock(&c10_i2c1_mutex);
    result = i2c_write(device, data, sizeof(data));
    mutex_unlock(&c10_i2c1_mutex);
    return result;
}
#endif

int xfer_write_subaddr(unsigned char subaddr, unsigned char device, const unsigned char *buf, int length)
{
    int timeout,w_i = 0;

    cmd_cnt = length;
    r_cnt = length;
    msg_buf = (unsigned char *)buf;
    i2c_rwflags = I2C_M_WR;
    i2c_ctrl_rest = I2C_CTRL_REST;
    i2c_init_as_master(device);

    REG_I2C_DC(I2C_CHN) = (I2C_WRITE << 8) | subaddr;

    cmd_flag = 0;
    REG_I2C_INTM(I2C_CHN) = 0x10;

    timeout = TIMEOUT;
    while ((cmd_flag != 2) && (--timeout))
    {
        if (cmd_flag == -1){
            w_i = 1;
            goto W_dev_err;
        }
        udelay(10);
    }

    timeout = TIMEOUT;
    while((!(REG_I2C_STA(I2C_CHN) & I2C_STA_TFE)) && --timeout){
        udelay(10);
    }
    if (!timeout){
        w_i = 2;
        goto W_timeout;
    }

    timeout = TIMEOUT;
    while (__i2c_master_active(I2C_CHN) && --timeout);
    if (!timeout){
        w_i = 3;
        goto W_timeout;
    }

    if ((length == 1)&&
        ((cmd_flag == -1) ||
        (REG_I2C_INTST(I2C_CHN) & I2C_INTST_TXABT) ||
        REG_I2C_TXABRT(I2C_CHN))) {
        int ret;
        w_i = 5;
        __i2c_clear_interrupts(ret,I2C_CHN);
	(void)ret;
        goto W_dev_err;
    }

    return 0;

W_dev_err:
W_timeout:

    i2c_init_as_master(device);
    if (w_i == 1) {
        logf("Write i2c device 0x%2x failed in w_i=%d:device no ack. I2C_CHN:[%d]sxyzhang\n",device,w_i,I2C_CHN);
    } else if (w_i == 2) {
        logf("Write i2c device 0x%2x failed in w_i=%d:waite TF buff empty timeout.\n",device,w_i);
    } else if (w_i == 3) {
        logf("Write i2c device 0x%2x failed in w_i=%d:waite master inactive timeout.\n",device,w_i);
    } else if (w_i == 5) {
        logf("Write i2c device 0x%2x failed in w_i=%d:device no ack or abort.I2C_CHN:[%d]sxyzhang \n",device,w_i,I2C_CHN);
    } else  {
        logf("Write i2c device 0x%2x failed in w_i=%d.\n",device,w_i);
    }

    return -1;
}

int i2c_read(int device, unsigned char* buf, int count)
{
    return xfer_read_subaddr(i2c_subaddr, device, &buf[0], count);
}

int i2c_write(int device, const unsigned char* buf, int count)
{
    if (count < 2)
    {
        i2c_subaddr = buf[0];
        return 0;
    }
    return xfer_write_subaddr(buf[0], device, &buf[1], count-1);
}

void i2c_init(void)
{
#ifdef COLORFLY_C10
    if (!c10_i2c1_mutex_ready)
    {
        mutex_init(&c10_i2c1_mutex);
        c10_i2c1_mutex_ready = true;
    }
#endif
    __gpio_as_i2c(I2C_CHN);
    __cpm_start_i2c1();
    system_enable_irq(IRQ_I2C1);
}
