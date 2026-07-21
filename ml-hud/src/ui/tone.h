/**
 * @file tone.h
 * @brief Short beeps over the buzzer: key tones, setting confirmations, the low-voltage alarm.
 *
 * A beep is the buzzer enabled for a fixed duration then released. tone_tick ends it, so the caller
 * never blocks. Silent when the buzzer volume is 0.
 */
#ifndef HUD_UI_TONE_H
#define HUD_UI_TONE_H

#include <stdint.h>

/** @brief Start a short beep at the current buzzer volume. No-op while the volume is 0 or a melody
 * is playing (the melody owns the buzzer until it finishes). */
void tone_beep(uint32_t now_ms);

/** @brief Play the bind-success melody (the rising power-on chime). No-op while the volume is 0. */
void tone_success(uint32_t now_ms);

/** @brief Play the bind-failure cue (a distinct descending pair). No-op while the volume is 0. */
void tone_fail(uint32_t now_ms);

/** @brief Advance the current beep or melody. Call every loop iteration. */
void tone_tick(uint32_t now_ms);

#endif /* HUD_UI_TONE_H */
