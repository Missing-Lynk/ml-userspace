/** @file msp_font.c @brief Implementation; see msp_font.h and docs/reference/msp-osd-format.md.
 *  VERBATIM COPY of libre/app/osd/msp_font.c (libre stays untouched). */
#include "msp_font.h"
#include "lodepng.h"

#include <stdlib.h>
#include <string.h>

#define GLYPH_STRIDE (MSP_FONT_GLYPH_W * 4)          /* bytes per glyph row (RGBA) */
#define GLYPH_BYTES  (GLYPH_STRIDE * MSP_FONT_GLYPH_H)

static unsigned char g_glyphs[MSP_FONT_GLYPHS][GLYPH_BYTES];
static int           g_loaded;

int msp_font_load(const char *path)
{
    unsigned char *img = NULL;
    unsigned w = 0;
    unsigned h = 0;
    if (lodepng_decode32_file(&img, &w, &h, path) != 0) {
        return -1;
    }

    /* Standard HD sheet: 256 glyphs stacked vertically, 24 wide, 36*256 tall (we use page 0 = x 0..23). */
    if (w < MSP_FONT_GLYPH_W || h < (unsigned) (MSP_FONT_GLYPH_H * MSP_FONT_GLYPHS)) {
        free(img);
        return -1;
    }

    for (int g = 0; g < MSP_FONT_GLYPHS; g++) {
        for (int r = 0; r < MSP_FONT_GLYPH_H; r++) {
            size_t src_row = (size_t) (g * MSP_FONT_GLYPH_H + r) * w * 4;   /* x = 0 (page 0) */
            memcpy(g_glyphs[g] + (size_t) r * GLYPH_STRIDE, img + src_row, GLYPH_STRIDE);
        }
    }

    free(img);
    g_loaded = 1;

    return 0;
}

int msp_font_loaded(void)
{
    return g_loaded;
}

const unsigned char *msp_font_glyph(int code)
{
    if (!g_loaded || code < 0 || code >= MSP_FONT_GLYPHS) {
        return NULL;
    }

    return g_glyphs[code];
}
