/**
 * @file display.h
 * @brief The DRM overlay plane as an LVGL display.
 *
 * Renders ARGB8888 and converts each flushed area to the overlay's ARGB4444 buffer, preserving
 * alpha so the menu composites over the live video. The overlay buffer/plane is shared with the
 * BTFL OSD (they are mutually exclusive by menu state).
 */
#ifndef HUD_UI_DISPLAY_H
#define HUD_UI_DISPLAY_H

#include "lvgl.h"
#include "drmoverlay.h"

/** @brief Create the LVGL display over @p overlay (already opened). Returns 0 on success, -1 on
 *         allocation failure.
 */
int ui_display_init(drm_overlay_t *overlay);

int ui_display_width(void);   /**< @brief Display width in pixels. */
int ui_display_height(void);  /**< @brief Display height in pixels. */

/** @brief Free the render buffer. Call once on shutdown, after LVGL is torn down. */
void ui_display_deinit(void);

#endif /* HUD_UI_DISPLAY_H */
