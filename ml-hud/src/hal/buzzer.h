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

#endif /* HUD_BUZZER_H */
