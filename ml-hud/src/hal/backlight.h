/**
 * @file backlight.h
 * @brief Goggle panel brightness, set directly via the backlight PWM duty cycle (no SDK).
 */
#ifndef HUD_BACKLIGHT_H
#define HUD_BACKLIGHT_H

/**
 * @brief Set panel brightness as a percentage.
 * @param percent 0-100; clamped to a usable minimum so the screen never goes fully dark.
 */
void backlight_set_percent(int percent);

#endif /* HUD_BACKLIGHT_H */
