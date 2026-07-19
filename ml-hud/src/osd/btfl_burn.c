/** @file btfl_burn.c @brief See btfl_burn.h. */
#include "btfl_burn.h"
#include "btfl_osd.h"
#include "msp_font.h"
#include "msp_canvas.h"
#include "../services/pipecmd.h"

#include <string.h>

#define CELL_EMPTY  (-1)   /* no glyph in this cell */
#define CELL_UNSET  (-2)   /* grid reset: forces a full re-send on the next update */

/* Largest cell patch we render (1920/53 x 970/20 is ~37x49; the wire caps at 64x64). */
#define BURN_WMAX 64
#define BURN_HMAX 64

static int g_grid[BTFL_OSD_ROWS][BTFL_OSD_COLS];   /* last sent glyph per cell */
static int g_new[BTFL_OSD_ROWS][BTFL_OSD_COLS];    /* the decoding scratch for one update */

void btfl_burn_invalidate(void)
{
    for (int r = 0; r < BTFL_OSD_ROWS; r++) {
        for (int c = 0; c < BTFL_OSD_COLS; c++) {
            g_grid[r][c] = CELL_UNSET;
        }
    }
}

/* Render one glyph into an RGBA patch of the cell's exact rect: transparent background (a = 0),
 * opaque glyph pixels (a = 255). The nearest-neighbour scale and the a >= 128 opacity rule are
 * draw_cell's (btfl_osd.c), so the burned pixels match the displayed ones.
 */
static void render_patch(unsigned char *px, const rect_t *rect, const unsigned char *glyph_px)
{
    for (int y = 0; y < rect->h; y++) {
        int gy = y * MSP_FONT_GLYPH_H / rect->h;
        unsigned char *drow = px + (size_t) y * rect->w * 4;

        for (int x = 0; x < rect->w; x++) {
            int gx = x * MSP_FONT_GLYPH_W / rect->w;
            const unsigned char *sp = glyph_px + ((size_t) gy * MSP_FONT_GLYPH_W + gx) * 4;

            if (sp[3] >= 128) {
                drow[0] = sp[0];
                drow[1] = sp[1];
                drow[2] = sp[2];
                drow[3] = 255;
            } else {
                drow[0] = drow[1] = drow[2] = drow[3] = 0;
            }
            drow += 4;
        }
    }
}

/* msp_canvas glyph sink: record each on-grid glyph into the decoding scratch. */
static void grid_sink(void *ctx, int row, int col, int attr, unsigned char glyph)
{
    (void) ctx;
    (void) attr;
    if (row >= 0 && row < BTFL_OSD_ROWS && col >= 0 && col < BTFL_OSD_COLS) {
        g_new[row][col] = glyph;
    }
}

void btfl_burn_update(const unsigned char *canvas, int len)
{
    static unsigned char patch[BURN_WMAX * BURN_HMAX * 4];

    if (!msp_font_loaded()) {
        return;
    }

    for (int r = 0; r < BTFL_OSD_ROWS; r++) {
        for (int c = 0; c < BTFL_OSD_COLS; c++) {
            g_new[r][c] = CELL_EMPTY;
        }
    }
    const msp_canvas_sink_t sink = { NULL, grid_sink };
    msp_canvas_parse(canvas, len, &sink, NULL);

    for (int r = 0; r < BTFL_OSD_ROWS; r++) {
        for (int c = 0; c < BTFL_OSD_COLS; c++) {
            if (g_new[r][c] == g_grid[r][c]) {
                continue;
            }

            /* An UNSET cell that is empty now was never sent: nothing to clear pipeline-side. */
            if (g_grid[r][c] == CELL_UNSET && g_new[r][c] == CELL_EMPTY) {
                g_grid[r][c] = CELL_EMPTY;
                continue;
            }

            g_grid[r][c] = g_new[r][c];

            rect_t rect = btfl_osd_cell_rect(r, c);
            const unsigned char *glyph_px =
                g_new[r][c] >= 0 ? msp_font_glyph(g_new[r][c]) : NULL;

            if (glyph_px != NULL && rect.w > 0 && rect.h > 0
                && rect.w <= BURN_WMAX && rect.h <= BURN_HMAX) {
                render_patch(patch, &rect, glyph_px);
                pipecmd_osd_cell(r, c, rect.x, rect.y, rect.w, rect.h, patch);
            } else {
                pipecmd_osd_cell(r, c, rect.x, rect.y, rect.w, rect.h, NULL);   /* emptied cell */
            }
        }
    }
}
