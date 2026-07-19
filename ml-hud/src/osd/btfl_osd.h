/**
 * @file btfl_osd.h
 * @brief Renders the Betaflight/INAV MSP DisplayPort OSD canvas into the HUD framebuffer, updating
 *        only the glyph cells that changed frame-to-frame.
 *
 * This is the HUD's port of libre's msp_osd, with the LVGL canvas replaced by direct framebuffer
 * writes and a grid diff on top: the OSD is a 53x20 glyph grid, so between frames usually only a few
 * cells change (e.g. the voltage digits). btfl_osd_update decodes the canvas, compares it to the last
 * grid, redraws only the changed cells into the target surface, and reports their screen rectangles
 * so the display backend rewrites only those pixels - never the whole 1080p plane, which is shared.
 *
 * The decode is done by the copied msp_canvas parser and the glyphs come from the copied msp_font
 * loader. The canvas arrives from the OSD channel (`:10000` type-0x10 frames).
 */
#ifndef HUD_BTFL_OSD_H
#define HUD_BTFL_OSD_H

#include "surface.h"

#include <stdbool.h>

/* Betaflight HD DisplayPort grid. Each cell is scaled from the font's 24x36 glyph to its slice of the
 * target surface; the scale uses a global (col*W/COLS) mapping so adjacent cells tile seamlessly. */
#define BTFL_OSD_COLS 53
#define BTFL_OSD_ROWS 20

/**
 * @brief Load the BTFL OSD glyph font. If @p font_path is NULL, the HUD_MSP_FONT env override is
 *        tried first, then a built-in search list (repo asset, then device ship paths).
 * @return 0 on success, -1 if no font could be loaded (updates then no-op).
 */
int btfl_osd_init(const char *font_path);

/** @brief Is a font loaded (so updates will draw)? */
bool btfl_osd_is_ready(void);

/**
 * @brief Set the target size and the colour a changed cell is cleared to before its glyph is drawn.
 *
 * On the device the backdrop is transparent (a=0) so the video/lower layers show through; on the host
 * it is an opaque preview colour. Also resets the grid, so the next update is a full redraw. Call once
 * after init and before the first update.
 */
void btfl_osd_configure(int screen_w, int screen_h,
                        unsigned char br, unsigned char bg, unsigned char bb, unsigned char ba);

/** @brief Reset the grid so the next update is a full redraw. Call after another layer (the menu)
 *         has overwritten the shared surface, so the OSD repaints all its cells. */
void btfl_osd_invalidate(void);

/**
 * @brief Decode @p canvas, diff against the last grid, and redraw only changed cells into @p dst.
 * @param rects     Filled with the changed cells' screen rectangles.
 * @param max_rects Capacity of @p rects.
 * @return >0 = that many dirty rectangles to present; 0 = nothing changed (skip present); -1 = more
 *         cells changed than @p max_rects (dst is fully redrawn - present the whole surface).
 */
int btfl_osd_update(surface_t *dst, const unsigned char *canvas, int len,
                    rect_t *rects, int max_rects);

/**
 * @brief Blank every currently-drawn cell in @p dst and empty the grid, without a new canvas. Used
 *        when the link drops so the last MSP OSD frame does not linger over the no-signal splash;
 *        btfl_osd_update alone cannot clear it because no further frames arrive. Only glyphed cells
 *        are touched, so a co-drawn layer (e.g. the System OSD bar) is left intact.
 * @param rects     Filled with the cleared cells' screen rectangles.
 * @param max_rects Capacity of @p rects.
 * @return >0 = that many dirty rectangles to present; 0 = nothing was drawn (skip present); -1 = more
 *         cells cleared than @p max_rects (present the whole surface).
 */
int btfl_osd_clear(surface_t *dst, rect_t *rects, int max_rects);

#endif /* HUD_BTFL_OSD_H */
