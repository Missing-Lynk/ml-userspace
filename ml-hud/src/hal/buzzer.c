/** @file buzzer.c @brief Implementation; see buzzer.h */
#include "buzzer.h"
#include "board.h"

#include <stdio.h>

/* Tone shape from the vendor DTS (pwm@1002000, ch0): a ~3.8 kHz carrier whose duty is the volume. */
#define BUZZER_PERIOD_NS  260000L
#define BUZZER_DUTY_STEP  13000L    /* duty = 13000 * volume ns (volume 1..10) */
#define BUZZER_MAX_VOLUME 10

static int g_volume;    /* 0 = silent */
static int g_exported;  /* the PWM channel has been exported + its period set */

static void write_long(const char *path, long value)
{
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return;
    }

    fprintf(file, "%ld\n", value);
    fclose(file);
}

/* Export the buzzer's PWM channel and set its period once. Exporting an already-exported channel
 * fails harmlessly; the period is only writable after the channel exists.
 */
static void ensure_exported(void)
{
    if (g_exported) {
        return;
    }

    write_long(board_current()->buzzer_export_path, 0);
    write_long(board_current()->buzzer_period_path, BUZZER_PERIOD_NS);
    g_exported = 1;
}

void buzzer_set_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    }

    if (volume > BUZZER_MAX_VOLUME) {
        volume = BUZZER_MAX_VOLUME;
    }

    g_volume = volume;
    ensure_exported();
    write_long(board_current()->buzzer_duty_path, (long) volume * BUZZER_DUTY_STEP);
}

int buzzer_volume(void)
{
    return g_volume;
}

void buzzer_enable(int on)
{
    if (on && g_volume <= 0) {
        return;
    }

    write_long(board_current()->buzzer_enable_path, on ? 1 : 0);
}
