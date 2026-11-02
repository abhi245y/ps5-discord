#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The in-game overlay's art (docs/overlay-plan.md mockups A and C), drawn at the exact
size each piece shows at on a 1920x1080 screen, anti-aliased (4x, then scaled down).
Shapes are drawn in their final colours; icons are white, and the overlay tints them.

    python3 overlay/tools/overlay_art.py app/assets/overlay

The sizes here and in overlay/src/app/discord_app.inc go together.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "app/assets/overlay"
FONT = os.path.join(os.path.dirname(__file__), "../../app/third_party/fonts/NunitoSans-Bold.ttf")
S = 4  # supersampling

DOCK = (28, 30, 35, 255)
CARD = (38, 40, 46, 255)
TILE = (44, 46, 52, 255)
CORAL = (255, 122, 89, 255)
LEAVE = (74, 38, 36, 255)
GLANCE = (32, 34, 39, 235)
LINE = (255, 255, 255, 30)
WHITE = (255, 255, 255, 255)


def canvas(w, h):
    img = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img)


def save(img, name, w, h):
    img.resize((w, h), Image.LANCZOS).save(os.path.join(OUT, name + ".png"), optimize=True)


def rounded(name, w, h, r, fill, outline=None, width=0):
    img, d = canvas(w, h)
    inset = width * S // 2 if outline else 0
    d.rounded_rectangle((inset, inset, w * S - 1 - inset, h * S - 1 - inset), r * S, fill=fill,
                        outline=outline, width=width * S)
    save(img, name, w, h)


def ring(name, size, width, colour):
    img, d = canvas(size, size)
    d.ellipse((width * S // 2, width * S // 2, size * S - width * S // 2, size * S - width * S // 2),
              outline=colour, width=width * S)
    save(img, name, size, size)


def icon(name, draw, size=64):
    img, d = canvas(size, size)
    draw(d, size * S / 64.0)
    save(img, name, size, size)


def mic(d, k, slash=False):
    d.rounded_rectangle((24 * k, 8 * k, 40 * k, 38 * k), 8 * k, fill=WHITE)
    d.arc((16 * k, 18 * k, 48 * k, 46 * k), 0, 180, fill=WHITE, width=int(4 * k))
    d.line((32 * k, 46 * k, 32 * k, 54 * k), fill=WHITE, width=int(4 * k))
    d.line((24 * k, 55 * k, 40 * k, 55 * k), fill=WHITE, width=int(4 * k))
    if slash:
        d.line((14 * k, 10 * k, 52 * k, 56 * k), fill=WHITE, width=int(5 * k))


def headphones(d, k, slash=False):
    d.arc((12 * k, 10 * k, 52 * k, 50 * k), 180, 360, fill=WHITE, width=int(5 * k))
    d.line((14 * k, 30 * k, 14 * k, 36 * k), fill=WHITE, width=int(5 * k))
    d.line((50 * k, 30 * k, 50 * k, 36 * k), fill=WHITE, width=int(5 * k))
    d.rounded_rectangle((9 * k, 34 * k, 21 * k, 54 * k), 5 * k, fill=WHITE)
    d.rounded_rectangle((43 * k, 34 * k, 55 * k, 54 * k), 5 * k, fill=WHITE)
    if slash:
        d.line((10 * k, 10 * k, 54 * k, 56 * k), fill=WHITE, width=int(5 * k))


def golive(d, k):
    d.ellipse((26 * k, 26 * k, 38 * k, 38 * k), fill=WHITE)
    for r in (13, 22):
        box = (32 * k - r * k, 32 * k - r * k, 32 * k + r * k, 32 * k + r * k)
        d.arc(box, 135, 225, fill=WHITE, width=int(4 * k))
        d.arc(box, -45, 45, fill=WHITE, width=int(4 * k))


def leave(d, k):
    # A handset lying down: a wide arc with the two ends.
    d.arc((6 * k, 22 * k, 58 * k, 58 * k), 200, 340, fill=WHITE, width=int(7 * k))
    d.rounded_rectangle((5 * k, 30 * k, 19 * k, 40 * k), 4 * k, fill=WHITE)
    d.rounded_rectangle((45 * k, 30 * k, 59 * k, 40 * k), 4 * k, fill=WHITE)


def clock(d, k):
    d.ellipse((8 * k, 8 * k, 56 * k, 56 * k), fill=WHITE)
    d.line((32 * k, 18 * k, 32 * k, 33 * k), fill=(0, 0, 0, 0), width=int(5 * k))
    d.line((31 * k, 33 * k, 42 * k, 40 * k), fill=(0, 0, 0, 0), width=int(5 * k))


def bars(d, k):
    for i, h in enumerate((10, 22, 34, 22, 10)):
        x = 12 + i * 9
        d.rounded_rectangle((x * k, (32 - h / 2) * k, (x + 5) * k, (32 + h / 2) * k), 2.5 * k, fill=WHITE)


def glyph(name, size, colour, shape):
    img, d = canvas(size, size)
    k = size * S / 64.0
    d.ellipse((3 * k, 3 * k, 61 * k, 61 * k), outline=colour, width=int(4 * k))
    if shape == "cross":
        d.line((21 * k, 21 * k, 43 * k, 43 * k), fill=colour, width=int(5 * k))
        d.line((43 * k, 21 * k, 21 * k, 43 * k), fill=colour, width=int(5 * k))
    else:
        d.ellipse((19 * k, 19 * k, 45 * k, 45 * k), outline=colour, width=int(5 * k))
    save(img, name, size, size)


def text_pill(name, w, h, fill, text, size, outline=False, dot=False):
    img, d = canvas(w, h)
    if outline:
        d.rounded_rectangle((S, S, w * S - 1 - S, h * S - 1 - S), 7 * S, outline=(200, 205, 215, 255), width=2 * S)
    else:
        d.rounded_rectangle((0, 0, w * S - 1, h * S - 1), h * S // 2, fill=fill)
    font = ImageFont.truetype(FONT, size * S)
    x0 = 0
    if dot:
        x0 = 9 * S
    tw = d.textlength(text, font=font)
    x = (w * S - tw + x0) / 2
    if dot:
        r = 4 * S
        d.ellipse((x - 10 * S - r, h * S / 2 - r, x - 10 * S + r, h * S / 2 + r), fill=WHITE)
    colour = (200, 205, 215, 255) if outline else WHITE
    d.text((x, h * S / 2), text, font=font, fill=colour, anchor="lm")
    save(img, name, w, h)


def main():
    os.makedirs(OUT, exist_ok=True)
    # A: the call panel.
    # The panel's height follows its rows (discord_app.inc dock_height): 0 to 4.
    for n in range(5):
        rounded("dock_%d" % n, 568, 264 if n == 0 else 430 + 143 * n, 28, DOCK)
    rounded("row", 519, 132, 16, CARD)
    rounded("tile", 122, 140, 16, TILE)
    rounded("tile_live", 122, 140, 16, CORAL)
    rounded("tile_leave", 122, 140, 16, LEAVE)
    rounded("tile_focus", 134, 152, 21, None, WHITE, 3)
    rounded("line", 519, 2, 0, LINE)
    ring("ring", 106, 4, CORAL)
    glyph("glyph_cross", 37, (122, 160, 255, 255), "cross")
    glyph("glyph_circle", 37, (255, 92, 110, 255), "circle")
    text_pill("chip_options", 78, 28, None, "OPTIONS", 12, outline=True)
    # C: the glance card, one per number of talking rows (0-3).
    for n in range(4):
        rounded("glance_%d" % n, 315, 205 + 60 * n, 18, GLANCE)
    rounded("glance_line", 275, 2, 0, LINE)
    ring("ring_small", 56, 3, CORAL)
    ring("you", 50, 2, (200, 205, 215, 255))
    text_pill("pill_live", 79, 34, CORAL, "LIVE", 17, dot=True)
    # Icons: white, tinted where they are used.
    icon("mic", lambda d, k: mic(d, k))
    icon("mic_off", lambda d, k: mic(d, k, True))
    icon("headphones", lambda d, k: headphones(d, k))
    icon("headphones_off", lambda d, k: headphones(d, k, True))
    icon("golive", golive)
    icon("leave", leave)
    icon("clock", clock)
    icon("bars", bars)


if __name__ == "__main__":
    main()
