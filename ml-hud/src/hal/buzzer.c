/** @file buzzer.c @brief Implementation; see buzzer.h */
#include "buzzer.h"
#include "board.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

/* Tone shape from the vendor DTS (pwm@1002000, ch0): a ~3.8 kHz carrier whose duty is the volume. */
#define BUZZER_PERIOD_NS  260000L
#define BUZZER_DUTY_STEP  13000L    /* duty = 13000 * volume ns (volume 1..10) */
#define BUZZER_MAX_VOLUME 10

/* Callers span two threads (the tone thread and the main/LVGL thread via the volume setting), so
 * g_lock serializes the volume state and every multi-write sysfs sequence. g_panic is set by
 * buzzer_panic_off (signal context, so no lock) and permanently refuses further enables, keeping
 * the crash path the last writer of the enable file.
 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_panic;

/* g_volume 0 = silent; g_exported = the PWM channel has been exported + its period set. */
static int g_volume;
static int g_exported;

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

    pthread_mutex_lock(&g_lock);
    g_volume = volume;
    ensure_exported();
    write_long(board_current()->buzzer_duty_path, (long) volume * BUZZER_DUTY_STEP);
    pthread_mutex_unlock(&g_lock);
}

int buzzer_volume(void)
{
    int volume;

    pthread_mutex_lock(&g_lock);
    volume = g_volume;
    pthread_mutex_unlock(&g_lock);
    return volume;
}

void buzzer_enable(int on)
{
    pthread_mutex_lock(&g_lock);
    if (on && (g_volume <= 0 || g_panic)) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    write_long(board_current()->buzzer_enable_path, on ? 1 : 0);
    pthread_mutex_unlock(&g_lock);
}

void buzzer_pitch(int freq_hz)
{
    long period;

    if (freq_hz <= 0) {
        return;
    }

    pthread_mutex_lock(&g_lock);
    ensure_exported();
    period = 1000000000L / freq_hz;

    /* Write duty 0 before the period so the core never sees duty > period across the change; then a
     * volume-proportional duty, period/2 (50%, loudest) at max volume.
     */
    write_long(board_current()->buzzer_duty_path, 0);
    write_long(board_current()->buzzer_period_path, period);
    write_long(board_current()->buzzer_duty_path, period * g_volume / (2 * BUZZER_MAX_VOLUME));
    pthread_mutex_unlock(&g_lock);
}

void buzzer_reset_pitch(void)
{
    pthread_mutex_lock(&g_lock);
    ensure_exported();
    write_long(board_current()->buzzer_duty_path, 0);
    write_long(board_current()->buzzer_period_path, BUZZER_PERIOD_NS);
    write_long(board_current()->buzzer_duty_path, (long) g_volume * BUZZER_DUTY_STEP);
    pthread_mutex_unlock(&g_lock);
}

void buzzer_panic_off(void)
{
    int fd;

    /* Refuse all future enables first, so a concurrently running tone thread cannot re-latch the
     * PWM after the off write below. No locking: this runs in fatal-signal context.
     */
    g_panic = 1;

    fd = open(board_current()->buzzer_enable_path, O_WRONLY);
    if (fd < 0) {
        return;
    }

    ssize_t written = write(fd, "0\n", 2);
    (void) written;
    close(fd);
}
