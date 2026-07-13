# OSD glyph fonts (MSP / FC OSD)

Source glyph fonts for the MSP DisplayPort / FC OSD renderer (see
`docs/reference/msp-osd-format.md`). These are **source assets**; the menu loads a
converted runtime atlas (a separate file shipped to the device), never a font baked
into the binary. Users can later supply their own font (the custom-font-from-SD
feature, deferred).

## betaflight.mcm , default

The standard Betaflight OSD font: MAX7456 character map, **256 glyphs, 12x18 px each**,
2 bits/pixel (0 = black, 1/3 = transparent, 2 = white).

- **Source:** `betaflight/betaflight-configurator`, `resources/osd/2/betaflight.mcm`.
- **License:** GPLv3 (ships with the GPLv3 Betaflight Configurator). Attribution:
  the Betaflight project. Redistributed here under the same terms.

The glyph indices are the standard MAX7456 codes the MSP DisplayPort stream uses, so
this renders Betaflight/INAV OSD canvases directly. For our HD canvas (53x20) the
12x18 glyphs are upscaled to the HD cell size by the converter.
