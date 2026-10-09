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

#ifndef BUTTON_TARGET_H
#define BUTTON_TARGET_H

/* The C10 has no physical hold switch. The shared JZ4760 USB DMA driver still
 * probes this target hook directly, so keep the always-false implementation
 * without advertising HAS_BUTTON_HOLD to Rockbox's input layer.
 */
bool button_hold(void);

#define BUTTON_POWER      0x00000001
#define BUTTON_HOME       0x00000002
#define BUTTON_OPTION     0x00000004
#define BUTTON_UP       0x00000008
#define BUTTON_DOWN       0x00000010
#define BUTTON_PLAY       0x00000020
#define BUTTON_LEFT       0x00000040
#define BUTTON_RIGHT      0x00000080

#define BUTTON_MAIN  (BUTTON_POWER | BUTTON_HOME | BUTTON_OPTION | BUTTON_UP | \
                      BUTTON_DOWN | BUTTON_PLAY | BUTTON_LEFT | BUTTON_RIGHT)

/* Allow 800 ms before repeating navigation and long-press actions. */
#define KEY_REPEAT_START (80*HZ/100)

/* Software power-off */
#define POWEROFF_BUTTON BUTTON_POWER
#define POWEROFF_COUNT  10

#endif /* BUTTON_TARGET_H */
