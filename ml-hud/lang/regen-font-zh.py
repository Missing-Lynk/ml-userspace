#!/usr/bin/env python3
"""
Regenerate ml-hud/src/services/font_zh.c, the baked CJK glyph subset wired as Montserrat's fallback
so the menu renders Chinese. The subset is exactly the glyphs the zh catalog (ml-hud/lang/zh.lang)
uses, at the menu font size. Re-run this AFTER editing zh.lang, otherwise newly added characters
render as missing boxes (the font only carries the glyphs it was built with).

Needs: fontTools (extract the SC face from the Noto CJK collection), node/npx (lv_font_conv, the
canonical LVGL converter, fetched on demand - it has no Python equivalent, so it stays the one
subprocess), and a Noto Sans CJK source font (Debian: fonts-noto-cjk).

Env (same as the old shell script): NOTO_TTC = path to the .ttc collection, SC_INDEX = face index
of Noto Sans CJK SC inside it (default 2).
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from fontTools.ttLib import TTCollection

HUD = Path(__file__).resolve().parent.parent   # hud/
OUT = HUD / "src/services/font_zh.c"
CATALOG = HUD / "lang/zh.lang"

# CJK strings the menu hardcodes outside the catalog: the language endonyms in menu.c's
# language_options ("中文"), which must render even in the English catalog.
EXTRA_GLYPHS = "中文"

FONT_SIZE = "48"   # px, matching the menu font
FONT_BPP = "4"


def glyph_set() -> str:
    """Every non-ASCII character in zh.lang's VALUES plus EXTRA_GLYPHS, sorted + de-duplicated."""
    chars = {c for c in EXTRA_GLYPHS if ord(c) > 0x7F and not c.isspace()}
    for line in CATALOG.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if not s or s.startswith("#") or "=" not in s:
            continue
        chars.update(c for c in s.split("=", 1)[1] if ord(c) > 0x7F and not c.isspace())

    return "".join(sorted(chars))


def main() -> int:
    ttc = Path(os.environ.get("NOTO_TTC", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"))
    sc_index = int(os.environ.get("SC_INDEX", "2"))   # face 2 = Noto Sans CJK SC

    if not ttc.is_file():
        print(f"Noto CJK source not found: {ttc} (set NOTO_TTC, or apt install fonts-noto-cjk)",
              file=sys.stderr)
        return 1

    symbols = glyph_set()

    # Extract the SC face from the collection into a standalone OTF (lv_font_conv takes one face),
    # then render the subset to the LVGL C font.
    with tempfile.NamedTemporaryFile(suffix=".otf") as otf:
        TTCollection(str(ttc)).fonts[sc_index].save(otf.name)
        try:
            # cwd=HUD + relative -o keeps the machine-specific path out of the generated header
            subprocess.run(
                ["npx", "--yes", "lv_font_conv",
                 "--font", otf.name, "--size", FONT_SIZE, "--bpp", FONT_BPP, "--format", "lvgl",
                 "--no-compress", "--no-kerning", "--symbols", symbols,
                 "-o", str(OUT.relative_to(HUD))],
                check=True, cwd=HUD)
        except FileNotFoundError:
            print("npx not found: lv_font_conv needs node/npm installed", file=sys.stderr)
            return 1

    print(f"regenerated {OUT.relative_to(HUD)} ({len(symbols)} glyphs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
