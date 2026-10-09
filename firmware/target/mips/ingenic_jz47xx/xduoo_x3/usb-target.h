/***************************************************************************
 * Xduoo X3 USB detection and OTG pins
 * Copyright (C) 2016 by Roman Stolyarov
 ****************************************************************************/
#ifndef USB_TARGET_H
#define USB_TARGET_H
#define PIN_USB_DET      (32*4+19)
#define IRQ_USB_DET      GPIO_IRQ(PIN_USB_DET)
#define GPIO_USB_DET     GPIO147
#define PIN_USB_DRVVBUS  (32*4+10)
#define PIN_USB_OTG_ID   (32*3+7)
#endif
