/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__\/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 by Anton Bambura
 ****************************************************************************/

#include <stdint.h>
#include <string.h>

#include "config.h"
#include "cpu.h"
#include "i2c.h"
#include "kernel.h"
#include "lcd.h"
#include "system.h"

/*
 * C10 binary-verified panel table:
 *
 *   +0x00 LCD_CFG  = 0x9000000d
 *   +0x04 SLCD_CFG = 0x00001100
 *   +0x08 LCD_CTRL = 0x20000800
 *
 * jz4760fb_set_panel_mode() adds the 16-bpp value to LCD_CTRL. SlcdInit
 * then changes only the SLCD data width from 8-bit x1 to 8-bit x2.
 */
#define C10_LCD_CFG             0x9000000d
#define C10_SLCD_CFG_INIT       0x00001100
#define C10_SLCD_CFG_RUN        0x00000d00
#define C10_LCD_CTRL_INIT       0x20000804

#define C10_LCDC_WIDTH          400
#define C10_LCDC_HEIGHT         360
#define C10_LCD_FPS             60
#define C10_LCD_PIXCLOCK        (C10_LCD_FPS * C10_LCDC_WIDTH * \
                                 C10_LCDC_HEIGHT)
#define C10_LCDC_TIMING         ((C10_LCDC_WIDTH << 16) | C10_LCDC_HEIGHT)
#define C10_LCDC_FG_SIZE        ((C10_LCDC_HEIGHT << 16) | C10_LCDC_WIDTH)
#define C10_FB_BYTES            (C10_LCDC_WIDTH * C10_LCDC_HEIGHT * \
                                 sizeof(fb_data))
#define C10_LCD_DMA_WORDS       (C10_FB_BYTES / 4)

/* The framebuffer layout must match the stock LCDC descriptor raster. */
#if LCD_WIDTH != C10_LCDC_WIDTH || LCD_HEIGHT != C10_LCDC_HEIGHT
#error "Colorfly C10 LCD framebuffer must be 400x360 for LCDC scanout"
#endif

#define C10_RAM_HSTART          0x001e
#define C10_RAM_HEND            0x0185
#define C10_RAM_VSTART          0x0000
#define C10_RAM_VEND            0x018f

#define C10_LCD_GPIOC_SLCD_MASK 0x000c30fc
#define C10_PANEL_PC8           (1 << 8)
#define C10_PANEL_PC9           (1 << 9)
#define C10_PANEL_PE2           (1 << 2)

#define C10_PANEL_POWER_SETTLE_MS 120

#define C10_AXP202_ADDR          0x34
#define C10_AXP202_OUTPUT        0x12
#define C10_AXP202_DCDC3         0x27
#define C10_AXP202_LDO3          0x29
#define C10_AXP202_2V8           0x54
#define C10_AXP202_3V3           0x68
#define C10_AXP202_OUTPUT_DCDC3  (1 << 1)
#define C10_AXP202_OUTPUT_LDO3   (1 << 6)
#define C10_AXP202_LCD_OUTPUTS   (C10_AXP202_OUTPUT_DCDC3 | \
                                  C10_AXP202_OUTPUT_LDO3)

#define C10_LCD_FID_FRAME        0x0000da00
#define C10_LCD_FID_FG1          0x0000da10
#define C10_LCD_FID_CMD0         0x0da0cad0
#define C10_LCD_FID_CMD          0x0da000cd
#define C10_LCD_IPUR             0x80001000
#define C10_DESC_ADDR(x)         PHYSADDR((unsigned long)(x))

struct c10_panel_reg {
    uint16_t reg;
    uint16_t value;
};

struct c10_lcd_dma_desc {
    uint32_t next;
    uint32_t source;
    uint32_t fid;
    uint32_t cmd;
    uint32_t offsize;
    uint32_t page_width;
    uint32_t cpos;
    uint32_t dessize;
} __attribute__((aligned(32)));

static bool lcd_is_on;
static bool c10_i2c_ready;

static struct c10_lcd_dma_desc c10_lcd_desc[4];

/* LCDC must not scan Rockbox's live drawing surface: normal drawing can
 * evict half-composed lines from cache before lcd_update_rect() is called.
 * Keep a DMA-only completed-frame copy so unrelated regions never flash. */
static fb_data c10_scanout[LCD_HEIGHT][LCD_WIDTH]
    __attribute__((aligned(32)));

/* The C10 kernel allocates and zeroes this command area but never fills it. */
static uint32_t c10_lcd_cmd_buf[3] __attribute__((aligned(32)));

static const struct c10_panel_reg c10_panel_init[] = {
    { 0x0001, 0x0004 }, { 0x0002, 0x0100 }, { 0x0003, 0x00ec },
    { 0x0005, 0x0000 }, { 0x0006, 0x0000 }, { 0x0007, 0x0103 },
    { 0x0008, 0x0303 }, { 0x000d, 0x0000 }, { 0x0010, 0x00c1 },
    { 0x0011, 0xb108 }, { 0x0012, 0xb108 }, { 0x0013, 0x000f },
    { 0x0014, 0x0014 }, { 0x0015, 0x0004 }, { 0x0016, 0x0000 },
    { 0x0022, 0x0000 }, { 0x0023, 0x0000 }, { 0x0030, 0x7c3f },
    { 0x0032, 0x0000 }, { 0x0070, 0x0001 }, { 0x0091, 0x0100 },
    { 0x00e0, 0x0001 }, { 0x00e1, 0x0061 }, { 0x0100, 0x1030 },
    { 0x0101, 0xc633 }, { 0x0102, 0x501f }, { 0x0103, 0x0025 },
    { 0x0108, 0x0360 }, { 0x0111, 0x0001 }, { 0x0135, 0x7666 },
    { 0x0139, 0x0026 }, { 0x0400, 0x00c7 }, { 0x0401, 0x0000 },
    { 0x0606, 0x0000 }, { 0x0300, 0x0d0e }, { 0x0301, 0x0003 },
    { 0x0302, 0x0808 }, { 0x0303, 0x0201 }, { 0x0304, 0x0301 },
    { 0x0305, 0x0004 }, { 0x0306, 0x1b21 }, { 0x0307, 0x0f0e },
    { 0x0308, 0x0104 }, { 0x0309, 0x0808 }, { 0x030a, 0x0201 },
    { 0x030b, 0x0301 }, { 0x030c, 0x0003 }, { 0x030d, 0x3134 },
    { 0x0580, 0x0000 }, { 0x0210, C10_RAM_HSTART },
    { 0x0211, C10_RAM_HEND }, { 0x0212, C10_RAM_VSTART },
    { 0x0213, C10_RAM_VEND }, { 0x0200, C10_RAM_HEND },
    { 0x0201, C10_RAM_VSTART },
};

static void c10_lcd_start_support_clocks(void)
{
    /*
     * The C10 kernel explicitly starts IPU clock 0x1d. Its running clock-gate
     * snapshot also has LCDC's OSD/AUX side and both DMA blocks ungated. A
     * direct ROM START2 has no vendor U-Boot handoff, so own all of them here.
     * In particular, LCDC must be ungated before any LCD/SLCD MMIO; Linux gets
     * that handoff from U-Boot, while the ROM START2 path does not.
     */
    __cpm_start_lcd();
    __cpm_start_ipu();
    __cpm_start_mdma();
    __cpm_start_dmac();
    REG_CPM_CLKGR1 &= ~(CLKGR1_AUX | CLKGR1_OSD);
}

static void c10_lcd_set_pixel_clock(void)
{
    unsigned int pll = __cpm_get_pllout2();
    unsigned int pixdiv = pll / C10_LCD_PIXCLOCK;

    if (pixdiv != 0)
        pixdiv--;
    if (pixdiv > 0x7ff)
        pixdiv = 0x7ff;

    /* This is jz4760fb_change_clock() for a non-TVE 400x360@60 panel. */
    __cpm_stop_lcd();
    REG_CPM_LPCDR &= ~(LPCDR_PIXDIV_MASK | LPCDR_LTCS | LPCDR_LPCS);
    REG_CPM_LPCDR |= pixdiv << LPCDR_PIXDIV_LSB;
    REG_CPM_CPCCR |= CPCCR_CE;
    __cpm_start_lcd();
    udelay(1000);
}

static int c10_axp202_read(unsigned char reg)
{
    unsigned char value;

    if (c10_i2c1_read_reg(C10_AXP202_ADDR, reg, &value) < 0)
        return -1;

    return value;
}

static int c10_axp202_write(unsigned char reg, unsigned char value)
{
    return c10_i2c1_write_reg(C10_AXP202_ADDR, reg, value);
}

static void c10_panel_power_init(void)
{
    int dcdc3;
    int ldo3;
    int outputs;

    if (!c10_i2c_ready) {
        i2c_init();
        c10_i2c_ready = true;
    }

    /* U-Boot initially selects 2.8 V for both outputs, but the stock kernel
     * immediately raises DCDC3 to 3.3 V while leaving the panel's separate
     * LDO3 at 2.8 V. DCDC3 is the shared peripheral rail; leaving it at the
     * U-Boot value makes MSC2 reads lose CRC margin when the audio relay
     * switches. Reproduce the final stock-kernel state before storage starts. */
    if (c10_axp202_write(C10_AXP202_DCDC3, C10_AXP202_3V3) < 0)
        return;

    outputs = c10_axp202_read(C10_AXP202_OUTPUT);
    if (outputs < 0)
        return;

    /* U-Boot sets LDO3; START2 must additionally own DCDC3's enable bit. */
    if (c10_axp202_write(C10_AXP202_OUTPUT,
                         outputs | C10_AXP202_LCD_OUTPUTS) < 0)
        return;

    if (c10_axp202_write(C10_AXP202_LDO3, C10_AXP202_2V8) < 0)
        return;

    dcdc3 = c10_axp202_read(C10_AXP202_DCDC3);
    ldo3 = c10_axp202_read(C10_AXP202_LDO3);
    outputs = c10_axp202_read(C10_AXP202_OUTPUT);
    if (dcdc3 < 0 || (dcdc3 & 0x7f) != C10_AXP202_3V3 ||
        ldo3 < 0 || (ldo3 & 0x7f) != C10_AXP202_2V8 ||
        outputs < 0 ||
        (outputs & C10_AXP202_LCD_OUTPUTS) != C10_AXP202_LCD_OUTPUTS)
        return;

    mdelay(C10_PANEL_POWER_SETTLE_MS);
}

static void c10_lcdc_quiesce(void)
{
    REG_LCD_CTRL &= ~LCD_CTRL_ENA;
    REG_SLCD_CTRL = 0;
    REG_LCD_STATE = 0;
}

static void c10_lcd_configure_slcd_pins(void)
{
    /* Exact reduced bank-C SLCD mask selected by the C10 kernel binary. */
    REG_GPIO_PXFUNS(2) = C10_LCD_GPIOC_SLCD_MASK;
    REG_GPIO_PXTRGC(2) = C10_LCD_GPIOC_SLCD_MASK;
    REG_GPIO_PXSELC(2) = C10_LCD_GPIOC_SLCD_MASK;
    REG_GPIO_PXPES(2) = C10_LCD_GPIOC_SLCD_MASK;
}

static void c10_panel_control_pulse(void)
{
    /* Exact PC9-low and PE2 high-low-high sequence from ctrl_enable(). */
    REG_LCD_STATE = 0;

    REG_GPIO_PXFUNS(2) = C10_PANEL_PC9;
    REG_GPIO_PXTRGC(2) = C10_PANEL_PC9;
    REG_GPIO_PXSELC(2) = C10_PANEL_PC9;
    REG_GPIO_PXFUNC(2) = C10_PANEL_PC9;
    REG_GPIO_PXSELC(2) = C10_PANEL_PC9;
    REG_GPIO_PXDIRS(2) = C10_PANEL_PC9;
    REG_GPIO_PXPES(2) = C10_PANEL_PC9;
    REG_GPIO_PXDATC(2) = C10_PANEL_PC9;

    REG_GPIO_PXFUNC(4) = C10_PANEL_PE2;
    REG_GPIO_PXSELC(4) = C10_PANEL_PE2;
    REG_GPIO_PXDIRS(4) = C10_PANEL_PE2;
    REG_GPIO_PXPES(4) = C10_PANEL_PE2;
    REG_GPIO_PXDATS(4) = C10_PANEL_PE2;

    udelay(2000);
    REG_GPIO_PXDATC(4) = C10_PANEL_PE2;
    mdelay(10);
    REG_GPIO_PXDATS(4) = C10_PANEL_PE2;
    mdelay(10);

    REG_LCD_CTRL &= ~(LCD_CTRL_ENA | LCD_CTRL_DIS);
}

static void c10_panel_post_enable_gpio_init(void)
{
    /* Exact PC8 input sequence immediately following ctrl_enable(). */
    REG_GPIO_PXPEC(2) = C10_PANEL_PC8;
    REG_GPIO_PXFUNC(2) = C10_PANEL_PC8;
    REG_GPIO_PXSELC(2) = C10_PANEL_PC8;
    REG_GPIO_PXDIRC(2) = C10_PANEL_PC8;
}

static inline void c10_slcd_wait(void)
{
    while (REG_SLCD_STATE & SLCD_STATE_BUSY)
        ;
}

static inline void c10_slcd_write_cmd8(uint8_t value)
{
    c10_slcd_wait();
    REG_SLCD_DATA = SLCD_DATA_RS_COMMAND | value;
}

static inline void c10_slcd_write_data8(uint8_t value)
{
    c10_slcd_wait();
    REG_SLCD_DATA = value;
}

static void c10_slcd_write_cmd16(uint16_t value)
{
    c10_slcd_write_cmd8(value >> 8);
    c10_slcd_write_cmd8(value & 0xff);
}

static void c10_slcd_write_data16(uint16_t value)
{
    c10_slcd_write_data8(value >> 8);
    c10_slcd_write_data8(value & 0xff);
}

static void c10_slcd_write_reg16(uint16_t reg, uint16_t value)
{
    c10_slcd_write_cmd16(reg);
    c10_slcd_write_data16(value);
}

static void c10_panel_delay_after_reg(uint16_t reg)
{
    switch (reg) {
    case 0x0016:
    case 0x0111:
    case 0x0139:
        mdelay(2);
        break;
    case 0x0302:
        mdelay(100);
        break;
    default:
        break;
    }
}

static void c10_panel_register_init(void)
{
    unsigned int i;

    /* U-Boot issues four 16-bit zero commands, then command 0x00be. */
    for (i = 0; i < 4; i++)
        c10_slcd_write_cmd16(0x0000);
    c10_slcd_write_cmd16(0x00be);
    c10_slcd_write_data16(0xc329);

    for (i = 0; i < ARRAYLEN(c10_panel_init); i++) {
        c10_slcd_write_reg16(c10_panel_init[i].reg,
                             c10_panel_init[i].value);
        c10_panel_delay_after_reg(c10_panel_init[i].reg);

        /* This cold-only padding exists in stock U-Boot, not the kernel. */
        if (c10_panel_init[i].reg == 0x0580) {
            c10_slcd_write_cmd16(0x0000);
            c10_slcd_write_cmd16(0x0000);
            c10_slcd_write_cmd16(0x0000);
            c10_slcd_write_cmd16(0x0000);
        }
    }

    c10_slcd_write_cmd16(0x0202);
    mdelay(20);
}

static void c10_lcd_build_descriptors(void)
{
    /* One-shot command -> frame -> zero-length dummy -> frame. */
    c10_lcd_desc[0].next = C10_DESC_ADDR(&c10_lcd_desc[1]);
    c10_lcd_desc[0].source = C10_DESC_ADDR(c10_lcd_cmd_buf);
    c10_lcd_desc[0].fid = C10_LCD_FID_CMD0;
    c10_lcd_desc[0].cmd = LCD_CMD_CMD | ARRAYLEN(c10_lcd_cmd_buf);
    c10_lcd_desc[0].offsize = 0;
    c10_lcd_desc[0].page_width = 0;
    c10_lcd_desc[0].cpos = ARRAYLEN(c10_lcd_cmd_buf);
    c10_lcd_desc[0].dessize = 0;

    c10_lcd_desc[1].next = C10_DESC_ADDR(&c10_lcd_desc[2]);
    c10_lcd_desc[1].source = C10_DESC_ADDR(&c10_scanout[0][0]);
    c10_lcd_desc[1].fid = C10_LCD_FID_FRAME;
    c10_lcd_desc[1].cmd = C10_LCD_DMA_WORDS;
    c10_lcd_desc[1].offsize = 0;
    c10_lcd_desc[1].page_width = 0;
    c10_lcd_desc[1].cpos = 0;
    c10_lcd_desc[1].dessize = C10_LCDC_FG_SIZE;

    c10_lcd_desc[2].next = C10_DESC_ADDR(&c10_lcd_desc[1]);
    c10_lcd_desc[2].source = 0;
    c10_lcd_desc[2].fid = C10_LCD_FID_CMD;
    c10_lcd_desc[2].cmd = LCD_CMD_CMD;
    c10_lcd_desc[2].offsize = 0;
    c10_lcd_desc[2].page_width = 0;
    c10_lcd_desc[2].cpos = 0;
    c10_lcd_desc[2].dessize = 0;

    /* The kernel also gives disabled foreground 1 a valid descriptor. */
    c10_lcd_desc[3].next = C10_DESC_ADDR(&c10_lcd_desc[3]);
    c10_lcd_desc[3].source = C10_DESC_ADDR(&c10_scanout[0][0]);
    c10_lcd_desc[3].fid = C10_LCD_FID_FG1;
    c10_lcd_desc[3].cmd = C10_LCD_DMA_WORDS;
    c10_lcd_desc[3].offsize = 0;
    c10_lcd_desc[3].page_width = 0;
    c10_lcd_desc[3].cpos = 0;
    c10_lcd_desc[3].dessize = C10_LCDC_FG_SIZE;

    commit_discard_dcache_range(c10_lcd_cmd_buf, sizeof(c10_lcd_cmd_buf));
    commit_discard_dcache_range(c10_lcd_desc, sizeof(c10_lcd_desc));

    REG_LCD_DA0 = C10_DESC_ADDR(&c10_lcd_desc[0]);
    REG_LCD_DA1 = C10_DESC_ADDR(&c10_lcd_desc[3]);
}

static void c10_lcd_program_panel_mode(void)
{
    /* Preserve the store order in jz4760fb_set_panel_mode(). */
    REG_LCD_CTRL = C10_LCD_CTRL_INIT;
    REG_LCD_CFG = C10_LCD_CFG;
    REG_SLCD_CFG = C10_SLCD_CFG_INIT;
    REG_SLCD_CTRL = SLCD_CTRL_DMA_EN;

    REG_LCD_VAT = C10_LCDC_TIMING;
    REG_LCD_DAH = C10_LCDC_WIDTH;
    REG_LCD_DAV = C10_LCDC_HEIGHT;
    REG_LCD_HSYNC = 0;
    REG_LCD_VSYNC = 0;
    REG_LCD_PS = 0;
    REG_LCD_CLS = 0;
    REG_LCD_SPL = 0;
    REG_LCD_REV = 0;
}

static void c10_lcd_program_osd_mode(void)
{
    REG_LCD_OSDC = LCD_OSDC_OSDEN | LCD_OSDC_F0EN;
    REG_LCD_OSDCTRL = LCD_OSDCTRL_OSDBPP_16;
    REG_LCD_RGBC = 0;
    REG_LCD_BGC = 0;
    REG_LCD_KEY0 = 0;
    REG_LCD_KEY1 = 0;
    REG_LCD_ALPHA = 0xa0;
    REG_LCD_IPUR = C10_LCD_IPUR;

    REG_LCD_XYP0 = 0;
    REG_LCD_XYP1 = 0;
    REG_LCD_SIZE0 = C10_LCDC_FG_SIZE;
    REG_LCD_SIZE1 = C10_LCDC_FG_SIZE;
}

static void c10_panel_enable(void)
{
    c10_panel_control_pulse();
    c10_panel_register_init();

    /* The programming manual requires the PIO bus idle before DMA takeover. */
    c10_slcd_wait();
    REG_SLCD_CTRL |= SLCD_CTRL_DMA_EN;
    REG_SLCD_CFG = C10_SLCD_CFG_RUN;
    REG_LCD_CTRL &= ~LCD_CTRL_DIS;
    REG_LCD_CTRL |= LCD_CTRL_ENA;

    c10_panel_post_enable_gpio_init();
}

void lcd_init_device(void)
{
    c10_lcd_start_support_clocks();
    c10_panel_power_init();

    c10_lcdc_quiesce();
    c10_lcd_configure_slcd_pins();
    c10_lcd_build_descriptors();
    c10_lcd_program_panel_mode();
    c10_lcd_program_osd_mode();

    /* deep_set_mode() starts the pixel clock and briefly enables LCDC. */
    c10_lcd_set_pixel_clock();
    REG_LCD_STATE = 0;
    REG_LCD_CTRL |= LCD_CTRL_ENA;

    /* ctrl_enable() then resets/programs the panel and starts scanout. */
    c10_panel_enable();

    lcd_is_on = true;
    lcd_update();
}

void lcd_update(void)
{
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

void lcd_update_rect(int x, int y, int width, int height)
{
    if (!lcd_is_on)
        return;

    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > LCD_WIDTH) width = LCD_WIDTH - x;
    if (y + height > LCD_HEIGHT) height = LCD_HEIGHT - y;
    if (width <= 0 || height <= 0)
        return;

    /* Publish only completed Rockbox drawing into the DMA scanout surface. */
    if (x == 0 && width == LCD_WIDTH)
    {
        memcpy(&c10_scanout[y][0], FBADDR(0, y),
               width * height * sizeof(fb_data));
        commit_dcache_range(&c10_scanout[y][0],
                            width * height * sizeof(fb_data));
    }
    else
        for (int row = 0; row < height; ++row)
        {
            memcpy(&c10_scanout[y + row][x], FBADDR(x, y + row),
                   width * sizeof(fb_data));
            commit_dcache_range(&c10_scanout[y + row][x],
                                width * sizeof(fb_data));
        }
}

bool lcd_active(void)
{
    return lcd_is_on;
}
