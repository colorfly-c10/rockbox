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

#include "config.h"
#include "jz4760b.h"
#include "../kernel-internal.h"
#include "backlight.h"
#include "font.h"
#include "lcd.h"
#include "file.h"
#include "system.h"
#include "button.h"
#include "common.h"
#include "rb-loader.h"
#include "loader_strerror.h"
#include "storage.h"
#include "file_internal.h"
#include "disk.h"
#include "version.h"

#include "xdebug.h"

#define SHOW_LOGO

extern void power_off(void);

static int lcd_inited = 0;
void init_lcd(void)
{
    if (lcd_inited)
        return;

    lcd_init();
    font_init();
    lcd_setfont(FONT_SYSFIXED);

    lcd_clear_display();
    lcd_update();

    backlight_init();

    lcd_inited = true;
}

/* Jump to loaded binary */
void exec(void* addr) __attribute__((noinline, noreturn, section(".icode")));

void exec(void* addr)
{
    commit_discard_idcache();
    typedef void(*entry_fn)(void) __attribute__((noreturn));
    entry_fn fn = (entry_fn)addr;
    fn();
}

static int boot_rockbox(void)
{
    int rc;

    printf("Mounting disk...");

    while ((rc = disk_mount_all()) <= 0) {
        verbose = true;
        printf("disk_mount_all rc %d", rc);
        error(EDISK, rc, true);
    }

    printf("Loading firmware...");
    rc = load_firmware((unsigned char *)CONFIG_SDRAM_START, BOOTFILE, 0x400000);
    if (rc <= EFILE_EMPTY) {
        printf("!! " BOOTFILE);
        return rc;
    } else {
        printf("Starting Rockbox...");
        disable_interrupt();
        exec((void*) CONFIG_SDRAM_START);
        return 0; /* Shouldn't happen */
    }
}

int main(void)
{
    int rc;

    serial_puts("\n\nSPL Stage 2\n\n");

    system_init();
    kernel_init();

    init_lcd();

#ifdef SHOW_LOGO
    show_logo();
#else
    printf(MODEL_NAME" Rockbox Bootloader");
    printf("Version %s", rbversion);
#endif

    button_init();
    enable_irq();

    int btn = button_read_device();
    if (btn & BUTTON_POWER) {
        verbose = true;
    }

    rc = storage_init();
    if (rc) {
        verbose = true;
        error(EATA, rc, true);
    }

    filesystem_init();

    rc = boot_rockbox();

    if (rc <= EFILE_EMPTY) {
        verbose = true;
        printf("Error: %s", loader_strerror(rc));
    }

    /* Power off */
    sleep(5*HZ);
    power_off();

    return 0;
}
