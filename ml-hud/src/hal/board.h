/**
 * @file board.h
 * @brief Per-device hardware profile, the board layer of the HAL.
 *
 * The HAL modules (input, and later backlight/buzzer/telemetry) are device-agnostic; this struct
 * holds the values that differ per device: the device nodes, the battery divider, the PWM channels,
 * the keypad map, and which optional peripherals exist. board_current() returns the active profile.
 * One profile is linked in at build time; runtime detection can replace the selector later without
 * touching this interface or the modules.
 *
 * On the open kernel the display is a DRM overlay plane, not a framebuffer node, so there is no
 * framebuffer_device field here (see fb/drmoverlay.c).
 */
#ifndef HUD_BOARD_H
#define HUD_BOARD_H

/* Device taxonomy: an air unit (TX), or a ground unit which is either a goggle (built-in display)
 * or a VRX (drives an external display, e.g. HDMI). Goggle vs VRX differ by capabilities, not code. */
typedef enum {
    BOARD_CLASS_GOGGLE,   /* ground unit, built-in display(s) */
    BOARD_CLASS_VRX,      /* ground unit, external display (e.g. HDMI) */
    BOARD_CLASS_AIR,      /* air unit (TX): camera + video transmitter */
} board_class_t;

/* evdev keycodes the keypad emits (not WSAD; captured per device). */
typedef struct {
    int up;
    int down;
    int center;
    int back;
    int left;
    int right;
    int record;   /* global REC toggle (0 = the board has no record key) */
    int bind;      /* global RF-pair action (0 = the board has no bind key) */
} board_keymap_t;

typedef struct {
    const char    *name;                /* human-readable, e.g. "BetaFPV P1 HD (VR04)" */
    board_class_t  device_class;

    /* device nodes */
    const char *input_device;           /* /dev/input/eventN (the keypad) */
    const char *sdcard_mount;           /* where the DVR SD card mounts */

    /* battery (ADC) */
    /* The driver's calibrated PROCESSED node (in_voltageN_input, mV): per-channel ADC gain+offset
     * calibration, so only the external resistor divider is left to apply.
     */
    const char *battery_adc_input_path; /* iio processed node (in_voltageN_input, mV) */
    float       battery_divider;        /* external divider ratio: pack mV = pin mV * divider */
    int         battery_cell_min;       /* clamp + "pack present" floor for cell auto-detect */
    int         battery_cell_max;

    /* thermal */
    const char *temp_path;              /* iio temperature node, whole degrees Celsius */

    /* backlight (the open kernel exposes a standard backlight class device) */
    const char *backlight_brightness_path;
    const char *backlight_max_path;

    /* buzzer (PWM: export the channel, duty sets the volume, enable gates the tone) */
    const char *buzzer_export_path;
    const char *buzzer_enable_path;
    const char *buzzer_duty_path;
    const char *buzzer_period_path;

    /* keypad */
    board_keymap_t keys;

    /* capabilities, peripherals a given board may lack */
    unsigned has_backlight : 1;
    unsigned has_buzzer    : 1;
    unsigned has_temp      : 1;
} board_profile_t;

/**
 * @brief The active board profile (one is linked in at build time).
 * @return the active profile, never NULL.
 */
const board_profile_t *board_current(void);

#endif /* HUD_BOARD_H */
