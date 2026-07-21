/** @file tone.c @brief See tone.h. */
#include "tone.h"
#include "buzzer.h"

#include <stddef.h>

#define TONE_BEEP_MS 70

/* A melody is a note list stepped by tone_tick; a note is a pitch held for a duration, freq 0 = a
 * silent rest. Pitches sit near the ~3.85 kHz piezo resonance so they stay loud (notes far below it
 * are much quieter, so a "low" note is only relatively lower). The success melody is the rising
 * power-on chime; the failure cue is a distinct high-then-low pair. */
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

static int      g_beeping;
static uint32_t g_beep_off_ms;

static const struct tone_note *g_mel;   /* NULL = no melody playing */
static int      g_mel_len;
static int      g_mel_idx;              /* next note to start */
static uint32_t g_mel_next_ms;          /* when to advance to g_mel_idx */

void tone_beep(uint32_t now_ms)
{
    if (buzzer_volume() <= 0 || g_mel != NULL) {
        return;
    }

    buzzer_enable(1);
    g_beeping = 1;
    g_beep_off_ms = now_ms + TONE_BEEP_MS;
}

/* Start @p mel; cancels any plain beep. The first note fires on the next tone_tick. */
static void melody_start(const struct tone_note *mel, int len, uint32_t now_ms)
{
    if (buzzer_volume() <= 0) {
        return;
    }

    if (g_beeping) {
        buzzer_enable(0);
        g_beeping = 0;
    }

    g_mel = mel;
    g_mel_len = len;
    g_mel_idx = 0;
    g_mel_next_ms = now_ms;
}

void tone_success(uint32_t now_ms)
{
    melody_start(MELODY_SUCCESS, (int) (sizeof MELODY_SUCCESS / sizeof MELODY_SUCCESS[0]), now_ms);
}

void tone_fail(uint32_t now_ms)
{
    melody_start(MELODY_FAIL, (int) (sizeof MELODY_FAIL / sizeof MELODY_FAIL[0]), now_ms);
}

/* Step the melody: end the previous note, then start the next (or finish and restore the default
 * beep pitch so later key tones sound right). */
static void melody_tick(uint32_t now_ms)
{
    const struct tone_note *note;

    if ((int32_t) (now_ms - g_mel_next_ms) < 0) {
        return;
    }

    buzzer_enable(0);
    if (g_mel_idx >= g_mel_len) {
        buzzer_reset_pitch();
        g_mel = NULL;
        return;
    }

    note = &g_mel[g_mel_idx++];
    if (note->freq_hz > 0) {
        buzzer_pitch(note->freq_hz);
        buzzer_enable(1);
    }

    g_mel_next_ms = now_ms + note->ms;
}

void tone_tick(uint32_t now_ms)
{
    if (g_mel != NULL) {
        melody_tick(now_ms);
        return;
    }

    if (g_beeping && (int32_t) (now_ms - g_beep_off_ms) >= 0) {
        buzzer_enable(0);
        g_beeping = 0;
    }
}
