#!/usr/bin/env python3
"""The app's launcher icon, from Emerald 3DS's HOME Menu icon (48x48 pixel art).

Android 13+ draws every app icon as an adaptive icon: a 108 dp foreground
over a background, cut to the launcher's own shape (circle, squircle...),
of which only about the middle 72 dp shows. The art is scaled whole into that
middle, by a whole number with nearest-neighbour so its pixels stay crisp (6x
at xxxhdpi: 288 of 432 px), and the background is the colour of its edges, so
whatever the shape cuts reads as one tile.

  make_icon.py [ICON.png] [RES_DIR]

Writes RES_DIR/mipmap-anydpi-v26/ic_launcher.xml, the foreground at
drawable-xxxhdpi and the background colour (values/ic_launcher.xml) - the
round icon - and the art itself as a plain bitmap at 1x to 4x
(mipmap-*/ic_launcher_bitmap.png), the app's icon: some launchers (the iiSU
frontend) draw a bitmap icon and nothing for an adaptive one.
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
ICON = HERE.parent.parent / "3ds_port/assets/artwork/icon.png"
RES = HERE.parent / "app/res"
LAYER = 432          # 108 dp at xxxhdpi
SAFE = 288           # 72 dp: what every mask shows


def edge_colour(art: Image.Image) -> tuple[int, int, int]:
    w, h = art.size
    px = art.load()
    edge = [px[x, y] for x in range(w) for y in (0, h - 1)] + [px[x, y] for y in range(h) for x in (0, w - 1)]
    return tuple(sum(c[i] for c in edge) // len(edge) for i in range(3))


def main() -> None:
    icon = Path(sys.argv[1]) if len(sys.argv) > 1 else ICON
    res = Path(sys.argv[2]) if len(sys.argv) > 2 else RES
    art = Image.open(icon).convert("RGBA")
    scale = SAFE // max(art.size)
    big = art.resize((art.width * scale, art.height * scale), Image.NEAREST)
    layer = Image.new("RGBA", (LAYER, LAYER), (0, 0, 0, 0))
    layer.paste(big, ((LAYER - big.width) // 2, (LAYER - big.height) // 2))

    (res / "drawable-xxxhdpi").mkdir(parents=True, exist_ok=True)
    layer.save(res / "drawable-xxxhdpi/ic_launcher_foreground.png", optimize=True)
    r, g, b = edge_colour(art.convert("RGB"))
    (res / "values").mkdir(parents=True, exist_ok=True)
    (res / "values/ic_launcher.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
        '    <!-- The icon art\'s edge colour (tools/make_icon.py) -->\n'
        '    <color name="ic_launcher_background">#%02X%02X%02X</color>\n</resources>\n' % (r, g, b))
    (res / "mipmap-anydpi-v26").mkdir(parents=True, exist_ok=True)
    (res / "mipmap-anydpi-v26/ic_launcher.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<!-- Made by tools/make_icon.py from 3ds_port/assets/artwork/icon.png -->\n'
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@color/ic_launcher_background" />\n'
        '    <foreground android:drawable="@drawable/ic_launcher_foreground" />\n'
        '</adaptive-icon>\n')
    # mdpi 48, xhdpi 96, xxhdpi 144, xxxhdpi 192: whole multiples of the art.
    for folder, k in (("mipmap-mdpi", 1), ("mipmap-xhdpi", 2), ("mipmap-xxhdpi", 3), ("mipmap-xxxhdpi", 4)):
        (res / folder).mkdir(parents=True, exist_ok=True)
        art.convert("RGB").resize((art.width * k, art.height * k), Image.NEAREST).save(
            res / folder / "ic_launcher_bitmap.png", optimize=True)
    print("make_icon: %s x%d, background #%02X%02X%02X -> %s" % (icon.name, scale, r, g, b, res))


if __name__ == "__main__":
    main()
