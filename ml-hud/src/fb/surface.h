/**
 * @file surface.h
 * @brief A plain RGBA8888 pixel surface: the framebuffer the HUD writes into.
 *
 * Every layer (BTFL OSD, System OSD, menu) composites into one of these; the pixels map to the DRM
 * overlay.
 */
#ifndef HUD_SURFACE_H
#define HUD_SURFACE_H

typedef struct {
    int w;
    int h;
    unsigned char *px;   /* w*h*4 bytes, RGBA row-major, R at [0] */
} surface_t;

/** @brief A pixel rectangle: the unit of a dirty-region update to a shared display. */
typedef struct {
    int x;
    int y;
    int w;
    int h;
} rect_t;

/** @brief Allocate an @p w x @p h RGBA surface (zeroed = transparent). Returns 0 on success. */
int surface_init(surface_t *s, int w, int h);

/** @brief Free the pixel buffer. */
void surface_free(surface_t *s);

#endif /* HUD_SURFACE_H */
