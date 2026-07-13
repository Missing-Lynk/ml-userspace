/**
 * @file board_betafpv_p1_hd.c
 * @brief Board profile for the BetaFPV P1 HD goggle (VR04). See board.h.
 */
#include "board.h"

static const board_profile_t betafpv_p1_hd = {
    .name         = "BetaFPV P1 HD (VR04)",
    .device_class = BOARD_CLASS_GOGGLE,

    .input_device = "/dev/input/event0",
    .sdcard_mount = "/mnt/sdcard",

    /* pack_volts = in_voltage1_input (calibrated pin mV) * divider / 1000; divider is the board
     * resistor ratio.
     */
    .battery_adc_input_path = "/sys/bus/iio/devices/iio:device0/in_voltage1_input",
    .battery_divider        = 20.7f,
    .battery_cell_min       = 2,
    .battery_cell_max       = 6,

    /* in_temp_scale holds whole degC directly (no arithmetic); there is no in_temp_input. */
    .temp_path = "/sys/bus/iio/devices/iio:device1/in_temp_scale",

    .backlight_brightness_path = "/sys/class/backlight/backlight/brightness",
    .backlight_max_path        = "/sys/class/backlight/backlight/max_brightness",

    /* Buzzer = PWM controller 1, channel 0 (DTS: pwm@1002000). The open 6.18 kernel numbers it
     * pwmchip1 (the stock 4.9 kernel called it pwmchip8). Period 260000 ns, duty = 13000 * volume.
     */
    .buzzer_export_path = "/sys/class/pwm/pwmchip1/export",
    .buzzer_enable_path = "/sys/class/pwm/pwmchip1/pwm0/enable",
    .buzzer_duty_path   = "/sys/class/pwm/pwmchip1/pwm0/duty_cycle",
    .buzzer_period_path = "/sys/class/pwm/pwmchip1/pwm0/period",

    .keys = {
      .up = 87,
      .down = 83,
      .center = 69,
      .back = 66,
      .left = 65,
      .right = 68,
      .record = 77,
      .bind = 73
    },

    .has_backlight = 1,
    .has_buzzer    = 1,
    .has_temp      = 1,
};

const board_profile_t *board_current(void)
{
    return &betafpv_p1_hd;
}
