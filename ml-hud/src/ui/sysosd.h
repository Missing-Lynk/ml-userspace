/**
 * @file sysosd.h
 * @brief The System OSD: a persistent status bar across the bottom of the overlay. The left group
 *        shows the goggle's own telemetry (channel, battery, link/SNR, SD card, temperature); the right
 *        group shows the air unit (battery, distance, temperature, standby). It occupies the bottom
 *        strip the menu reserves (OSD_BAR_HEIGHT), so the menu and the BTFL OSD render above it.
 *
 * Bar visibility follows the "Show System OSD" setting; the temperature fields follow "Show
 * Temperature". Both live in the goggle settings section and are read live each update. The air-unit
 * battery/temperature come from the :10000 status frames (air_telem_t, assembled by the caller); the
 * RF link metrics (channel/SNR/distance) come from services/linkstate.
 */
#ifndef HUD_SYSOSD_H
#define HUD_SYSOSD_H

#include "hal/telemetry.h"
#include "settings/settings.h"

#include "lvgl.h"

/* Air-unit telemetry relayed down the :10000 status frames (via MLM_T_STATUS), assembled in hud.c
 * from the decoded 0x09/0x11 frames. Distinct from services/linkstate, which carries the local
 * baseband link metrics (channel/SNR/distance). */
typedef struct {
    int have_voltage;   /* air-unit battery reading valid */
    int voltage_mV;     /* air-unit pack millivolts (0x09/0x11 frame @96) */
    int have_temp;      /* air-unit temperature reading valid */
    int temp_c;         /* air-unit temperature, whole degrees Celsius (0x09 frame @98) */
} air_telem_t;

/* Height of the bottom status strip. The menu reserves this same strip above itself, and the BTFL OSD
 * is mapped into the region above it, so the three never fight over the shared overlay plane.
 */
#define OSD_BAR_HEIGHT 110

/** @brief Build the bar on @p parent (the active screen). Call once, after the LVGL display is up. */
void sysosd_create(lv_obj_t *parent);

/** @brief Refresh the fields from @p telemetry (goggle) and @p air (air unit), gating each on its
 *         setting in @p settings. The RF link metrics are pulled from services/linkstate internally.
 *         Call each loop tick; cheap when nothing changed (LVGL only repaints dirtied labels).
 */
void sysosd_update(const telemetry_t *telemetry, const air_telem_t *air, settings_t *settings);

/** @brief Tell the bar the menu opened/closed. Open gives it a solid background (context behind the
 *         menu); closed makes it transparent so the video shows through.
 */
void sysosd_set_menu_open(int open);

/** @brief Show or hide the red "REC" indicator (driven by ml-pipeline's reported state). */
void sysosd_set_recording(int recording);

/** @brief Show or hide the orange "BIND" indicator (driven by ml-linkd's bind state). Shares the
 *  REC slot and takes precedence over it while a bind is in progress. */
void sysosd_set_binding(int binding);

/** @brief Show or hide the whole System OSD bar (playback hides it, leaving only the transport bar). */
void sysosd_set_visible(int visible);

/** @brief Force a full repaint of the bar. Call after another layer (a full BTFL redraw on menu close)
 *         has overwritten the shared overlay under the strip.
 */
void sysosd_invalidate(void);

#endif /* HUD_SYSOSD_H */
