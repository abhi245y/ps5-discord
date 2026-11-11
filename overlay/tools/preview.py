#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws the overlay previews tests/preview.cpp wrote (<dir>/*.txt) to <dir>/*.png, over a
dim game-like backdrop, with the overlay's art (app/assets/overlay) and the app's font.
Labels are approximations of PUI's: the text centred in its box's height."""
import glob
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(__file__)
ART = os.path.join(HERE, "../../app/assets/overlay")
BOLD = os.path.join(HERE, "../../app/third_party/fonts/NunitoSans-Bold.ttf")
REGULAR = next((p for p in (
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
) if os.path.isfile(p)), "DejaVuSans.ttf")  # the preview only


def tinted(img, tint):
    r, g, b, a = (int(round(v * 255)) for v in tint)
    px = img.split()
    out = [p.point(lambda v, m=m: v * m // 255) for p, m in zip(px, (r, g, b, a))]
    return Image.merge("RGBA", out)


def avatar(size):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(img).ellipse((0, 0, size - 1, size - 1), fill=(120, 140, 200, 255))
    return img


def render(txt):
    screen = Image.new("RGBA", (1920, 1080), (40, 60, 90, 255))
    d0 = ImageDraw.Draw(screen)
    for y in range(1080):  # a game-ish gradient
        d0.line((0, y, 1920, y), fill=(30 + y // 20, 50 + y // 30, 80 + y // 25, 255))
    for line in open(txt, encoding="utf-8"):
        f = line.rstrip("\n").split("\t")
        kind, x, y, w, h = f[0], *map(float, f[1:5])
        path, tint, size, weight, ink, align, text = f[5], f[6], float(f[7]), int(f[8]), f[9], int(f[10]), f[11]
        box = (int(round(x)), int(round(y)), max(1, int(round(w))), max(1, int(round(h))))
        if kind == "i":
            name = os.path.basename(path)
            if "/av/" in path:
                img = avatar(box[2])
            elif path and os.path.isfile(path):
                img = Image.open(path).convert("RGBA")
            elif not name or not os.path.exists(os.path.join(ART, name)):
                continue
            else:
                img = Image.open(os.path.join(ART, name)).convert("RGBA")
            img = tinted(img.resize((box[2], box[3]), Image.LANCZOS), tuple(map(float, tint.split(","))))
            screen.alpha_composite(img, (box[0], box[1]))
        elif kind == "p":
            panel = Image.new("RGBA", (box[2], box[3]), tuple(int(round(v * 255)) for v in map(float, tint.split(","))))
            screen.alpha_composite(panel, (box[0], box[1]))
        elif kind == "l" and text:
            font = ImageFont.truetype(BOLD if weight in (1, 3) else REGULAR, max(8, int(size)))
            col = tuple(int(round(v * 255)) for v in map(float, ink.split(",")))
            if col[3] == 0:  # inkless labels only measure text (the console draws nothing)
                continue
            d = ImageDraw.Draw(screen)
            tw = d.textlength(text, font=font)
            while tw > box[2] and len(text) > 1:  # PUI ends long text in an ellipsis
                text = text[:-2] + "…"
                tw = d.textlength(text, font=font)
            tx = box[0] if align == 0 else box[0] + (box[2] - tw) / 2 if align == 1 else box[0] + box[2] - tw
            d.text((tx, box[1] + box[3] / 2), text, font=font, fill=col, anchor="lm")
            if not os.path.basename(txt).startswith("lab-"):
                d.rectangle(box[:2] + (box[0] + box[2], box[1] + box[3]), outline=(255, 0, 255, 60))
    out = txt[:-4] + ".png"
    screen.convert("RGB").save(out)
    return out


if __name__ == "__main__":
    for txt in sorted(glob.glob(os.path.join(sys.argv[1] if len(sys.argv) > 1 else ".", "*.txt"))):
        print(render(txt))
