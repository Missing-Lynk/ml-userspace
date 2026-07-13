/**
 * @file drmoverlay.h
 * @brief Device display backend: present the HUD onto the DRM ARGB4444 overlay plane.
 *
 * This is the surface that actually composites over the live video.
 * Create a 1920x1080 ARGB4444 dumb buffer, and SETPLANE it on the overlay plane. The
 * CRTC must already be modeset by another client (ml-pipeline video, or ml-splash) -
 * SETPLANE retries until it is.
 */
#ifndef HUD_DRMOVERLAY_H
#define HUD_DRMOVERLAY_H

#include "surface.h"
#include <stdint.h>

typedef struct {
    int           drm;        /* shared DRM master fd from ml-drmfd */
    uint16_t     *px;         /* mmap'd ARGB4444 dumb buffer */
    unsigned long size;
    int           pitch_px;   /* buffer stride in pixels */
    int           w, h;
    uint32_t      crtc_id, plane_id, fb_id;
    uint32_t      handle;     /* dumb-buffer GEM handle, freed in close (the fd is shared, so exit will not) */
    int           plane_on;   /* SETPLANE succeeded (CRTC was active) */
} drm_overlay_t;

/**
 * @brief Get the DRM master fd, create the ARGB4444 overlay buffer. @p plane_id is the overlay plane
 *        (ml-hud uses 38). Returns 0 on success, -1 on failure (e.g. the ml-drmfd broker not running).
 */
int drm_overlay_open(drm_overlay_t *d, uint32_t plane_id);

/**
 * @brief Pack the given dirty rectangles of @p s (RGBA) into the ARGB4444 buffer and enable/refresh
 *        the overlay plane. @p nrects < 0 means the whole surface is dirty.
 */
void drm_overlay_present(drm_overlay_t *d, const surface_t *s, const rect_t *rects, int nrects);

/** @brief Enable the overlay plane (SETPLANE) if not already on. Idempotent. Callers that write the
 *         buffer directly (e.g. the LVGL display flush) call this to bind/scan out the plane.
 */
void drm_overlay_enable(drm_overlay_t *d);

/** @brief Zero the whole buffer (fully transparent). Used to hand the surface between owners (menu
 *         vs BTFL OSD) so stale pixels do not linger.
 */
void drm_overlay_clear(drm_overlay_t *d);

/** @brief Disable the plane and release the buffer/fd. */
void drm_overlay_close(drm_overlay_t *d);

#endif /* HUD_DRMOVERLAY_H */
