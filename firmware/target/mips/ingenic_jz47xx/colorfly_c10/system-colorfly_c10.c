/***************************************************************************
 * Colorfly C10 BootROM/stock bootloader USB PHY handoff
 * Copyright (C) 2026 by Anton Bambura
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "cpu.h"
#include "system-board.h"

void c10_usb_preinit(void)
{
    /* START2 enters directly from the USB BootROM with MUSB still connected.
     * Quiesce it before changing its clock or resetting the PHY. */
    REG_USB_POWER &= ~USB_POWER_SOFTCONN;
    REG_USB_INTRINE = 0;
    REG_USB_INTROUTE = 0;
    REG_USB_INTRUSBE = 0;
    REG_USB_CNTL(USB_INTR_DMA_BULKIN) = 0;
    REG_USB_CNTL(USB_INTR_DMA_BULKOUT) = 0;
    /* START2 inherits BootROM's active MUSB session. Stock musb_start()
     * clears SESSION before entering its fixed peripheral role. */
    REG_USB_DEVCTL = 0;

    /* Clear ECS bit of CPCCR, 0:clock source is EXCLK, 1:clock source is EXCLK/2 */
    REG_CPM_CPCCR &= ~CPCCR_ECS;

    /* Clear all bits of USBCDR, 0:OTG clock source is pin EXCLK, PLL0 output, divider = 1:12MHZ */
    REG_CPM_USBCDR = 0;

    /* Set CE bit of CPCCR, it means frequence is changed immediately */
    REG_CPM_CPCCR |= CPCCR_CE;

    /* Stock cpm_set_clock() is followed by a 100 us settling delay. */
    udelay(100);

    /* Clear OTG bit of CLKGR0, 0:device can be accessed */
    REG_CPM_CLKGR0 &= ~CLKGR0_OTG;

    /* fil */
    REG_CPM_USBVBFIL = 0x80;

    /* rdt */
    /* The stock C10 kernel programs 0x196 before enabling FILLOAD. This
     * timer is derived from EXCLK, not from Rockbox's current CPU DVFS rate. */
    REG_CPM_USBRDT = 0x196;

    /* rdt - filload_en */
    REG_CPM_USBRDT |= (1 << 25);

    /* TXRISETUNE & TXVREFTUNE. */
    /* A direct BootROM handoff does not provide the reset-state upper bits
     * assumed by the vendor kernel. Use the complete value measured while
     * the stock high-speed gadget was working. */
    REG_CPM_USBPCR = 0x439921b5;

    /* Device Mode. */
    REG_CPM_USBPCR &= ~(1 << 31);
    REG_CPM_USBPCR |= USBPCR_VBUSVLDEXT;

    /* phy reset */
    REG_CPM_USBPCR |= USBPCR_POR;
    udelay(30);
    REG_CPM_USBPCR &= ~USBPCR_POR;
    udelay(300);

    /* Enable the USB PHY */
    REG_CPM_OPCR |= OPCR_OTGPHY_ENABLE;

    /* Wait PHY Clock Stable. */
    udelay(300);
}
