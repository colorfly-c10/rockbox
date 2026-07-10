/*
 * This config file is for the Colorfly C10.
 * Copyright (C) 2026 by Anton Bambura
 */

#define MODEL_NAME   "Colorfly C10"
#define MODEL_NUMBER 125

/* Support FAT16 for SD cards <= 2GB */
#define HAVE_FAT16SUPPORT

/* define this if you have access to the quickscreen */
#define HAVE_QUICKSCREEN

/* define this if you would like tagcache to build on this target */
#define HAVE_TAGCACHE

/* The C10 has ample SDRAM; keep database browsing independent of SD latency. */
#define DEFAULT_TAGCACHE_RAM 1 /* TAGCACHE_RAM_ON */

/* define this if the target has volume keys which can be used in the lists */
#define HAVE_VOLUME_IN_LIST

/*
 * Stock Linux exposes a 400x360 RGB565 framebuffer and the C10 LCDC
 * descriptors use the same 400x360 raster.  The panel GRAM window itself is
 * 360x400, but entry-mode 0x00ec rotates that stream inside the controller.
 *
 * Rockbox must use the LCDC raster here: a 360x400 row-major framebuffer has
 * the same byte count, but makes every 400-pixel LCDC line cross its rows and
 * turns text into a sheared pattern.
 */
#define LCD_WIDTH  400
#define LCD_HEIGHT 360
#define LCD_DPI    160 /* TODO: confirm physical panel size. */
#define LCD_DEPTH  16
#define LCD_PIXELFORMAT RGB565
#define HAVE_LCD_COLOR
#define HAVE_ALBUMART
#define HAVE_BMP_SCALING
#define HAVE_JPEG

/* Define this for LCD backlight available */
#define HAVE_BACKLIGHT
#define HAVE_BACKLIGHT_BRIGHTNESS

/* PWM-backed brightness control. */
#define CONFIG_BACKLIGHT_FADING BACKLIGHT_FADING_SW_HW_REG

/* The C10 has a four-way navigation pad plus Back and Play buttons. */
#define CONFIG_KEYPAD COLORFLY_C10_PAD

/* The C10 switches have a long physical travel. Avoid treating a deliberate
 * press as a hold; repeats still accelerate normally after this threshold. */
#define BUTTON_REPEAT_START 80

/* Stock kernel IRQs identify PE18 as Phone_check and PE19 as Lout_check. */
#define HAVE_HEADPHONE_DETECTION

/* Define this if a programmable hotkey is mapped */
#define HAVE_HOTKEY

#ifndef BOOTLOADER
/* define this if you have a real-time clock */
#define CONFIG_RTC RTC_JZ4760
#endif

/* Define this if you have a software controlled poweroff */
#define HAVE_SW_POWEROFF

/* The number of bytes reserved for loadable codecs */
#define CODEC_SIZE 0x100000

/* The number of bytes reserved for loadable plugins */
#define PLUGIN_BUFFER_SIZE 0x100000

#define HAVE_CS4398

/* The CS4398 has no Rockbox tone-control driver, so use software controls. */
#define HAVE_SW_TONE_CONTROLS

/* define the bitmask of hardware sample rates */
#define HW_SAMPR_CAPS (SAMPR_CAP_32 | SAMPR_CAP_44 | SAMPR_CAP_48 | \
                       SAMPR_CAP_64 | SAMPR_CAP_88 | SAMPR_CAP_96 | \
                       SAMPR_CAP_176 | SAMPR_CAP_192)

#define AB_REPEAT_ENABLE

#define BATTERY_CAPACITY_DEFAULT 3000 /* TODO: confirm C10 cell capacity. */
#define BATTERY_CAPACITY_MIN 500
#define BATTERY_CAPACITY_MAX 3000
#define BATTERY_CAPACITY_INC 100

#define CURRENT_NORMAL 167
#define CURRENT_BACKLIGHT 180
#define CURRENT_MAX_CHG 500

#define CONFIG_BATTERY_MEASURE VOLTAGE_MEASURE

/* Runtime PMIC dumps confirm the AXP202 on I2C1 address 0x34. The target
 * reads its input-status register for charging and USB-power reporting. */
#define CONFIG_CHARGING CHARGING_MONITOR
#define HAVE_USB_POWER

#define CFG_EXTAL 12000000    /* EXT clock: 12 MHz */

#define CONFIG_STORAGE STORAGE_SD
#define HAVE_MULTIDRIVE
#define HAVE_HOTSWAP
#define NUM_DRIVES 2
#define HAVE_BOOTDATA
#define BOOT_REDIR "rockbox_main.c10"
#define MULTIBOOT_MIN_VOLUME 1

/* Both media use the SD driver, but present them by their physical roles. */
#define STORAGE_VOLUME_DECORATION(drive) \
    ((drive) == 0 ? "Internal" : "microSD")

/* Only expose stock eMMC partition 4 and microSD partition 1. */
#define STORAGE_PARTITION_FILTER(drive, partition) \
    ((drive) == 0 ? (partition) == 3 : (partition) == 0)

/* Export only the Rockbox data partition from each physical drive over USB. */
#define USBSTOR_PARTITION_INDEX(drive) ((drive) == 0 ? 3 : 0)
#define USBSTOR_EXPOSE_ALL_DRIVES

/* define this if you have a flash memory storage */
#define HAVE_FLASH_STORAGE

/** Non-simulator section **/
#ifndef SIMULATOR

/* The closest supported Ingenic core in this Rockbox tree. */
#define CONFIG_CPU JZ4760B

/* If we have adjustable frequency */
#define CPUFREQ_MAX      480000000
#define CPUFREQ_MIN      120000000
#define CPUFREQ_NORMAL   240000000

#ifndef BOOTLOADER
#define HAVE_ADJUSTABLE_CPU_FREQ
//#define HAVE_GUI_BOOST
#endif

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
#define CPUFREQ_DEFAULT  CPUFREQ_MIN
#else
#define CPUFREQ_DEFAULT  CPUFREQ_MAX
#define CPU_FREQ         CPUFREQ_MAX
#endif

/* Define this if you want to use the JZ47XX i2c interface */
#define CONFIG_I2C I2C_JZ47XX

#define NEED_ADC_CLOSE 1

/* Type of LCD */
#define CONFIG_LCD LCD_COLORFLYC10

/* USB On-the-go */
#define CONFIG_USBOTG     USBOTG_JZ4760

/* Enable the Rockbox USB stack. */
#define HAVE_USBSTACK

/* Connect by events, not by tick polling */
#define USB_STATUS_BY_EVENT

/* Matches the Linux g_file_storage VID/PID used by the stock firmware. */
#define USB_VENDOR_ID 0x0525
#define USB_PRODUCT_ID 0xA4A5

#define USB_NUM_ENDPOINTS  3
#define USB_DEVBSS_ATTR    IBSS_ATTR

#define BOOTFILE_EXT "c10"
#define BOOTFILE     "rockbox." BOOTFILE_EXT
#define BOOTDIR      "/.rockbox"

#define INCLUDE_TIMEOUT_API

#define ICODE_ATTR_TREMOR_NOT_MDCT

#endif /* !SIMULATOR */

/** Port-specific settings **/

#define CONFIG_SDRAM_START 0x80004000

/* Main LCD backlight brightness range and defaults */
#define MIN_BRIGHTNESS_SETTING          1
#define MAX_BRIGHTNESS_SETTING          16
#define DEFAULT_BRIGHTNESS_SETTING      16
