/**
 * @file msp_font.h
 * @brief Loads an HD MSP-OSD glyph font (the standard `font_<fcid>_hd.png`) at runtime.
 *
 * One font format for the whole project: the wtfos/Walksnail HD font PNG, 24x36 RGBA glyphs stacked
 * vertically (256 per page; we use page 0). The MSP DisplayPort canvas references glyphs by MAX7456
 * index (0..255); this maps an index to its 24x36 RGBA bitmap (white glyph + black outline +
 * transparent). The SAME loader serves the bundled default and a user's custom font from the SD, so
 * there is one glyph path, not two (see docs/msp-osd-format.md).
 *
 * The bundled default (`font_BTFL_hd.png`) is generated from `assets/osd-fonts/betaflight.mcm` by
 * `assets/osd-fonts/mcm2png.py`. The font is a SEPARATE FILE shipped to the device, never baked in.
 *
 * VERBATIM COPY of libre/app/osd/msp_font.h (libre stays untouched).
 */
#ifndef MSP_FONT_H
#define MSP_FONT_H

#define MSP_FONT_GLYPHS  256
#define MSP_FONT_GLYPH_W 24
#define MSP_FONT_GLYPH_H 36

/**
 * @brief Load an HD font PNG from @p path into the glyph store.
 * @return 0 on success, -1 on any error (missing/undecodable file, or smaller than 24 x 36*256).
 *         On failure the previously loaded font is kept.
 */
int msp_font_load(const char *path);

/** @brief Is a font currently loaded (so glyphs can be drawn)? */
int msp_font_loaded(void);

/**
 * @brief RGBA pixels of glyph @p code, row-major, MSP_FONT_GLYPH_W * MSP_FONT_GLYPH_H * 4 bytes.
 * @return The glyph buffer, or NULL if @p code is out of range or no font is loaded.
 */
const unsigned char *msp_font_glyph(int code);

#endif /* MSP_FONT_H */
