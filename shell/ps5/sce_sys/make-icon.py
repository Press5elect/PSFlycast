#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the PSFlyCast contributors
"""PSFlyCast - draws the title's icon, sce_sys/icon0.png.

    python3 shell/ps5/sce_sys/make-icon.py [Roboto-Bold.ttf]

The app's mark over its name: a disc made of one line, which starts at the
disc's left edge, winds in to the centre in two and a half turns and winds
out again to the right edge (the same line as spiralLine in bigpicture.cpp).
Drawn four times as large and scaled down. Needs Pillow; the font is Roboto
Bold, as in the interface (Flycast's own fonts/ has it compressed; any copy
of the font's file can be named).
"""

import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SIZE, SCALE = 512, 4
ACCENT = (26, 159, 255)
TOP, BOTTOM = (14, 19, 26), (23, 30, 40)


def spiral(radius, cx, cy, half=440, turns=2.5):
    points = []
    for k in range(2 * half + 1):
        u = k / half - 1
        angle, r, side = turns * 2 * math.pi * abs(u), radius * abs(u), 1 if u < 0 else -1
        points.append((cx + side * r * math.cos(angle), cy + side * r * math.sin(angle)))
    return points


def main():
    font_file = sys.argv[1] if len(sys.argv) > 1 else "Roboto-Bold.ttf"
    n = SIZE * SCALE
    cx, cy, radius = n / 2, 202 * SCALE, 128 * SCALE
    # The ground: the interface's gradient, and light where the disc is.
    image = Image.new("RGB", (n, n))
    draw = ImageDraw.Draw(image)
    for y in range(n):
        t = y / (n - 1)
        draw.line([(0, y), (n, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(TOP, BOTTOM)))
    light = Image.new("L", (n, n), 0)
    ImageDraw.Draw(light).ellipse([cx - radius * 1.9, cy - radius * 1.9, cx + radius * 1.9, cy + radius * 1.9], fill=52)
    light = light.filter(ImageFilter.GaussianBlur(radius * 0.5))
    image.paste(Image.new("RGB", (n, n), ACCENT), (0, 0), light)

    # The disc's body, lighter toward the hub, and its rim.
    outer = radius * 1.14
    body = Image.new("L", (n, n), 0)
    body_draw = ImageDraw.Draw(body)
    steps = 64
    for i in range(steps):
        r = outer * (1 - i / steps)
        body_draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=round(255 * (0.12 + 0.24 * i / steps)))
    image.paste(Image.new("RGB", (n, n), ACCENT), (0, 0), body)
    over = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    over_draw = ImageDraw.Draw(over)
    rim = round(radius * 0.018)
    over_draw.ellipse([cx - outer, cy - outer, cx + outer, cy + outer], outline=(255, 255, 255, 150), width=rim)
    hub = radius * 0.085
    over_draw.ellipse([cx - hub, cy - hub, cx + hub, cy + hub], fill=TOP + (255,), outline=(255, 255, 255, 150), width=rim)

    # The line, over its own light.
    line = spiral(radius, cx, cy)
    width = round(radius * 0.05)
    glow = Image.new("L", (n, n), 0)
    ImageDraw.Draw(glow).line(line, fill=70, width=width * 2, joint="curve")
    glow = glow.filter(ImageFilter.GaussianBlur(width * 0.9))
    image.paste(Image.new("RGB", (n, n), ACCENT), (0, 0), glow)
    over_draw.line(line, fill=(255, 255, 255, 255), width=width, joint="curve")
    for x, y in (line[0], line[-1]):
        over_draw.ellipse([x - width / 2, y - width / 2, x + width / 2, y + width / 2], fill=(255, 255, 255, 255))

    # The name.
    font = ImageFont.truetype(font_file, 76 * SCALE)
    left, top, right, bottom = over_draw.textbbox((0, 0), "PSFlyCast", font=font)
    over_draw.text((cx - (right - left) / 2 - left, 392 * SCALE), "PSFlyCast", font=font, fill=(255, 255, 255, 255))

    image = Image.alpha_composite(image.convert("RGBA"), over).convert("RGB")
    out = Path(__file__).resolve().parent / "icon0.png"
    image.resize((SIZE, SIZE), Image.LANCZOS).save(out, optimize=True)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
