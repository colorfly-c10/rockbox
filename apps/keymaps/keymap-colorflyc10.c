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

/* Button mappings for the Colorfly C10 navigation pad. */

#include "config.h"
#include "action.h"
#include "button.h"
#include "settings.h"

/* {Action Code,    Button code,    Prereq button code } */

/*
 * The format of the list is as follows
 * { Action Code,   Button code,    Prereq button code }
 * if there's no need to check the previous button's value, use BUTTON_NONE
 * Insert LAST_ITEM_IN_LIST at the end of each mapping
 */
static const struct button_mapping button_context_standard[] = {
    { ACTION_STD_PREV,        BUTTON_UP,                      BUTTON_NONE },
    { ACTION_STD_PREVREPEAT,  BUTTON_UP|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_STD_NEXT,        BUTTON_DOWN,                    BUTTON_NONE },
    { ACTION_STD_NEXTREPEAT,  BUTTON_DOWN|BUTTON_REPEAT,      BUTTON_NONE },
    { ACTION_STD_CONTEXT,     BUTTON_PLAY|BUTTON_REPEAT,        BUTTON_PLAY },
    { ACTION_STD_CANCEL,      BUTTON_HOME|BUTTON_REL,           BUTTON_HOME },
    { ACTION_STD_OK,          BUTTON_PLAY|BUTTON_REL,           BUTTON_PLAY },
    { ACTION_STD_OK,          BUTTON_RIGHT|BUTTON_REL,          BUTTON_RIGHT },
    { ACTION_STD_MENU,        BUTTON_OPTION|BUTTON_REL,         BUTTON_OPTION },
    { ACTION_STD_QUICKSCREEN, BUTTON_OPTION|BUTTON_REPEAT,      BUTTON_OPTION },
    { ACTION_STD_KEYLOCK,     BUTTON_POWER|BUTTON_REL,          BUTTON_POWER },

    LAST_ITEM_IN_LIST
}; /* button_context_standard */

static const struct button_mapping button_context_wps[] = {
    { ACTION_WPS_PLAY,        BUTTON_PLAY|BUTTON_REL,           BUTTON_PLAY },
    { ACTION_STD_KEYLOCK,     BUTTON_POWER|BUTTON_REL,          BUTTON_POWER },
    { ACTION_WPS_STOP,        BUTTON_POWER|BUTTON_REPEAT,       BUTTON_POWER },
    { ACTION_WPS_QUICKSCREEN, BUTTON_UP|BUTTON_REL,             BUTTON_UP },
    { ACTION_WPS_VIEW_PLAYLIST, BUTTON_DOWN|BUTTON_REL,         BUTTON_DOWN },
    { ACTION_WPS_SKIPPREV,    BUTTON_LEFT|BUTTON_REL,           BUTTON_LEFT },
    { ACTION_WPS_SEEKBACK,    BUTTON_LEFT|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_WPS_STOPSEEK,    BUTTON_LEFT|BUTTON_REL,           BUTTON_LEFT|BUTTON_REPEAT },
    { ACTION_WPS_SKIPNEXT,    BUTTON_RIGHT|BUTTON_REL,          BUTTON_RIGHT },
    { ACTION_WPS_SEEKFWD,     BUTTON_RIGHT|BUTTON_REPEAT,       BUTTON_NONE },
    { ACTION_WPS_STOPSEEK,    BUTTON_RIGHT|BUTTON_REL,          BUTTON_RIGHT|BUTTON_REPEAT },
    { ACTION_WPS_BROWSE,      BUTTON_HOME|BUTTON_REL,           BUTTON_HOME },
    { ACTION_WPS_CONTEXT,     BUTTON_PLAY|BUTTON_REPEAT,        BUTTON_PLAY },
    { ACTION_WPS_HOTKEY,      BUTTON_HOME|BUTTON_REPEAT,        BUTTON_HOME },

    { ACTION_WPS_MENU,        BUTTON_OPTION|BUTTON_REL,                BUTTON_OPTION },
    { ACTION_WPS_QUICKSCREEN, BUTTON_OPTION|BUTTON_REPEAT,             BUTTON_OPTION },
    { ACTION_WPS_PITCHSCREEN, BUTTON_OPTION|BUTTON_HOME|BUTTON_REPEAT, BUTTON_NONE },

    { ACTION_WPS_ABSETB_NEXTDIR,    BUTTON_OPTION|BUTTON_DOWN,   BUTTON_OPTION },
    { ACTION_WPS_ABSETA_PREVDIR,    BUTTON_OPTION|BUTTON_UP,   BUTTON_OPTION },
    { ACTION_WPS_ABRESET,           BUTTON_OPTION|BUTTON_PLAY,   BUTTON_OPTION },

    LAST_ITEM_IN_LIST
}; /* button_context_wps */

/** Bookmark Screen **/
static const struct button_mapping button_context_bmark[] = {
    { ACTION_BMS_DELETE,      BUTTON_HOME|BUTTON_REPEAT,        BUTTON_HOME },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
}; /* button_context_bmark */

/** Keyboard **/
static const struct button_mapping button_context_keyboard[] = {
    { ACTION_KBD_LEFT,        BUTTON_LEFT,                      BUTTON_NONE },
    { ACTION_KBD_LEFT,        BUTTON_LEFT|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_KBD_RIGHT,       BUTTON_RIGHT,                     BUTTON_NONE },
    { ACTION_KBD_RIGHT,       BUTTON_RIGHT|BUTTON_REPEAT,       BUTTON_NONE },
    { ACTION_KBD_DOWN,        BUTTON_DOWN,                      BUTTON_NONE },
    { ACTION_KBD_DOWN,        BUTTON_DOWN|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_KBD_UP,          BUTTON_UP,                      BUTTON_NONE },
    { ACTION_KBD_UP,          BUTTON_UP|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_KBD_BACKSPACE,   BUTTON_HOME,                      BUTTON_NONE },
    { ACTION_KBD_BACKSPACE,   BUTTON_HOME|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_KBD_SELECT,      BUTTON_PLAY|BUTTON_REL,           BUTTON_PLAY },
    { ACTION_KBD_DONE,        BUTTON_PLAY|BUTTON_REPEAT,        BUTTON_PLAY },
    { ACTION_KBD_ABORT,       BUTTON_POWER|BUTTON_REL,          BUTTON_POWER },

    LAST_ITEM_IN_LIST
}; /* button_context_keyboard */

/** Pitchscreen **/
static const struct button_mapping button_context_pitchscreen[] = {
    { ACTION_PS_INC_SMALL,    BUTTON_UP,                      BUTTON_NONE },
    { ACTION_PS_INC_BIG,      BUTTON_UP|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_PS_DEC_SMALL,    BUTTON_DOWN,                      BUTTON_NONE },
    { ACTION_PS_DEC_BIG,      BUTTON_DOWN|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_PS_NUDGE_LEFT,   BUTTON_LEFT,                      BUTTON_NONE },
    { ACTION_PS_NUDGE_LEFTOFF, BUTTON_LEFT|BUTTON_REL,          BUTTON_NONE },
    { ACTION_PS_NUDGE_RIGHT,  BUTTON_RIGHT,                     BUTTON_NONE },
    { ACTION_PS_NUDGE_RIGHTOFF, BUTTON_RIGHT|BUTTON_REL,        BUTTON_NONE },
    { ACTION_PS_TOGGLE_MODE,  BUTTON_PLAY|BUTTON_REL,           BUTTON_NONE },
    { ACTION_PS_RESET,        BUTTON_POWER|BUTTON_REL,          BUTTON_POWER },
    { ACTION_PS_EXIT,         BUTTON_HOME|BUTTON_REL,           BUTTON_HOME },
    { ACTION_PS_SLOWER,       BUTTON_LEFT|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_PS_FASTER,       BUTTON_RIGHT|BUTTON_REPEAT,       BUTTON_NONE },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
}; /* button_context_pitchscreen */

/** Quickscreen **/
static const struct button_mapping button_context_quickscreen[] = {
    { ACTION_QS_TOP,          BUTTON_UP,                      BUTTON_NONE },
    { ACTION_QS_TOP,          BUTTON_UP|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_QS_DOWN,         BUTTON_DOWN,                      BUTTON_NONE },
    { ACTION_QS_DOWN,         BUTTON_DOWN|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_QS_LEFT,         BUTTON_LEFT,                      BUTTON_NONE },
    { ACTION_QS_LEFT,         BUTTON_LEFT|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_QS_RIGHT,        BUTTON_RIGHT,                     BUTTON_NONE },
    { ACTION_QS_RIGHT,        BUTTON_RIGHT|BUTTON_REPEAT,       BUTTON_NONE },
    { ACTION_STD_CANCEL,      BUTTON_HOME|BUTTON_REL,           BUTTON_HOME },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
}; /* button_context_quickscreen */

/** Settings - General Mappings **/
static const struct button_mapping button_context_settings[] = {
    { ACTION_STD_PREV,        BUTTON_UP,                      BUTTON_NONE },
    { ACTION_STD_PREVREPEAT,  BUTTON_UP|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_STD_NEXT,        BUTTON_DOWN,                      BUTTON_NONE },
    { ACTION_STD_NEXTREPEAT,  BUTTON_DOWN|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_STD_OK,          BUTTON_PLAY|BUTTON_REL,           BUTTON_PLAY },
    { ACTION_STD_CANCEL,      BUTTON_HOME|BUTTON_REL,           BUTTON_HOME },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
}; /* button_context_settings */

static const struct button_mapping button_context_settings_vol_is_inc[] = {
    { ACTION_SETTINGS_INC,    BUTTON_RIGHT,                     BUTTON_NONE },
    { ACTION_SETTINGS_INCREPEAT,BUTTON_RIGHT|BUTTON_REPEAT,     BUTTON_NONE },
    { ACTION_SETTINGS_DEC,    BUTTON_LEFT,                      BUTTON_NONE },
    { ACTION_SETTINGS_DECREPEAT,BUTTON_LEFT|BUTTON_REPEAT,      BUTTON_NONE },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
}; /* button_context_settings_vol_is_inc */

/** Tree **/
static const struct button_mapping button_context_tree[] = {
    { ACTION_STD_CANCEL,      BUTTON_LEFT|BUTTON_REL,           BUTTON_LEFT },
    { ACTION_TREE_HOTKEY,     BUTTON_HOME|BUTTON_REPEAT,        BUTTON_HOME},

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
}; /* button_context_tree */

static const struct button_mapping button_context_listtree_scroll_with_combo[]  = {
    { ACTION_NONE,           BUTTON_POWER,                             BUTTON_NONE },
    { ACTION_TREE_WPS,       BUTTON_HOME|BUTTON_REL,                    BUTTON_HOME },
    { ACTION_TREE_PGLEFT,    BUTTON_PLAY|BUTTON_UP,               BUTTON_NONE },
    { ACTION_TREE_PGLEFT,    BUTTON_PLAY|BUTTON_UP|BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_TREE_PGRIGHT,   BUTTON_PLAY|BUTTON_DOWN,                 BUTTON_NONE },
    { ACTION_TREE_PGRIGHT,   BUTTON_PLAY|BUTTON_DOWN|BUTTON_REPEAT,   BUTTON_NONE },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_CUSTOM|CONTEXT_TREE),
}; /* button_context_listtree_scroll_with_combo */

/** Yes/No Screen **/
static const struct button_mapping button_context_yesnoscreen[] = {
    { ACTION_YESNO_ACCEPT,    BUTTON_PLAY,                      BUTTON_NONE },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
}; /* button_context_settings_yesnoscreen */

#ifdef USB_ENABLE_HID
static const struct button_mapping button_context_usb_hid[] = {
    { ACTION_USB_HID_MODE_SWITCH_NEXT, BUTTON_OPTION|BUTTON_DOWN|BUTTON_REL, BUTTON_DOWN },
    { ACTION_USB_HID_MODE_SWITCH_NEXT, BUTTON_OPTION|BUTTON_DOWN|BUTTON_REPEAT, BUTTON_DOWN },
    { ACTION_USB_HID_MODE_SWITCH_PREV, BUTTON_OPTION|BUTTON_UP|BUTTON_REL, BUTTON_UP },
    { ACTION_USB_HID_MODE_SWITCH_PREV, BUTTON_OPTION|BUTTON_UP|BUTTON_REPEAT, BUTTON_UP },

    LAST_ITEM_IN_LIST
}; /* button_context_usb_hid */

static const struct button_mapping button_context_usb_hid_mode_multimedia[] = {
    { ACTION_USB_HID_MULTIMEDIA_VOLUME_MUTE,         BUTTON_POWER|BUTTON_REL,       BUTTON_POWER },
    { ACTION_USB_HID_MULTIMEDIA_PLAYBACK_PLAY_PAUSE, BUTTON_PLAY|BUTTON_REL,        BUTTON_PLAY },
    { ACTION_USB_HID_MULTIMEDIA_PLAYBACK_STOP,       BUTTON_HOME|BUTTON_REL,        BUTTON_HOME },
    { ACTION_USB_HID_MULTIMEDIA_PLAYBACK_TRACK_PREV, BUTTON_UP|BUTTON_REL,        BUTTON_UP },
    { ACTION_USB_HID_MULTIMEDIA_PLAYBACK_TRACK_NEXT, BUTTON_DOWN|BUTTON_REL,        BUTTON_DOWN },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_USB_HID)
}; /* button_context_usb_hid_mode_multimedia */

static const struct button_mapping button_context_usb_hid_mode_presentation[] = {
    { ACTION_USB_HID_PRESENTATION_SLIDESHOW_START, BUTTON_POWER|BUTTON_REL,       BUTTON_POWER },
    { ACTION_USB_HID_PRESENTATION_SLIDESHOW_LEAVE, BUTTON_HOME|BUTTON_REL,        BUTTON_HOME },
    { ACTION_USB_HID_PRESENTATION_SLIDE_PREV,      BUTTON_UP|BUTTON_REL,        BUTTON_UP },
    { ACTION_USB_HID_PRESENTATION_SLIDE_NEXT,      BUTTON_DOWN|BUTTON_REL,        BUTTON_DOWN },
    { ACTION_USB_HID_PRESENTATION_SLIDE_FIRST,     BUTTON_UP|BUTTON_REPEAT,     BUTTON_UP },
    { ACTION_USB_HID_PRESENTATION_SLIDE_LAST,      BUTTON_DOWN|BUTTON_REPEAT,     BUTTON_DOWN },
    { ACTION_USB_HID_PRESENTATION_SCREEN_BLACK,    BUTTON_OPTION|BUTTON_REL,      BUTTON_OPTION },
    { ACTION_USB_HID_PRESENTATION_SCREEN_WHITE,    BUTTON_OPTION|BUTTON_REPEAT,   BUTTON_OPTION },
    { ACTION_USB_HID_PRESENTATION_LINK_PREV,       BUTTON_LEFT,                   BUTTON_NONE },
    { ACTION_USB_HID_PRESENTATION_LINK_PREV,       BUTTON_LEFT|BUTTON_REPEAT,     BUTTON_NONE },
    { ACTION_USB_HID_PRESENTATION_LINK_NEXT,       BUTTON_RIGHT,                  BUTTON_NONE },
    { ACTION_USB_HID_PRESENTATION_LINK_NEXT,       BUTTON_RIGHT|BUTTON_REPEAT,    BUTTON_NONE },
    { ACTION_USB_HID_PRESENTATION_MOUSE_CLICK,     BUTTON_PLAY|BUTTON_REL,        BUTTON_PLAY },
    { ACTION_USB_HID_PRESENTATION_MOUSE_OVER,      BUTTON_PLAY|BUTTON_REPEAT,     BUTTON_PLAY },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_USB_HID)
}; /* button_context_usb_hid_mode_presentation */

#endif

/* get_context_mapping returns a pointer to one of the above defined arrays depending on the context */
const struct button_mapping* get_context_mapping(int context)
{
    switch (context)
    {
        case CONTEXT_LIST:
        case CONTEXT_STD:
            return button_context_standard;
        case CONTEXT_BOOKMARKSCREEN:
            return button_context_bmark;
        case CONTEXT_KEYBOARD:
            return button_context_keyboard;
        case CONTEXT_PITCHSCREEN:
            return button_context_pitchscreen;
        case CONTEXT_QUICKSCREEN:
            return button_context_quickscreen;
        case CONTEXT_SETTINGS:
            return button_context_settings;
        case CONTEXT_SETTINGS_TIME:
        case CONTEXT_SETTINGS_COLOURCHOOSER:
        case CONTEXT_SETTINGS_EQ:
        case CONTEXT_SETTINGS_RECTRIGGER:
            return button_context_settings_vol_is_inc;
        case CONTEXT_TREE:
                return button_context_listtree_scroll_with_combo;
        case CONTEXT_MAINMENU:
            return button_context_tree;
        case CONTEXT_CUSTOM|CONTEXT_TREE:
            return button_context_tree;
        case CONTEXT_WPS:
            return button_context_wps;
        case CONTEXT_YESNOSCREEN:
            return button_context_yesnoscreen;
#ifdef USB_ENABLE_HID
        case CONTEXT_USB_HID:
            return button_context_usb_hid;
        case CONTEXT_USB_HID_MODE_MULTIMEDIA:
            return button_context_usb_hid_mode_multimedia;
        case CONTEXT_USB_HID_MODE_PRESENTATION:
            return button_context_usb_hid_mode_presentation;
#endif
    }
    return button_context_standard;
}
