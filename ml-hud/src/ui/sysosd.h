/**
 * @file sysosd.h
 * @brief The System OSD: a persistent status bar across the bottom of the overlay, showing the
 *        goggle's own telemetry (battery, SD card, temperature). It occupies the bottom strip that the
 *        menu reserves (OSD_BAR_HEIGHT), so the menu and the BTFL OSD render above it and never overlap.
 *
 * This is the goggle (left) side only. Visibility follows the "Show System OSD" setting, and the
 * temperature field follows "Show Temperature"; both live in the goggle settings section and are read
 * live each update.
 */
#ifndef HUD_SYSOSD_H
#define HUD_SYSOSD_H

#include "hal/telemetry.h"
#include "settings/settings.h"

#include "lvgl.h"

/* Height of the bottom status strip. The menu reserves this same strip above itself, and the BTFL OSD
 * is mapped into the region above it, so the three never fight over the shared overlay plane.
 */
#define OSD_BAR_HEIGHT 110

/** @brief Build the bar on @p parent (the active screen). Call once, after the LVGL display is up. */
void sysosd_create(lv_obj_t *parent);

/** @brief Refresh the fields from @p telemetry, gating each on its setting in @p settings. Call each
 *         loop tick; cheap when nothing changed (LVGL only repaints dirtied labels).
 */
void sysosd_update(const telemetry_t *telemetry, settings_t *settings);

/** @brief Tell the bar the menu opened/closed. Open gives it a solid background (context behind the
 *         menu); closed makes it transparent so the video shows through.
 */
void sysosd_set_menu_open(int open);

/** @brief Show or hide the red "REC" indicator (driven by ml-pipeline's reported state). */
void sysosd_set_recording(int recording);

/** @brief Force a full repaint of the bar. Call after another layer (a full BTFL redraw on menu close)
 *         has overwritten the shared overlay under the strip.
 */
void sysosd_invalidate(void);

#endif /* HUD_SYSOSD_H */
