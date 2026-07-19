/** @file btfl_osd.c @brief See btfl_osd.h. */
#include "btfl_osd.h"
#include "msp_font.h"
#include "msp_canvas.h"

#include <stdlib.h>
#include <string.h>

/* Font search order after the HUD_MSP_FONT env override: repo asset first, then the device ship
 * paths.
 */
static const char *FONT_PATHS[] = {
    "assets/osd-fonts/font_BTFL_hd.png",
    "../assets/osd-fonts/font_BTFL_hd.png",
    "../../assets/osd-fonts/font_BTFL_hd.png",
    "/usr/share/hud/fonts/font_BTFL_hd.png",
    "/usrdata/hud/fonts/font_BTFL_hd.png",
};

#define CELL_EMPTY  (-1)   /* no glyph in this cell */
#define CELL_UNSET  (-2)   /* grid reset: forces a full redraw on the next update */

static int  g_grid[BTFL_OSD_ROWS][BTFL_OSD_COLS];   /* last drawn glyph per cell */
static int  g_new[BTFL_OSD_ROWS][BTFL_OSD_COLS];    /* the decoding scratch for one update */
static int  g_screen_width, g_screen_height;                             /* target surface size */
static unsigned char g_bg[4];                       /* cleared-cell colour, RGBA */

int btfl_osd_init(const char *font_path)
{
    if (font_path != NULL) {
        return msp_font_load(font_path);
    }

    const char *env = getenv("HUD_MSP_FONT");
    if (env != NULL && msp_font_load(env) == 0) {
        return 0;
    }

    for (int i = 0; i < (int) (sizeof(FONT_PATHS) / sizeof(FONT_PATHS[0])); i++) {
        if (msp_font_load(FONT_PATHS[i]) == 0) {
            return 0;
        }
    }

    return -1;
}

bool btfl_osd_is_ready(void)
{
    return msp_font_is_loaded();
}

void btfl_osd_configure(int screen_w, int screen_h,
                        unsigned char br, unsigned char bg, unsigned char bb, unsigned char ba)
{
    g_screen_width = screen_w;
    g_screen_height = screen_h;
    g_bg[0] = br; g_bg[1] = bg; g_bg[2] = bb; g_bg[3] = ba;
    btfl_osd_invalidate();
}

void btfl_osd_invalidate(void)
{
    for (int r = 0; r < BTFL_OSD_ROWS; r++) {
        for (int c = 0; c < BTFL_OSD_COLS; c++) {
            g_grid[r][c] = CELL_UNSET;   /* force a full redraw on the next update */
        }
    }
}

/* Screen rectangle of grid cell (row,col). The col*W/COLS mapping aligns adjacent cells exactly, so
 * glyphs tile seamlessly across cell boundaries.
 */
rect_t btfl_osd_cell_rect(int row, int col)
{
    int x0 = col * g_screen_width / BTFL_OSD_COLS;
    int x1 = (col + 1) * g_screen_width / BTFL_OSD_COLS;
    int y0 = row * g_screen_height / BTFL_OSD_ROWS;
    int y1 = (row + 1) * g_screen_height / BTFL_OSD_ROWS;
    rect_t rect = { x0, y0, x1 - x0, y1 - y0 };

    return rect;
}

/* Clear a cell to the background, then blit its (scaled) glyph. Writes exactly the cell rectangle. */
static void draw_cell(surface_t *dst, int row, int col, int glyph)
{
    rect_t rect = btfl_osd_cell_rect(row, col);
    const unsigned char *glyph_px = glyph >= 0 ? msp_font_glyph(glyph) : NULL;

    for (int y = 0; y < rect.h; y++) {
        int py = rect.y + y;
        if (py < 0 || py >= dst->h) {
            continue;
        }

        int gy = glyph_px != NULL ? y * MSP_FONT_GLYPH_H / rect.h : 0;
        unsigned char *drow = dst->px + ((size_t) py * dst->w + rect.x) * 4;

        for (int x = 0; x < rect.w; x++) {
            int px = rect.x + x;
            if (px < 0 || px >= dst->w) {
                drow += 4;
                continue;
            }

            const unsigned char *src = g_bg;
            if (glyph_px != NULL) {
                int gx = x * MSP_FONT_GLYPH_W / rect.w;
                const unsigned char *sp = glyph_px + ((size_t) gy * MSP_FONT_GLYPH_W + gx) * 4;
                if (sp[3] >= 128) {
                    src = sp;   /* opaque glyph pixel */
                }
            }

            drow[0] = src[0];
            drow[1] = src[1];
            drow[2] = src[2];
            drow[3] = (src == g_bg) ? g_bg[3] : 255;
            drow += 4;
        }
    }
}

/* msp_canvas glyph sink: record each on-grid glyph into the decoding scratch. The BF attr byte
 * (page/blink/colour) is dropped.
 */
static void grid_sink(void *ctx, int row, int col, int attr, unsigned char glyph)
{
    (void) ctx;
    (void) attr;
    if (row >= 0 && row < BTFL_OSD_ROWS && col >= 0 && col < BTFL_OSD_COLS) {
        g_new[row][col] = glyph;
    }
}

int btfl_osd_update(surface_t *dst, const unsigned char *canvas, int len, rect_t *rects, int max_rects)
{
    if (dst == NULL || dst->px == NULL || !msp_font_is_loaded()) {
        return 0;
    }

    /* Decode the canvas into a fresh grid (cells no record touches are empty). */
    for (int r = 0; r < BTFL_OSD_ROWS; r++) {
        for (int c = 0; c < BTFL_OSD_COLS; c++) {
            g_new[r][c] = CELL_EMPTY;
        }
    }
    const msp_canvas_sink_t sink = { NULL, grid_sink };
    msp_canvas_parse(canvas, len, &sink, NULL);

    /* Redraw only the cells whose glyph changed; collect their rectangles. */
    int count = 0;
    int overflow = 0;
    for (int r = 0; r < BTFL_OSD_ROWS; r++) {
        for (int c = 0; c < BTFL_OSD_COLS; c++) {
            if (g_new[r][c] == g_grid[r][c]) {
                continue;
            }

            g_grid[r][c] = g_new[r][c];
            draw_cell(dst, r, c, g_new[r][c]);

            if (count < max_rects) {
                rects[count] = btfl_osd_cell_rect(r, c);
            } else {
                overflow = 1;
            }
            count++;
        }
    }

    if (count == 0) {
        return 0;
    }

    if (overflow) {
        return -1;   /* more cells changed than rects can hold; present the whole surface */
    }

    return count;
}

int btfl_osd_clear(surface_t *dst, rect_t *rects, int max_rects)
{
    if (dst == NULL || dst->px == NULL || !msp_font_is_loaded()) {
        return 0;
    }

    int count = 0;
    int overflow = 0;
    for (int r = 0; r < BTFL_OSD_ROWS; r++) {
        for (int c = 0; c < BTFL_OSD_COLS; c++) {
            /* Empty cells drew nothing, so there is nothing to clear. UNSET cells (fresh grid or
             * post-invalidate) never drew either - clearing them would count every cell, overflow
             * the rect list and force a full-plane present that clobbers the System OSD strip.
             * They stay UNSET so the next update still does its full redraw. */
            if (g_grid[r][c] == CELL_EMPTY || g_grid[r][c] == CELL_UNSET) {
                continue;
            }

            draw_cell(dst, r, c, CELL_EMPTY);   /* paint the cell back to the background */
            g_grid[r][c] = CELL_EMPTY;

            if (count < max_rects) {
                rects[count] = btfl_osd_cell_rect(r, c);
            } else {
                overflow = 1;
            }
            count++;
        }
    }

    if (count == 0) {
        return 0;
    }

    if (overflow) {
        return -1;
    }

    return count;
}
