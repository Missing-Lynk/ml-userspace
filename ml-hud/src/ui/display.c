/** @file display.c @brief See display.h. */
#include "display.h"

#include <stdint.h>
#include <stdlib.h>

static drm_overlay_t *g_overlay;
static lv_display_t  *g_display;
static uint8_t       *g_render_buf;

/* LVGL renders ARGB8888 (memory order B, G, R, A); convert to the overlay's ARGB4444, preserving
 * alpha so transparent areas composite over the live video. Only the flushed area is written.
 */
static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixel_map)
{
    drm_overlay_t *overlay = g_overlay;
    const uint8_t *source = pixel_map;

    for (int y = area->y1; y <= area->y2; y++) {
        uint16_t *destination = overlay->px + (size_t) y * overlay->pitch_px + area->x1;
        for (int x = area->x1; x <= area->x2; x++) {
            uint8_t blue = source[0];
            uint8_t green = source[1];
            uint8_t red = source[2];
            uint8_t alpha = source[3];
            *destination = (uint16_t) (((alpha >> 4) << 12) | ((red >> 4) << 8)
                                     | ((green >> 4) << 4) | (blue >> 4));
            source += 4;
            destination++;
        }
    }

    drm_overlay_enable(overlay);
    lv_display_flush_ready(display);
}

int ui_display_init(drm_overlay_t *overlay)
{
    g_overlay = overlay;

    int width = overlay->w;
    int height = overlay->h;
    size_t buffer_bytes = (size_t) width * height * 4;

    /* Full-screen render buffer so the menu opens in one flush (no top-down wipe). */
    g_render_buf = malloc(buffer_bytes);
    if (g_render_buf == NULL) {
        return -1;
    }

    g_display = lv_display_create(width, height);
    lv_display_set_color_format(g_display, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_flush_cb(g_display, flush_cb);
    lv_display_set_buffers(g_display, g_render_buf, NULL, buffer_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Transparent screen: anything not drawn composites over the live video. */
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_TRANSP, 0);

    return 0;
}

int ui_display_width(void)
{
    return g_overlay != NULL ? g_overlay->w : 0;
}

int ui_display_height(void)
{
    return g_overlay != NULL ? g_overlay->h : 0;
}

void ui_display_deinit(void)
{
    free(g_render_buf);
    g_render_buf = NULL;
}
