/**
 * @file tempwarn.h
 * @brief The air-unit overheat banner: a red blinking "OVERHEATING" label centered at the top of
 *        the overlay. The policy (threshold, hysteresis, link gating) lives in hud.c; this module
 *        only shows or hides the label.
 *
 * The banner sits inside the BTFL OSD's region of the shared overlay plane, so the two clobber each
 * other's pixels: a full BTFL present erases the banner, and every banner repaint erases the BTFL
 * glyphs beneath it. The blink timer repaints the label every half-period while active, so a
 * clobbered banner is restored within one blink toggle; on deactivation the caller invalidates the
 * BTFL OSD to restore the glyphs (hud.c does this).
 */
#ifndef HUD_TEMPWARN_H
#define HUD_TEMPWARN_H

#include "lvgl.h"

/** @brief Build the (hidden) banner on @p parent. Call once, after the LVGL display is up. */
void tempwarn_create(lv_obj_t *parent);

/** @brief Show (blinking) or hide the banner. The text is re-resolved from the i18n catalog on
 *         each activation, so a language switch is picked up.
 */
void tempwarn_set_active(int active);

#endif /* HUD_TEMPWARN_H */
