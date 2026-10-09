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

#include "config.h"
#include "system.h"
#include "cpu.h"
#include "button.h"
#include "button-target.h"
#include "powermgmt.h"
#ifndef BOOTLOADER
#include "codec-colorfly-c10.h"
#endif
#include "kernel.h"
#include "adc.h"
#include "i2c-target.h"
#ifndef BOOTLOADER
#include "sound.h"
#include "thread.h"
#include "usb.h"
#endif

/* GPIO key table recovered from the stock firmware. Power-key edges come
 * from the AXP202 PEK input through the PMIC IRQ on PD6.
 * Stock Linux IRQ 166 is GPIO IRQ base 64 + GPIO102 (PD6), not GPIO166.
 * Stock's separate PA22 path is only its delayed hard-poweroff handler. */
#define PIN_BTN_UP    (32*1+6)
#define PIN_BTN_LEFT  (32*1+7)
#define PIN_BTN_RIGHT (32*1+8)
#define PIN_BTN_DOWN  (32*1+9)
#define PIN_BTN_PAUSE (32*4+11)
#define PIN_BTN_ESC   (32*4+12)
#define PIN_BTN_ENTER (32*4+13)
#define PIN_PHONE_CHECK (32*4+18)
#define PIN_LINEOUT_CHECK (32*4+19)
#define PIN_AXP202_IRQ (32*3+6)

#define BTN_GPIO_MASK(pin) (1u << ((pin) & 31))

/* AXP202 is confirmed by stock Linux on I2C1 at 0x34. Its 12-bit battery
 * ADC is 1.1 mV/LSB, held in registers 0x78/0x79. */
#define C10_AXP202_BAT_V_H    0x78
#define C10_AXP202_BAT_V_L    0x79
#define C10_AXP202_OUTPUT       0x12
#define C10_AXP202_LDO24        0x28
#define C10_AXP202_GPIO0_ADC_H  0x64
#define C10_AXP202_GPIO0_CTRL   0x90
#define C10_AXP202_ADC_ENABLE2  0x83
#define C10_AXP202_ADC_RATE     0x84
#define C10_AXP202_PEK_CONTROL    0x36
#define C10_AXP202_INPUT_STATUS   0x00
#define C10_AXP202_VBUS_PRESENT   (1u << 5)
#define C10_AXP202_IRQ1_EN        0x40
#define C10_AXP202_IRQ2_EN        0x41
#define C10_AXP202_IRQ3_EN       0x42
#define C10_AXP202_IRQ4_EN        0x43
#define C10_AXP202_IRQ5_EN       0x44
#define C10_AXP202_IRQ_STATUS    0x48
#define C10_AXP202_IRQ1_STATUS   0x48
#define C10_AXP202_IRQ5_STATUS   0x4c
#define C10_AXP202_IRQ_COUNT     5
#define C10_AXP202_VBUS_IRQS      0x0c
#define C10_AXP202_PEK_RELEASE   (1u << 6)
#define C10_AXP202_PEK_PRESS     (1u << 5)

#define C10_JACK_DEBOUNCE_TICKS HZ              /* stock uses 1000 ms */
#define C10_VOLUME_POLL_TICKS   (HZ >= 50 ? HZ / 50 : 1)

static int battery_mv = 3700;
static long battery_next_tick;
static volatile bool power_pressed;
#ifndef BOOTLOADER
static volatile int usb_state = USB_EXTRACTED;
#endif
#ifndef BOOTLOADER
static struct semaphore power_wakeup;
static long power_stack[DEFAULT_STACK_SIZE / sizeof(long)];
static unsigned int jack_stable;
static unsigned int jack_pending;
static long jack_pending_since;
static bool jack_state_valid;

/* Tables recovered from the stock kernel's proc_adc_volume(). The physical
 * slider is intentionally non-linear and covers about -54 dB through 0 dB. */
static const unsigned short volume_thresholds[] = {
    0x7c6, 0x7bc, 0x79e, 0x76c, 0x758, 0x73a, 0x71c, 0x708, 0x6f4,
    0x6e0, 0x6c7, 0x6a4, 0x67c, 0x65e, 0x640, 0x622, 0x604, 0x5e1,
    0x5b9, 0x59b, 0x57d, 0x55f, 0x537, 0x50a, 0x4ec, 0x4ce, 0x4b0,
    0x488, 0x465, 0x44c, 0x424, 0x401, 0x3e3, 0x3b6, 0x39b, 0x375,
    0x334, 0x307, 0x2c9, 0x28f, 0x244, 0x208, 0x1cc, 0x195, 0x14f,
    0x118, 0x0dc, 0x0a5, 0x064, 0x00a, 0x000
};

static const unsigned char volume_attenuation[] = {
    0x6c, 0x6a, 0x68, 0x66, 0x64, 0x63, 0x62, 0x61, 0x60, 0x5f, 0x5e,
    0x5d, 0x5b, 0x5a, 0x58, 0x56, 0x54, 0x52, 0x50, 0x4f, 0x4e, 0x4c,
    0x4a, 0x48, 0x46, 0x45, 0x43, 0x41, 0x3f, 0x3d, 0x3b, 0x39, 0x36,
    0x33, 0x31, 0x2e, 0x2b, 0x29, 0x27, 0x24, 0x21, 0x1e, 0x1b, 0x19,
    0x16, 0x13, 0x10, 0x0d, 0x0a, 0x05, 0x00
};

static long volume_stack[DEFAULT_STACK_SIZE / sizeof(long)];
extern bool audio_is_initialized;
#endif

#ifndef BOOTLOADER
static void NORETURN_ATTR c10_power_thread(void);

int c10_usb_detect(void)
{
    return usb_state;
}

static void c10_usb_update(bool notify)
{
    int input_status = c10_axp202_read(C10_AXP202_INPUT_STATUS);

    if (input_status < 0)
        return;

    usb_state = (input_status & C10_AXP202_VBUS_PRESENT) ?
        USB_INSERTED : USB_EXTRACTED;
    if (notify)
        usb_status_event(usb_state);
}
#endif

static void gpio_input_nopull(int pin)
{
    __gpio_as_input(pin);
    __gpio_disable_pull(pin);
}

static void gpio_input_pullup(int pin)
{
    __gpio_as_input(pin);
    __gpio_enable_pull(pin);
}

void button_init_device(void)
{
    gpio_input_pullup(PIN_BTN_UP);
    gpio_input_pullup(PIN_BTN_LEFT);
    gpio_input_pullup(PIN_BTN_RIGHT);
    gpio_input_pullup(PIN_BTN_DOWN);
    gpio_input_pullup(PIN_BTN_PAUSE);
    gpio_input_pullup(PIN_BTN_ESC);
    gpio_input_pullup(PIN_BTN_ENTER);
    gpio_input_nopull(PIN_PHONE_CHECK);
    gpio_input_nopull(PIN_LINEOUT_CHECK);
    gpio_input_nopull(PIN_AXP202_IRQ);

#ifndef BOOTLOADER
    semaphore_init(&power_wakeup, 1, 0);
    create_thread(c10_power_thread, power_stack, sizeof(power_stack), 0,
                  "power key" IF_PRIO(, PRIORITY_USER_INTERFACE)
                  IF_COP(, CPU));

    /* Retain stock PEK timing, including the 8-second hardware cutoff.
     * IRQ5 press/release edges report the held state to Rockbox; the stock
     * IRQ3 short/long notifications only synthesize fixed-duration pulses. */
    c10_axp202_write(C10_AXP202_PEK_CONTROL, 0xbe);
    c10_axp202_write(C10_AXP202_IRQ1_EN, 0x00);
    c10_axp202_write(C10_AXP202_IRQ2_EN, 0x00);
    c10_axp202_write(C10_AXP202_IRQ3_EN, 0x00);
    c10_axp202_write(C10_AXP202_IRQ4_EN, 0x00);
    c10_axp202_write(C10_AXP202_IRQ5_EN, 0x00);

    /* Stock gets cable state from AXP202 input-status bit 5. Cache it while
     * I2C and IRQs are available; usb_detect() itself must remain atomic. */
    c10_usb_update(false);

    for (int i = 0; i < C10_AXP202_IRQ_COUNT; ++i)
        c10_axp202_write(C10_AXP202_IRQ_STATUS + i, 0xff);

    __gpio_as_irq_fall_edge(PIN_AXP202_IRQ);
    __gpio_disable_pull(PIN_AXP202_IRQ);
    system_enable_irq(GPIO_IRQ(PIN_AXP202_IRQ));

    /* Enable events only after clearing status and installing the PD6
     * handler, preventing an old PMIC event from holding the line low. */
    c10_axp202_write(C10_AXP202_IRQ1_EN, 0x0c);
    c10_axp202_write(C10_AXP202_IRQ2_EN, 0xcc);
    c10_axp202_write(C10_AXP202_IRQ5_EN,
                     C10_AXP202_PEK_PRESS | C10_AXP202_PEK_RELEASE);
#endif
}

bool button_hold(void)
{
    return false;
}

int button_read_device(void)
{
    int btn = BUTTON_NONE;
    int raw_keys = BUTTON_NONE;
    unsigned int keys_b = ~REG_GPIO_PXPIN(1);
    unsigned int keys_e = ~REG_GPIO_PXPIN(4);

#ifndef BOOTLOADER
    unsigned int jack = (__gpio_get_pin(PIN_PHONE_CHECK) ? 1u : 0u) |
                        (__gpio_get_pin(PIN_LINEOUT_CHECK) ? 2u : 0u);

    if (!jack_state_valid)
    {
        jack_stable = jack_pending = jack;
        jack_state_valid = true;
    }
    else if (jack != jack_pending)
    {
        jack_pending = jack;
        jack_pending_since = current_tick;
    }
    else if (jack_stable != jack_pending &&
             TIME_AFTER(current_tick, jack_pending_since +
                        C10_JACK_DEBOUNCE_TICKS))
    {
        jack_stable = jack_pending;
        c10_audio_jack_changed();
    }
#endif

    if (power_pressed)
        btn |= BUTTON_POWER;
    if (keys_b & BTN_GPIO_MASK(PIN_BTN_UP))
        raw_keys |= BUTTON_UP;
    if (keys_b & BTN_GPIO_MASK(PIN_BTN_DOWN))
        raw_keys |= BUTTON_DOWN;
    if (keys_b & BTN_GPIO_MASK(PIN_BTN_LEFT))
        raw_keys |= BUTTON_LEFT;
    if (keys_b & BTN_GPIO_MASK(PIN_BTN_RIGHT))
        raw_keys |= BUTTON_RIGHT;
    /* The stock GPIO labels are Enter=PE13 and Esc=PE12, but the C10 front
     * panel is wired the other way around: physical Esc selects and physical
     * Enter cancels. Match the device's observed controls, not the labels. */
    if (keys_e & BTN_GPIO_MASK(PIN_BTN_ENTER))
        raw_keys |= BUTTON_HOME;
    if (keys_e & BTN_GPIO_MASK(PIN_BTN_ESC))
        raw_keys |= BUTTON_PLAY;
    if (keys_e & BTN_GPIO_MASK(PIN_BTN_PAUSE))
        raw_keys |= BUTTON_OPTION;

    btn |= raw_keys;

    return btn;
}

bool headphones_inserted(void)
{
    /* Stock ctrl_phone_sound() selects the only active PE16 route when
     * Phone_check/PE18 is low and Lout_check/PE19 is high. */
#ifndef BOOTLOADER
    return jack_state_valid ? !(jack_stable & 1u) :
                              !__gpio_get_pin(PIN_PHONE_CHECK);
#else
    return !__gpio_get_pin(PIN_PHONE_CHECK);
#endif
}

/* TODO: replace these xDuoo-derived Li-ion curves with C10 measurements. */
unsigned short battery_level_disksafe = 3634;
unsigned short battery_level_shutoff = 3307;

unsigned short percent_to_volt_discharge[11] =
    { 3300, 3652, 3704, 3730, 3753, 3786, 3836, 3906, 3973, 4061, 4160 };

#if CONFIG_CHARGING
unsigned short percent_to_volt_charge[11] =
    { 3444, 3827, 3893, 3909, 3931, 4001, 4067, 4150, 4206, 4207, 4208 };
#endif

#ifndef BOOTLOADER
static void NORETURN_ATTR c10_power_thread(void)
{
    while (true)
    {
        semaphore_wait(&power_wakeup, TIMEOUT_BLOCK);

        int key_status = 0;
        int usb_irq_status = 0;
        bool read_failed = false;

        /* The PMIC IRQ output remains low until every pending status byte is
         * acknowledged. Stock reads and writes back all five bytes before
         * re-enabling the PD6 IRQ. Decode IRQ1 for USB and IRQ5 for PEK. */
        for (int i = 0; i < C10_AXP202_IRQ_COUNT; ++i)
        {
            int status = c10_axp202_read(C10_AXP202_IRQ_STATUS + i);

            if (status < 0)
            {
                read_failed = true;
                break;
            }

            if (C10_AXP202_IRQ_STATUS + i == C10_AXP202_IRQ5_STATUS)
                key_status = status;
            if (C10_AXP202_IRQ_STATUS + i == C10_AXP202_IRQ1_STATUS)
                usb_irq_status = status;
            if (status != 0)
                c10_axp202_write(C10_AXP202_IRQ_STATUS + i, status);
        }

        if (read_failed)
        {
            /* Keep the active-low IRQ masked and retry after the I2C bus has
             * had a tick to recover; otherwise no new falling edge can occur. */
            sleep(1);
            semaphore_release(&power_wakeup);
            continue;
        }

        if (key_status & C10_AXP202_PEK_PRESS)
            power_pressed = true;
        if (key_status & C10_AXP202_PEK_RELEASE)
            power_pressed = false;

        if (usb_irq_status & C10_AXP202_VBUS_IRQS)
            c10_usb_update(true);

        __gpio_unmask_irq(PIN_AXP202_IRQ);
    }
}

void GPIO102(void)
{
    /* AXP202 register access sleeps/spins on I2C interrupts, so only mask and
     * wake the deferred worker here. The generic dispatcher already ACKed PD6. */
    __gpio_mask_irq(PIN_AXP202_IRQ);
    semaphore_release(&power_wakeup);
}
#endif

static void c10_update_battery_voltage(void)
{
    int high = c10_axp202_read(C10_AXP202_BAT_V_H);
    int low = c10_axp202_read(C10_AXP202_BAT_V_L);

    if (high >= 0 && low >= 0)
        battery_mv = (((high << 4) | (low & 0x0f)) * 11 + 5) / 10;
}

#ifndef BOOTLOADER
static int c10_volume_read_raw(void)
{
    int high = c10_axp202_read(C10_AXP202_GPIO0_ADC_H);
    int low = c10_axp202_read(C10_AXP202_GPIO0_ADC_H + 1);

    /* The legacy interrupt-driven JZ4760 I2C1 path is reliable for the
     * stock-style single-register accesses, but can stall on a two-byte
     * receive here. Keep the two ADC bytes as separate transactions. */
    if (high < 0 || low < 0)
        return -1;
    return (high << 4) | (low & 0x0f);
}

static int c10_volume_index(int raw)
{
    /* Stock axp202_gpio0_adc_value() returns ADC * 500 / 1000 before
     * proc_adc_volume() compares it with this table. Keep sampling and
     * hysteresis in native ADC counts, then apply that exact conversion. */
    raw /= 2;

    for (unsigned int i = 0; i < ARRAYLEN(volume_thresholds); ++i)
        if (raw >= volume_thresholds[i])
            return i;

    return ARRAYLEN(volume_thresholds) - 1;
}

static int c10_volume_read_stable(void)
{
    int first = c10_volume_read_raw();

    for (int attempt = 0; attempt < 3 && first >= 0; ++attempt)
    {
        int second = c10_volume_read_raw();

        if (second < 0)
            return -1;

        int delta = second - first;
        if (delta < 0)
            delta = -delta;
        if (delta < 10)
            return (first + second) / 2;

        first = second;
    }

    return -1;
}

static void c10_volume_setup(void);

static void c10_volume_thread(void)
{
    int stable_raw = -1;
    int applied_index = -1;

    /* adc_init() precedes settings and audio initialization. Wait until both
     * are complete before making the absolute hardware control authoritative. */
    while (!audio_is_initialized)
        sleep(HZ / 2);

    c10_volume_setup();

    while (true)
    {
        int raw = c10_volume_read_stable();

        if (raw < 0)
        {
            sleep(C10_VOLUME_POLL_TICKS);
            continue;
        }

        /* Validate each position with back-to-back samples. This preserves
         * stock's 10-count rejection without comparing separate 50 Hz polls,
         * so deliberate slider movement is not mistaken for ADC noise. */
        if (stable_raw < 0)
            stable_raw = raw;
        else
        {
            int delta = raw - stable_raw;
            if (delta < 0)
                delta = -delta;
            if (delta >= 6)
                stable_raw = raw;
        }

        int index = c10_volume_index(stable_raw);

        if (index != applied_index)
        {
            if (index == 0)
                sound_set_volume(sound_min(SOUND_VOLUME));
            else
            {
                /* Rockbox settings are whole dB; round the stock CS4398
                 * half-dB attenuation to the nearest representable value. */
                sound_set_volume(-((volume_attenuation[index] + 1) / 2));
            }
            applied_index = index;
        }

        sleep(C10_VOLUME_POLL_TICKS);
    }
}

static void c10_volume_setup(void)
{
    int value;

    /* Exact stock volume-sense setup: LDO2 at 2.0 V, GPIO0 in ADC mode,
     * GPIO0 ADC enabled, and the observed 100 Hz ADC/rate configuration. */
    value = c10_axp202_read(C10_AXP202_LDO24);
    if (value >= 0)
        c10_axp202_write(C10_AXP202_LDO24, (value & 0x0f) | 0x20);
    value = c10_axp202_read(C10_AXP202_OUTPUT);
    if (value >= 0)
        c10_axp202_write(C10_AXP202_OUTPUT, value | (1u << 2));
    c10_axp202_write(C10_AXP202_GPIO0_CTRL, 0x04);
    c10_axp202_write(C10_AXP202_ADC_ENABLE2, 0x88);
    c10_axp202_write(C10_AXP202_ADC_RATE, 0x72);

}

static void c10_volume_init(void)
{
    create_thread(c10_volume_thread, volume_stack, sizeof(volume_stack), 0,
                  "volume" IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU));
}
#endif

int _battery_voltage(void)
{
    if (TIME_AFTER(current_tick, battery_next_tick))
    {
        c10_update_battery_voltage();
        battery_next_tick = current_tick + HZ * 30;
    }

    return battery_mv;
}

void adc_init(void)
{
    c10_i2c1_init();
    c10_update_battery_voltage();
    battery_next_tick = current_tick + HZ * 30;
#ifndef BOOTLOADER
    c10_volume_init();
#endif
}
