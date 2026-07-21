/**
 * @file tone.h
 * @brief Short beeps over the buzzer: key tones, setting confirmations, the low-voltage alarm.
 *
 * A dedicated timer thread owns the buzzer on/off writes, so a beep lasts exactly its nominal
 * duration regardless of main-loop (LVGL render) load. All calls are non-blocking posts to that
 * thread. Silent when the buzzer volume is 0.
 */
#ifndef HUD_UI_TONE_H
#define HUD_UI_TONE_H

/** @brief Start the tone thread. Call once before any tone request; without it all tones no-op. */
void tone_init(void);

/** @brief Stop and join the tone thread; silences the buzzer. */
void tone_shutdown(void);

/** @brief Start a short beep at the current buzzer volume. No-op while the volume is 0 or a melody
 * is playing (the melody owns the buzzer until it finishes). */
void tone_beep(void);

/** @brief Play the bind-success melody (the rising power-on chime). No-op while the volume is 0. */
void tone_success(void);

/** @brief Play the bind-failure cue (a distinct descending pair). No-op while the volume is 0. */
void tone_fail(void);

#endif /* HUD_UI_TONE_H */
