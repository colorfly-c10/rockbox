/***************************************************************************
 * Colorfly C10 CS4398 and board audio support
 *
 * The GPIO and register sequence follows the C10 stock kernel. The board
 * uses a CS4398 on I2C0, an external sample-rate selector on PB0..PB3, and
 * several ordered analog power controls.
 *
 * Copyright (C) 2026 by Anton Bambura
 ****************************************************************************/

#include "config.h"
#include "audio.h"
#include "sound.h"
#include "cpu.h"
#include "system.h"
#include "kernel.h"
#include "cs4398.h"
#include "i2c-target.h"
#include "codec-colorfly-c10.h"

#define PIN_CPLD_LATCH  (32*1+0)
#define PIN_CPLD_RATE0  (32*1+1)
#define PIN_CPLD_RATE1  (32*1+2)
#define PIN_CPLD_RATE2  (32*1+3)
#define PIN_CPLD_CONTROL (32*1+5)
#define PIN_DAC_RESET   (32*1+10)
#define PIN_POWER_3     (32*1+11)
#define PIN_AUDIO_AUX0  (32*3+4)
#define PIN_AUDIO_AUX1  (32*3+5)
#define PIN_POWER_2     (32*3+7)
#define PIN_POWER_AUX   (32*4+14)
#define PIN_POWER_1A    (32*4+15)
#define PIN_RELAY       (32*4+16)
#define PIN_POWER_1B    (32*4+17)
#define PIN_PHONE_CHECK (32*4+18)
#define PIN_LINE_CHECK  (32*4+19)
#define PIN_POWER_MAIN  (32*0+24)

#define C10_MUTE_SETTLE_MS 50
#define C10_DAC_SETTLE_MS  20
#define C10_RELAY_SETTLE_TICKS (HZ/2)

/* Final stock external-I2S state, captured on the C10 while idle. Enabling
 * the controller and setting the 27-word TX threshold changes this exactly
 * to the observed playback value 0x081b0051. The earlier 0x8870 write in
 * init_jz_i2s is only a temporary pre-configuration value. */
#define C10_AIC_FR_STOCK_BASE 0x08140050u

/* The C10 straps the CS4398 address to 0x4c on I2C0. */
void cs4398_write_reg(uint8_t reg, uint8_t value)
{
    unsigned char buf[2] = { reg, value };
    c10_i2c0_write(0x4c, buf, sizeof(buf));
}

static volatile bool playback_active;
static volatile bool output_has_progress;
static struct timeout relay_connect_timeout;
static bool dac_operating;
static unsigned char reg_mode;
static unsigned char reg_ramp;
static unsigned char reg_misc;

void audiohw_set_frequency(int fsel);
static void cpld_set_rate(unsigned int index);

static void gpio_output_low(int pin)
{
    __gpio_clear_pin(pin);
    __gpio_as_output(pin);
    __gpio_disable_pull(pin);
}

static void gpio_output_high(int pin)
{
    /* Direct USB START2 skips the stock U-Boot GPIO setup. Set the output
     * latch first, then claim the pin, so PE16 cannot produce a false relay
     * edge while Rockbox initializes storage and the audio rails. */
    __gpio_set_pin(pin);
    __gpio_as_output(pin);
    __gpio_disable_pull(pin);
}

static void relay_route(bool connect)
{
    __gpio_as_output(PIN_RELAY);
    __gpio_disable_pull(PIN_RELAY);

    /* Stock drives PE16 low only for Line_check=1 and Phone_check=0. */
    if (connect && __gpio_get_pin(PIN_LINE_CHECK) &&
        !__gpio_get_pin(PIN_PHONE_CHECK))
        __gpio_clear_pin(PIN_RELAY);
    else
        __gpio_set_pin(PIN_RELAY);
}

static int relay_connect_callback(struct timeout *tmo)
{
    (void)tmo;

    /* Timeout callbacks run from the tick task. GPIO is safe here; codec
     * I2C and waits must remain in normal thread context. */
    if (playback_active && output_has_progress)
        relay_route(true);

    return 0;
}

static void relay_disconnect(void)
{
    timeout_cancel(&relay_connect_timeout);
    relay_route(false);
}

static void relay_connect_later(void)
{
    /* Stock cs4398_wakeup() arms its PE16 connect timer for 0x32 ticks.
     * Its kernel runs at 100 Hz, making this a 500 ms analog-settle delay. */
    timeout_register(&relay_connect_timeout, relay_connect_callback,
                     C10_RELAY_SETTLE_TICKS, 0);
}

void c10_audio_jack_changed(void)
{
    if (!__gpio_get_pin(PIN_LINE_CHECK) ||
        __gpio_get_pin(PIN_PHONE_CHECK))
        relay_disconnect();
    else if (playback_active && output_has_progress)
        relay_connect_later();
}

static void audio_power_on(void)
{
    __gpio_set_pin(PIN_POWER_MAIN);
    /* ctrl_audio_power(1) does not mention PE14, but the raw stock runtime
     * captures consistently show it high both idle and during playback.
     * A separate board/analog path therefore enables it after the low state
     * established by init_cs4398_i2c(). Match the observed completed state
     * before powering the remaining stages or connecting PE16. */
    __gpio_set_pin(PIN_POWER_AUX);
    __gpio_set_pin(PIN_POWER_1A);
    __gpio_set_pin(PIN_POWER_1B);
    mdelay(20);
    __gpio_set_pin(PIN_POWER_2);
    mdelay(15);
    __gpio_set_pin(PIN_POWER_3);
    mdelay(15);
}

static void audio_power_off(void)
{
    relay_route(false);
    __gpio_clear_pin(PIN_POWER_AUX);
    __gpio_clear_pin(PIN_POWER_1A);
    __gpio_clear_pin(PIN_POWER_1B);
    mdelay(20);
    __gpio_clear_pin(PIN_POWER_2);
    mdelay(15);
    __gpio_clear_pin(PIN_POWER_3);
    mdelay(15);
    __gpio_clear_pin(PIN_POWER_MAIN);
}

static void board_gpio_init(void)
{
    gpio_output_low(PIN_CPLD_LATCH);
    gpio_output_low(PIN_CPLD_RATE0);
    gpio_output_low(PIN_CPLD_RATE1);
    gpio_output_low(PIN_CPLD_RATE2);
    gpio_output_low(PIN_CPLD_CONTROL);
    gpio_output_low(PIN_DAC_RESET);
    gpio_output_low(PIN_POWER_3);
    /* The stock CS4398 initializer claims PD4 and PD5 as low GPIO outputs.
     * Their board-level names are unknown, but omitting them leaves two
     * audio-adjacent controls in the START2 state. */
    gpio_output_low(PIN_AUDIO_AUX0);
    gpio_output_low(PIN_AUDIO_AUX1);
    gpio_output_low(PIN_POWER_2);
    gpio_output_low(PIN_POWER_AUX);
    gpio_output_low(PIN_POWER_1A);
    gpio_output_low(PIN_POWER_1B);
    gpio_output_low(PIN_POWER_MAIN);
    /* Stock keeps the active-low headphone route disconnected until the DAC
     * is ready. START1 does not establish this state, so do it explicitly. */
    gpio_output_high(PIN_RELAY);

    __gpio_as_input(PIN_PHONE_CHECK);
    __gpio_disable_pull(PIN_PHONE_CHECK);
    __gpio_as_input(PIN_LINE_CHECK);
    __gpio_disable_pull(PIN_LINE_CHECK);
}

static void dac_reset(void)
{
    __gpio_clear_pin(PIN_DAC_RESET);
    mdelay(10);
    __gpio_set_pin(PIN_DAC_RESET);
    mdelay(1);
}

static void cpld_set_rate(unsigned int index)
{
    const unsigned int latch = 1u << 0;
    const unsigned int control = 1u << 5;

    if (index >= 8)
        return;

    /* The stock rate ioctl first makes PB0 and PB5 GPIO outputs-low. It then
     * calls set_cpld_fs_gpio(), which writes PB3..PB1 and finishes by making
     * PB0 output-high. PB5 remains output-low in every stock playback dump.
     * Preserve the complete transaction: leaving PB5 as an input, or leaving
     * PB0 high across a later rate change, is not the stock clock-selector
     * protocol. */
    REG_GPIO_PXFUNS(1) = latch;
    REG_GPIO_PXTRGC(1) = latch;
    REG_GPIO_PXSELC(1) = latch;
    REG_GPIO_PXFUNC(1) = latch;
    REG_GPIO_PXSELC(1) = latch;
    REG_GPIO_PXDIRS(1) = latch;
    REG_GPIO_PXPES(1) = latch;
    REG_GPIO_PXDATC(1) = latch;

    REG_GPIO_PXFUNS(1) = control;
    REG_GPIO_PXTRGC(1) = control;
    REG_GPIO_PXSELC(1) = control;
    REG_GPIO_PXFUNC(1) = control;
    REG_GPIO_PXSELC(1) = control;
    REG_GPIO_PXDIRS(1) = control;
    REG_GPIO_PXPES(1) = control;
    REG_GPIO_PXDATC(1) = control;

    if (index & 4) REG_GPIO_PXDATS(1) = 1u << 3;
    else           REG_GPIO_PXDATC(1) = 1u << 3;
    if (index & 2) REG_GPIO_PXDATS(1) = 1u << 2;
    else           REG_GPIO_PXDATC(1) = 1u << 2;
    if (index & 1) REG_GPIO_PXDATS(1) = 1u << 1;
    else           REG_GPIO_PXDATC(1) = 1u << 1;

    REG_GPIO_PXFUNS(1) = latch;
    REG_GPIO_PXTRGC(1) = latch;
    REG_GPIO_PXSELC(1) = latch;
    REG_GPIO_PXFUNC(1) = latch;
    REG_GPIO_PXSELC(1) = latch;
    REG_GPIO_PXDIRS(1) = latch;
    REG_GPIO_PXPES(1) = latch;
    REG_GPIO_PXDATS(1) = latch;
}

static void audiohw_mute(bool mute)
{
    /* Exact stock cs4398_Mute(2) / cs4398_Mute(0) values. */
    cs4398_write_reg(CS4398_REG_MUTE, mute ? 0xfb : 0xe0);
}

void audiohw_preinit(void)
{
    reg_mode = CS4398_FM_SINGLE | CS4398_DIF_LJUST;
    /* 0xb0 is the CS4398-CZZ reset value and the stock kernel does not
     * overwrite register 7 during probe. */
    reg_ramp = CS4398_SOFT_RAMP | CS4398_RMP_UP | CS4398_RMP_DN;
    reg_misc = CS4398_CPEN | CS4398_PDN;

    cs4398_write_reg(CS4398_REG_MISC, reg_misc);
    cs4398_write_reg(CS4398_REG_MODECTL, reg_mode);
    cs4398_write_reg(CS4398_REG_VOLMIX, CS4398_VOL_B_EQ_A |
                     CS4398_ATAPI_A_L | CS4398_ATAPI_B_R);
    audiohw_mute(true);
    cs4398_write_reg(CS4398_REG_VOL_A, 0xff);
    cs4398_write_reg(CS4398_REG_VOL_B, 0x00);
    cs4398_write_reg(CS4398_REG_RAMPFILT, reg_ramp);
    /* Stock leaves STATIC_DSD set even for PCM playback. It does not affect
     * PCM data, but reproducing the complete captured register image avoids
     * inheriting an unknown reset state after direct USB boot. */
    cs4398_write_reg(CS4398_REG_MISC2, CS4398_STATIC_DSD);
}

static void audio_aic_init(void)
{
    /* Exact external-AIC pinmux from the stock codec ioctl. The binary never
     * touches PE5, so leave its board/startup state alone. */
    __gpio_as_func1(3*32+12);
    __gpio_as_func0(3*32+13);
    __gpio_as_func0(4*32+7);
    __cpm_start_aic();

    /* This is a plain JZ4760 (CLKGR0 bit 31 is zero), even though Rockbox's
     * closest CPU support uses the JZ4760B header. Stock selects the 12 MHz
     * external I2S clock and leaves the AIC divider at three. */
    REG_CPM_I2SCDR = 0;

    /* Reproduce the complete stock idle state instead of inheriting SPDIF
     * or AC-link state from START1. */
    REG_SPDIF_ENA = 0;
    REG_SPDIF_CTRL = SPDIF_CTRL_MTRIG | SPDIF_CTRL_MFFUR;

    /* Use the completed external-codec state, not init_jz_i2s's temporary
     * 0x8870 value. ESCLK is required even though BCLK/LRCLK are external. */
    __i2s_disable();
    REG_AIC_FR = C10_AIC_FR_STOCK_BASE & ~AIC_FR_ICDC;
    REG_AIC_CR = 0;
    REG_AIC_ACCR1 = 0;
    REG_AIC_ACCR2 = 0;
    REG_AIC_I2SCR = AIC_I2SCR_ESCLK | AIC_I2SCR_AMSL;
    REG_AIC_I2SDIV = 3;

    /* The stock external-I2S setup clears BCKD and SYNCD. The CPLD supplies
     * BCLK/LRCLK; retuning PLL1 here corrupts the stream and can stall other
     * work while Rockbox opens a track. */
    __i2s_as_slave();
    __i2s_send_lfirst();
    __i2s_unswitch_lr();
    __i2s_start_bitclk();
    __i2s_enable_unpack16();
    __aic_disable_mono2stereo();
    __aic_disable_byteswap();
    __aic_disable_unsignadj();
    __i2s_out_channel_select(1); /* two output channels */
    /* Stock's closed/idle endpoint is 24-bit in both directions. Opening an
     * S16 stream changes only OSS immediately before playback. */
    __i2s_set_iss_sample_size(24);
    __i2s_set_oss_sample_size(24);
    __i2s_set_transmit_trigger(20);
    __i2s_set_receive_trigger(8);

    /* Stock leaves AIC, replay, and TX DMA disabled after setup. The PCM
     * start hook enables all three only after the DAC and DMA are ready. */
}

void audiohw_init(void)
{
    board_gpio_init();
    relay_route(false);

    /* Match init_cs4398_i2c: keep PB0..PB3 low through reset. Stock only
     * enables the selected CPLD clock in the later set-speed operation. */
    dac_reset();
    c10_i2c0_init();

    audio_aic_init();

    audiohw_preinit();
    /* pcm_apply_settings() selects every rate, including the initial
     * default, immediately before the first PCM buffer. */
}

static int volume_to_hw(int tenth_db)
{
    /* CS4398 volume is hardware attenuation in 0.5 dB steps: 0x00 is
     * full-scale 0 dB and 0xff is mute. Stock passes this byte directly and
     * its volume table reaches 0x00, so do not impose an additional software
     * safety ceiling here. */
    if (tenth_db <= CS4398_VOLUME_MIN)
        return 0xff;
    if (tenth_db > CS4398_VOLUME_MAX)
        return 0x00;

    return -tenth_db / 5;
}

void audiohw_set_volume(int vol_l, int vol_r)
{
    int left = volume_to_hw(vol_l);
    (void)vol_r;

    /* Stock sets B_EQ_A, so volume register A controls both channels and B
     * remains zero in every active capture. */
    cs4398_write_reg(CS4398_REG_VOL_A, left);
}

void audiohw_set_lineout_volume(int vol_l, int vol_r)
{
    audiohw_set_volume(vol_l, vol_r);
}

void audiohw_set_filter_roll_off(int value)
{
    if (value)
        reg_ramp |= CS4398_FILT_SEL;
    else
        reg_ramp &= ~CS4398_FILT_SEL;
    cs4398_write_reg(CS4398_REG_RAMPFILT, reg_ramp);
}

void audiohw_set_frequency(int fsel)
{
    unsigned char mode, rate;

    switch (fsel)
    {
    case HW_FREQ_32:
        mode = CS4398_FM_SINGLE; rate = 0; break;
    case HW_FREQ_44:
        mode = CS4398_FM_SINGLE; rate = 1; break;
    case HW_FREQ_48:
        mode = CS4398_FM_SINGLE; rate = 2; break;
    case HW_FREQ_64:
        mode = CS4398_FM_DOUBLE; rate = 3; break;
    case HW_FREQ_88:
        mode = CS4398_FM_DOUBLE; rate = 4; break;
    case HW_FREQ_96:
        mode = CS4398_FM_DOUBLE; rate = 5; break;
    case HW_FREQ_176:
        mode = CS4398_FM_QUAD; rate = 6; break;
    case HW_FREQ_192:
        mode = CS4398_FM_QUAD; rate = 7; break;
    default:
        return;
    }

    bool playing = playback_active;
    if (playing)
        relay_disconnect();

    audiohw_mute(true);
    if (playing)
    {
        /* CS4398 soft mute moves 1 dB per eight LRCK periods. A full-scale
         * 44.1 kHz stream needs about 23 ms to reach mute, so changing MCLK
         * after only 20 ms can expose the tail of the ramp as a pop. */
        mdelay(C10_MUTE_SETTLE_MS);
    }

    cpld_set_rate(rate);
    reg_mode = mode | CS4398_DIF_LJUST;
    cs4398_write_reg(CS4398_REG_MODECTL, reg_mode);
    cs4398_write_reg(CS4398_REG_VOLMIX, CS4398_VOL_B_EQ_A |
                     CS4398_ATAPI_A_L | CS4398_ATAPI_B_R);

    mdelay(C10_DAC_SETTLE_MS);

    if (playing)
    {
        audiohw_mute(false);
        relay_connect_later();
    }
}

void audiohw_postinit(void)
{
    /* Stock enables the ordered analog rails at open, but leaves the DAC in
     * CPEN|PDN and muted until the first PCM buffer is ready. */
    audio_power_on();
}

void c10_audio_start_playback(void)
{
    if (playback_active)
        return;

    relay_route(false);
    audiohw_mute(true);

    /* The stock S16 open path changes only these two AIC fields. Together
     * with TDMS/ERPL below, this produces the captured active values
     * FR=0x081b0051 and CR=0x010c4002. */
    __i2s_set_transmit_trigger(27);
    __i2s_set_oss_sample_size(16);

    /* Stock enables AIC at /dev/dsp open, before first-output wakes the DAC,
     * and leaves it enabled between DMA transfers. The CPLD rate has already
     * been selected by pcm_apply_settings() at this point. */
    __i2s_enable();

    if (!dac_operating)
    {
        reg_misc = CS4398_CPEN;
        cs4398_write_reg(CS4398_REG_MISC, reg_misc);
        dac_operating = true;
    }

    output_has_progress = false;
    playback_active = true;
}

void c10_audio_output_started(void)
{
    if (!playback_active)
        return;

    /* The CS4398 requires stable MCLK/LRCK and valid serial data before
     * normal conversion. Match the stock pop-suppression order: settle the
     * unmuted DAC while PE16 isolates the jack, then connect headphones. */
    mdelay(2);
    audiohw_mute(false);
    mdelay(C10_DAC_SETTLE_MS);
    relay_connect_later();
}

void c10_audio_dma_buffer_complete(void)
{
    output_has_progress = true;
}

void c10_audio_stop_playback(void)
{
    /* PCM stop may run from the DMA callback, so it cannot perform codec
     * I2C. PE16 and timeout operations are IRQ-safe. Stock disconnects PE16
     * immediately from its audio-device release path. */
    playback_active = false;
    output_has_progress = false;
    relay_disconnect();
}

void audiohw_close(void)
{
    playback_active = false;
    output_has_progress = false;
    relay_disconnect();
    audiohw_mute(true);
    reg_misc = CS4398_CPEN | CS4398_PDN;
    cs4398_write_reg(CS4398_REG_MISC, reg_misc);
    dac_operating = false;
    __i2s_disable_replay();
    __i2s_disable_transmit_dma();
    __i2s_disable();
    __cpm_stop_aic();
    /* Rockbox calls audiohw_close() from whole-device power management, not
     * for an ordinary track stop. Match stock system power-off here. */
    audio_power_off();
    __gpio_clear_pin(PIN_DAC_RESET);
}
