/** @file backlight.c @brief Implementation; see backlight.h */
#include "backlight.h"
#include "board.h"

#include <stdio.h>

#define BACKLIGHT_MIN_PERCENT 10   /* never fully dark (the screen must stay usable) */

static long read_long(const char *path, long fallback)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return fallback;
    }

    long value = fallback;
    if (fscanf(file, "%ld", &value) != 1) {
        value = fallback;
    }

    fclose(file);
    return value;
}

void backlight_set_percent(int percent)
{
    if (percent < BACKLIGHT_MIN_PERCENT) {
        percent = BACKLIGHT_MIN_PERCENT;
    }

    if (percent > 100) {
        percent = 100;
    }

    /* The backlight class scales 0..max_brightness; map the percentage onto that, keeping at least 1
     * so the panel is never fully off.
     */
    long max = read_long(board_current()->backlight_max_path, 9);
    long level = max * percent / 100;
    if (level < 1) {
        level = 1;
    }

    FILE *file = fopen(board_current()->backlight_brightness_path, "w");
    if (file == NULL) {
        return;
    }

    fprintf(file, "%ld\n", level);
    fclose(file);
}
