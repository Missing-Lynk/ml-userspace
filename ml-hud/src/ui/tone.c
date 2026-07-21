/** @file tone.c @brief See tone.h. */
#include "tone.h"
#include "buzzer.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define TONE_BEEP_MS 70

/* A melody is a note list stepped by the tone thread; a note is a pitch held for a duration, freq
 * 0 = a silent rest. Pitches sit near the ~3.85 kHz piezo resonance so they stay loud (notes far
 * below it are much quieter, so a "low" note is only relatively lower). The success melody is the
 * rising power-on chime; the failure cue is a distinct high-then-low pair.
 */
struct tone_note {
    uint16_t freq_hz;   /* 0 = rest */
    uint16_t ms;
};

static const struct tone_note MELODY_SUCCESS[] = {
    { 3000, 120 }, { 0, 60 }, { 3500, 120 }, { 0, 60 }, { 3850, 300 },
};

static const struct tone_note MELODY_FAIL[] = {
    { 3850, 110 }, { 0, 70 }, { 3000, 260 },
};

enum tone_cmd {
    TONE_CMD_NONE,
    TONE_CMD_BEEP,
    TONE_CMD_SUCCESS,
    TONE_CMD_FAIL,
};

/* One-slot command mailbox consumed by the tone thread; a melody post overwrites anything pending,
 * a beep post never overwrites a pending melody (melodies outrank beeps end to end). g_lock guards
 * the mailbox and g_running; cross-thread buzzer access is serialized inside the buzzer HAL. The
 * condvar runs on CLOCK_MONOTONIC so note deadlines are immune to wall-clock steps.
 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond;
static pthread_t       g_thread;
static int             g_running;
static enum tone_cmd   g_cmd;

static void deadline_add_ms(struct timespec *ts, uint32_t ms)
{
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long) (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

/* Tone playback: sleep on the condvar until the next on/off transition or an incoming command. A
 * beep is the buzzer enabled for exactly TONE_BEEP_MS; a melody steps its note list on precise
 * deadlines and restores the default beep pitch when done. A beep posted during a melody is
 * dropped (the melody owns the buzzer); a melody cancels a running beep.
 */
static void *tone_thread(void *arg)
{
    /* NULL mel = no melody playing; mel_idx = next note to start. */
    const struct tone_note *mel = NULL;
    const struct tone_note *note;
    int mel_len = 0;
    int mel_idx = 0;
    int beeping = 0;
    struct timespec deadline;
    enum tone_cmd cmd;

    (void) arg;

    pthread_mutex_lock(&g_lock);
    while (g_running) {
        if (g_cmd != TONE_CMD_NONE) {
            cmd = g_cmd;
            g_cmd = TONE_CMD_NONE;

            if (cmd == TONE_CMD_BEEP) {
                if (mel == NULL) {
                    buzzer_enable(1);
                    beeping = 1;
                    clock_gettime(CLOCK_MONOTONIC, &deadline);
                    deadline_add_ms(&deadline, TONE_BEEP_MS);
                }
            } else {
                if (beeping) {
                    buzzer_enable(0);
                    beeping = 0;
                }

                if (cmd == TONE_CMD_SUCCESS) {
                    mel = MELODY_SUCCESS;
                    mel_len = (int) (sizeof MELODY_SUCCESS / sizeof MELODY_SUCCESS[0]);
                } else {
                    mel = MELODY_FAIL;
                    mel_len = (int) (sizeof MELODY_FAIL / sizeof MELODY_FAIL[0]);
                }

                mel_idx = 0;
                /* Arm an already-expired deadline so the step below starts note 0 immediately. */
                clock_gettime(CLOCK_MONOTONIC, &deadline);
            }

            continue;
        }

        if (!beeping && mel == NULL) {
            pthread_cond_wait(&g_cond, &g_lock);
            continue;
        }

        if (pthread_cond_timedwait(&g_cond, &g_lock, &deadline) != ETIMEDOUT) {
            /* A command arrived (or spurious wake): re-evaluate. */
            continue;
        }

        if (beeping) {
            buzzer_enable(0);
            beeping = 0;
            continue;
        }

        /* Melody step: end the previous note, then start the next (or finish and restore the
         * default beep pitch so later key tones sound right).
         */
        buzzer_enable(0);
        if (mel_idx >= mel_len) {
            buzzer_reset_pitch();
            mel = NULL;
            continue;
        }

        note = &mel[mel_idx++];
        if (note->freq_hz > 0) {
            buzzer_pitch(note->freq_hz);
            buzzer_enable(1);
        }

        deadline_add_ms(&deadline, note->ms);
    }

    buzzer_enable(0);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void post(enum tone_cmd cmd)
{
    if (buzzer_volume() <= 0) {
        return;
    }

    pthread_mutex_lock(&g_lock);
    if (g_running) {
        /* A beep never displaces a pending melody command; melodies overwrite anything. */
        if (cmd != TONE_CMD_BEEP || g_cmd == TONE_CMD_NONE || g_cmd == TONE_CMD_BEEP) {
            g_cmd = cmd;
            pthread_cond_signal(&g_cond);
        }
    }

    pthread_mutex_unlock(&g_lock);
}

void tone_init(void)
{
    pthread_condattr_t attr;

    if (g_running) {
        return;
    }

    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cond, &attr);
    pthread_condattr_destroy(&attr);

    g_running = 1;
    if (pthread_create(&g_thread, NULL, tone_thread, NULL) != 0) {
        g_running = 0;
        fprintf(stderr, "hud: tone thread create failed, tones disabled\n");
    }
}

void tone_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_running) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    g_running = 0;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_thread, NULL);
}

void tone_beep(void)
{
    post(TONE_CMD_BEEP);
}

void tone_success(void)
{
    post(TONE_CMD_SUCCESS);
}

void tone_fail(void)
{
    post(TONE_CMD_FAIL);
}
