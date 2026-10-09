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
#include "cpu.h"
#include "backlight-target.h"

/* Confirmed from the stock firmware: backlight is TCU/PWM1 on GPIO PE1. The
 * vendor routine uses a period of 0x190 and a maximum half-match of 0x18c.
 * This PWM is active-low: a smaller half-match produces more light.
 */
#define BACKLIGHT_PWM 1
#define BACKLIGHT_PERIOD 0x190
#define BACKLIGHT_DIMMEST 0x18c
#define BACKLIGHT_PIN (32*4+1) /* PE1 */

static int current_brightness = DEFAULT_BRIGHTNESS_SETTING;

static void backlight_pwm_set(int brightness)
{
    if (brightness < MIN_BRIGHTNESS_SETTING)
        brightness = MIN_BRIGHTNESS_SETTING;
    else if (brightness > MAX_BRIGHTNESS_SETTING)
        brightness = MAX_BRIGHTNESS_SETTING;

    current_brightness = brightness;

    __gpio_as_pwm1();
    __tcu_disable_pwm_output(BACKLIGHT_PWM);
    __tcu_stop_counter(BACKLIGHT_PWM);
    __tcu_set_count(BACKLIGHT_PWM, 0);
    __tcu_set_half_data(BACKLIGHT_PWM,
        (BACKLIGHT_DIMMEST * (MAX_BRIGHTNESS_SETTING - brightness)) /
        (MAX_BRIGHTNESS_SETTING - MIN_BRIGHTNESS_SETTING));
    __tcu_set_full_data(BACKLIGHT_PWM, BACKLIGHT_PERIOD);
    __tcu_start_counter(BACKLIGHT_PWM);
    __tcu_enable_pwm_output(BACKLIGHT_PWM);
}

bool backlight_hw_init(void)
{
    __gpio_as_pwm1();
    __tcu_start_timer_clock(BACKLIGHT_PWM);

    __tcu_stop_counter(BACKLIGHT_PWM);
    __tcu_init_pwm_output_low(BACKLIGHT_PWM);
    __tcu_set_pwm_output_shutdown_graceful(BACKLIGHT_PWM);
    __tcu_disable_pwm_output(BACKLIGHT_PWM);

    __tcu_select_extalclk(BACKLIGHT_PWM);
    __tcu_select_clk_div64(BACKLIGHT_PWM);

    __tcu_mask_half_match_irq(BACKLIGHT_PWM);
    __tcu_mask_full_match_irq(BACKLIGHT_PWM);

    __tcu_set_count(BACKLIGHT_PWM, 0);
    __tcu_set_half_data(BACKLIGHT_PWM, 0);
    __tcu_set_full_data(BACKLIGHT_PWM, BACKLIGHT_PERIOD);
    backlight_pwm_set(current_brightness);

    return true;
}

void backlight_hw_on(void)
{
    backlight_pwm_set(current_brightness);
}

void backlight_hw_off(void)
{
    __tcu_disable_pwm_output(BACKLIGHT_PWM);
    __tcu_stop_counter(BACKLIGHT_PWM);
    __tcu_set_count(BACKLIGHT_PWM, 0);

    __gpio_clear_pin(BACKLIGHT_PIN);
    __gpio_as_output(BACKLIGHT_PIN);
}

#ifdef HAVE_BACKLIGHT_BRIGHTNESS
void backlight_hw_brightness(int brightness)
{
    backlight_pwm_set(brightness);
}
#endif
