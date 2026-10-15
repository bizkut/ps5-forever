#!/usr/bin/env python3
# The title's home-screen icon, sce_sys/icon0.png, drawn from nothing but
# shapes, so its origin is this script. Forever Launcher replaces the copies
# the home screen shows with the installed Battle.net Launcher.exe's icon once
# Battle.net is installed (psmkit-wine/title-icon.h); this one shows until then
# and is the icon the homebrew catalog lists.
#
# Adapted from ps5-forever ps5/tools/make-title-art.py (commit 6cf23f2d):
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: MIT
# Changes: writes ps5/title/sce_sys/icon0.png.
#
#   python3 ps5/title/tools/make-title-art.py      write the file
#
# A wine glass on a dark blue tile, its wine the PlayStation-blue of a pad's
# light bar. Drawn at 4x and reduced, so the edges are smooth; the same
# bytes on every machine.
from pathlib import Path

from PIL import Image, ImageDraw

TITLE = Path(__file__).resolve().parent.parent
SIZE, SCALE = 512, 4


def icon() -> Image.Image:
    n = SIZE * SCALE
    image = Image.new("RGB", (n, n))
    draw = ImageDraw.Draw(image)
    # A vertical gradient, deep navy to near black
    for y in range(n):
        t = y / (n - 1)
        draw.line([(0, y), (n, y)], fill=(round(18 - 10 * t), round(26 - 14 * t), round(58 - 32 * t)))

    def box(x0, y0, x1, y1):
        return [round(v * SCALE) for v in (x0, y0, x1, y1)]

    glass, wine = (232, 238, 248), (0, 112, 224)
    # The bowl: a rounded cup, its wine the lower part, a highlight on the left
    draw.chord(box(136, 40, 376, 336), 0, 180, fill=glass)
    draw.rectangle(box(136, 112, 376, 188), fill=glass)
    draw.chord(box(152, 56, 360, 320), 0, 180, fill=wine)
    draw.rectangle(box(152, 188, 360, 190), fill=wine)
    draw.rectangle(box(152, 120, 360, 188), fill=(20, 30, 62))
    # The highlight follows the bowl's left wall, a little inside it
    draw.arc(box(168, 72, 344, 304), 118, 172, fill=(90, 170, 245), width=10 * SCALE)
    # The stem and the foot
    draw.rectangle(box(246, 330, 266, 426), fill=glass)
    draw.ellipse(box(176, 410, 336, 450), fill=glass)
    return image.resize((SIZE, SIZE), Image.LANCZOS)


def main() -> int:
    icon().save(TITLE / "sce_sys" / "icon0.png", optimize=True)
    print("sce_sys/icon0.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
