/***************************************************************************
 * Colorfly C10 USB detection
 * Copyright (C) 2026 by Anton Bambura
 ****************************************************************************/
#ifndef USB_TARGET_H
#define USB_TARGET_H
/* The AXP202 worker caches VBUS state: usb_detect() also runs with IRQs off. */
int c10_usb_detect(void);
#define USB_DETECT_TARGET c10_usb_detect
#endif
