#!/usr/bin/env python3
"""
Convert a Betaflight .mcm (MAX7456: 256 glyphs, 12x18, 2 bpp) to the HD MSP-OSD font PNG
(font_<fcid>_hd.png): 24x36 glyphs (2x), stacked vertically, RGBA (white glyph, black
outline, transparent background). This is the runtime font format documented in
docs/msp-osd-format.md.
"""
import sys
from PIL import Image

SRC_W, SRC_H = 12, 18
SCALE = 2
DST_W, DST_H = SRC_W * SCALE, SRC_H * SCALE   # 24x36
GLYPH_COUNT = 256

# MAX7456 glyph layout in the .mcm text body.
LINES_PER_GLYPH = 64                  # 54 carry pixels, the rest is padding
LINES_PER_ROW = 3                     # each 12-pixel row is split across 3 lines
PIXELS_PER_LINE = 4                   # 4 pixels per line, 2 bits ("high low") each
PIXEL_LINES = SRC_H * LINES_PER_ROW   # 54

# Decoded per-pixel values.
TRANSPARENT, BLACK, WHITE = 0, 1, 2

Glyph = list[list[int]]  # SRC_H rows of SRC_W pixel values


def decode_mcm(path: str) -> list[Glyph]:
    with open(path) as mcm:
        lines = mcm.read().splitlines()
    assert lines[0].startswith("MAX7456"), "not a .mcm file"

    body = lines[1:]
    glyphs: list[Glyph] = []
    for glyph_index in range(GLYPH_COUNT):
        pixels: Glyph = [[TRANSPARENT] * SRC_W for _ in range(SRC_H)]
        base = glyph_index * LINES_PER_GLYPH
        for line_index in range(PIXEL_LINES):
            row = line_index // LINES_PER_ROW
            col_base = (line_index % LINES_PER_ROW) * PIXELS_PER_LINE
            line = body[base + line_index]

            for pixel in range(PIXELS_PER_LINE):
                high, low = line[pixel * 2], line[pixel * 2 + 1]
                if high == "0" and low == "0":
                    value = BLACK
                elif high == "1" and low == "0":
                    value = WHITE
                else:
                    value = TRANSPARENT

                pixels[row][col_base + pixel] = value

        glyphs.append(pixels)

    return glyphs


def render_png(glyphs: list[Glyph], dst: str) -> Image.Image:
    img = Image.new("RGBA", (DST_W, GLYPH_COUNT * DST_H), (0, 0, 0, 0))
    canvas = img.load()
    for glyph_index, glyph in enumerate(glyphs):
        for row in range(SRC_H):
            for col in range(SRC_W):
                value = glyph[row][col]
                if value == TRANSPARENT:
                    continue

                color = (255, 255, 255, 255) if value == WHITE else (0, 0, 0, 255)
                for dy in range(SCALE):
                    for dx in range(SCALE):
                        canvas[col * SCALE + dx, glyph_index * DST_H + row * SCALE + dy] = color

    img.save(dst)
    return img


def main() -> None:
    src = sys.argv[1] if len(sys.argv) > 1 else "assets/osd-fonts/betaflight.mcm"
    dst = sys.argv[2] if len(sys.argv) > 2 else "assets/osd-fonts/font_BTFL_hd.png"
    glyphs = decode_mcm(src)
    img = render_png(glyphs, dst)
    print(f"wrote {dst}: {img.width}x{img.height} ({GLYPH_COUNT} glyphs of {DST_W}x{DST_H})")


if __name__ == "__main__":
    main()
