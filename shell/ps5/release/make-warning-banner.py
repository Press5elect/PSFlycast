#!/usr/bin/env python3
"""
PSFlyCast - the red IMPORTANT WARNING banner at the top of the README and of
each release's notes (shell/ps5/screenshots/important-warning.png).

Copyright 2026 the PSFlyCast contributors
SPDX-License-Identifier: GPL-3.0-or-later

Drawn here, from nothing but the repository's own Roboto Bold (fonts/, Apache
License 2.0): a rounded red bar, a warning triangle and the two words.

    python3 shell/ps5/release/make-warning-banner.py shell/ps5/screenshots/important-warning.png

The picture is in the repository; this file is how to make it again. The same
picture comes out every time.
"""
import io
import os
import sys
import zipfile

from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT, SCALE = 1600, 220, 2      # drawn at twice the size, then reduced


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "important-warning.png"
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..")
    with zipfile.ZipFile(os.path.join(root, "fonts", "Roboto-Bold.ttf.zip")) as archive:
        font_data = archive.read(archive.namelist()[0])
    w, h = WIDTH * SCALE, HEIGHT * SCALE
    image = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((0, 0, w - 1, h - 1), radius=36 * SCALE, fill=(198, 40, 40, 255))
    draw.rounded_rectangle((8 * SCALE, 8 * SCALE, w - 1 - 8 * SCALE, h - 1 - 8 * SCALE), radius=29 * SCALE,
                           outline=(255, 255, 255, 110), width=3 * SCALE)
    font = ImageFont.truetype(io.BytesIO(font_data), 104 * SCALE)
    text = "IMPORTANT WARNING"
    left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
    side = 112 * SCALE                       # the triangle's width
    gap = 48 * SCALE
    total = side + gap + (right - left)
    x = (w - total) // 2
    cy = h // 2
    # The triangle: white, with the mark cut out of it in the bar's red.
    tall = int(side * 0.88)
    triangle = [(x + side // 2, cy - tall // 2), (x, cy + tall // 2), (x + side, cy + tall // 2)]
    draw.polygon(triangle, fill=(255, 255, 255, 255))
    bar = 13 * SCALE
    draw.rounded_rectangle((x + side // 2 - bar // 2, cy - tall // 2 + 30 * SCALE, x + side // 2 + bar // 2, cy + 14 * SCALE),
                           radius=bar // 2, fill=(198, 40, 40, 255))
    draw.ellipse((x + side // 2 - bar * 0.62, cy + 24 * SCALE, x + side // 2 + bar * 0.62, cy + 24 * SCALE + bar * 1.24),
                 fill=(198, 40, 40, 255))
    draw.text((x + side + gap - left, cy - (top + bottom) // 2), text, font=font, fill=(255, 255, 255, 255))
    image = image.resize((WIDTH, HEIGHT), Image.LANCZOS)
    image.save(out, optimize=True)
    print(f"{out}: {WIDTH} x {HEIGHT}")


if __name__ == "__main__":
    main()
