/**
 * @file buzzer.h
 * @brief Goggle buzzer over its PWM: duty sets the volume, enable gates the tone.
 *
 * The volume is the PWM duty cycle and a "beep" is the enable line pulsed on then
 * off (see ui/tone). Volume 0 is silent: enable is then a no-op.
 */
#ifndef HUD_BUZZER_H
#define HUD_BUZZER_H

/** @brief Set the volume (0 = silent, 1..10 louder), as the PWM duty. Persists in memory only. */
void buzzer_set_volume(int volume);

/** @brief The current volume. */
int buzzer_volume(void);

/** @brief Gate the tone on/off. Turning on is a no-op while the volume is 0. */
void buzzer_enable(int on);

/**
 * @brief Set the tone pitch from @p freq_hz, with a volume-proportional duty (50% at max volume,
 * the loudest ratio, matching the boot chime). For melodies; gate it with buzzer_enable. A pitch
 * far below the ~3.8 kHz piezo resonance is much quieter. No-op at @p freq_hz <= 0.
 */
void buzzer_pitch(int freq_hz);

/** @brief Restore the default ~3.8 kHz beep period and volume duty after a melody. */
void buzzer_reset_pitch(void);

/**
 * @brief Force the tone off with a single raw sysfs write (open/write/close only).
 *
 * Async-signal-safe, unlike buzzer_enable (which uses stdio): safe to call from a fatal-signal
 * handler or atexit so a crashing/aborting process never leaves the PWM latched on.
 */
void buzzer_panic_off(void);

#endif /* HUD_BUZZER_H */
