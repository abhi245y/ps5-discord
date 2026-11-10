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
GLASS = (32, 34, 39, 158)  # live-pill-demo.html: 62%, no blur in the overlay
MUTED = (237, 66, 69, 255)
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


def mic(d, k, slash=False, colour=WHITE):
    d.rounded_rectangle((24 * k, 8 * k, 40 * k, 38 * k), 8 * k, fill=colour)
    d.arc((16 * k, 18 * k, 48 * k, 46 * k), 0, 180, fill=colour, width=int(4 * k))
    d.line((32 * k, 46 * k, 32 * k, 54 * k), fill=colour, width=int(4 * k))
    d.line((24 * k, 55 * k, 40 * k, 55 * k), fill=colour, width=int(4 * k))
    if slash:
        d.line((14 * k, 10 * k, 52 * k, 56 * k), fill=colour, width=int(5 * k))


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


def eye(d, k):
    # The demo's four quadratic curves: M6 32 Q17 15 32 15 ...
    points = []
    for a, b, c in (((6, 32), (17, 15), (32, 15)), ((32, 15), (47, 15), (58, 32)),
                    ((58, 32), (47, 49), (32, 49)), ((32, 49), (17, 49), (6, 32))):
        for i in range(25):
            t = i / 24
            points.append(tuple(((1-t)**2*a[j] + 2*(1-t)*t*b[j] + t*t*c[j]) * k for j in (0, 1)))
    d.line(points, fill=WHITE, width=int(4 * k), joint="curve")
    d.ellipse((24*k, 24*k, 40*k, 40*k), fill=WHITE)


def live_piece(name, w, fill, corner=None):
    """Only the selected corner rounds; the other three stay flush and square."""
    img, d = canvas(w, 28)
    d.rectangle((0, 0, w*S-1, 28*S-1), fill=fill)
    if corner is not None:
        # Cut just one 7.5 px quadrant from a full square, at supersampled size.
        r = int(7.5*S)
        mask = Image.new("L", (2*r, 2*r), 0)
        ImageDraw.Draw(mask).ellipse((0, 0, 2*r-1, 2*r-1), fill=255)
        left, top = corner in (0, 2), corner in (0, 1)
        quadrant = mask.crop((0 if left else r, 0 if top else r, r if left else 2*r, r if top else 2*r))
        alpha = img.getchannel("A")
        if fill[3] != 255:
            quadrant = quadrant.point(lambda a: round(a * fill[3] / 255))
        alpha.paste(quadrant, (0 if left else w*S-r, 0 if top else 28*S-r))
        img.putalpha(alpha)
    # Resize alpha alone: Pillow's RGBA resampling rounds dark RGB by a level and
    # can overshoot translucent alpha. Keep the demo's fill exact, even at the cap.
    alpha = img.getchannel("A").resize((w, 28), Image.LANCZOS).point(lambda a: min(a, fill[3]))
    final = Image.new("RGBA", (w, 28), fill)
    final.putalpha(alpha)
    final.save(os.path.join(OUT, name + ".png"), optimize=True)


def live_art():
    live_piece("live_body", 1, GLASS)
    for corner in range(4):  # TL, TR, BL, BR: only the diagonal inner corner rounds
        live_piece("live_segment_%d" % corner, 31, CORAL, {1: 2, 3: 0}.get(corner))
        live_piece("live_end_%d" % corner, 8, GLASS, {0: 3, 2: 1}.get(corner))
    rounded("live_divider", 1, 12, 0, LINE)
    icon("eye", eye)
    icon("mic_off_red", lambda d, k: mic(d, k, True, MUTED))


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
    elif shape == "triangle":
        d.polygon([(32 * k, 17 * k), (46 * k, 42 * k), (18 * k, 42 * k)], outline=colour, width=int(4 * k))
        d.line([(32 * k, 17 * k), (46 * k, 42 * k), (18 * k, 42 * k), (32 * k, 17 * k)], fill=colour, width=int(5 * k), joint="curve")
    elif shape == "square":
        d.rectangle((21 * k, 21 * k, 43 * k, 43 * k), outline=colour, width=int(5 * k))
    else:
        d.ellipse((19 * k, 19 * k, 45 * k, 45 * k), outline=colour, width=int(5 * k))
    save(img, name, size, size)


def toggle(name, on):
    img, d = canvas(60, 34)
    d.rounded_rectangle((0, 0, 60 * S - 1, 34 * S - 1), 17 * S, fill=CORAL if on else (70, 74, 82, 255))
    x = 43 if on else 17
    d.ellipse(((x - 13) * S, 4 * S, (x + 13) * S, 30 * S), fill=WHITE)
    save(img, name, 60, 34)


def chevron(name, up):
    img, d = canvas(24, 24)
    k = S
    pts = [(5, 15), (12, 8), (19, 15)] if up else [(5, 9), (12, 16), (19, 9)]
    d.line([(x * k, y * k) for x, y in pts], fill=WHITE, width=3 * k, joint="curve")
    save(img, name, 24, 24)


def ring_dot(name, size, colour=WHITE):
    img, d = canvas(size, size)
    d.ellipse((0, 0, size * S - 1, size * S - 1), fill=colour)
    save(img, name, size, size)


def check(d, k):
    d.line([(14 * k, 33 * k), (27 * k, 46 * k), (50 * k, 20 * k)], fill=WHITE, width=int(7 * k), joint="curve")


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


# The on-screen keyboard (discord_app.inc kKb*): 11 columns of 108 px, rows of 70, caps 62 high.
KB_W, KB_H, KB_PAD, KB_COL, KB_ROW, KB_KEY = 1232, 394, 26, 108, 70, 62
KEY = (58, 61, 70, 255)
FKEY = (72, 76, 87, 255)
HINT = (178, 183, 194, 255)
LOWER = ["1234567890@", "qwertyuiop#", "asdfghjkl'/", "zxcvbnm,.?!"]
SYMBOLS = ["1234567890@", "!\"#$%&'()*+", "-/:;<=>[]\\^", "_`{|}~,.?\u20ac\u00a3"]
# Function row: (first column, columns, kind)
KB_FUNCS = [(0, 1.5, "shift"), (1.5, 1.5, "page"), (3, 3, "space"), (6, 1.5, "back"), (7.5, 1, "left"),
            (8.5, 1, "right"), (9.5, 1.5, "send")]


def keyboard(name, rows, shift, page):
    """shift: 0 off, 1 one-shot, 2 caps lock. page: the page key's word."""
    img, d = canvas(KB_W, KB_H)
    d.rounded_rectangle((0, 0, KB_W * S - 1, KB_H * S - 1), 24 * S, fill=(28, 30, 35, 250))
    big = ImageFont.truetype(FONT, 33 * S)
    mid = ImageFont.truetype(FONT, 27 * S)
    tiny = ImageFont.truetype(FONT, 14 * S)

    def box(c0, cols, r):
        x = KB_PAD + KB_COL * c0
        return x, KB_PAD + KB_ROW * r, KB_COL * cols - 8, KB_KEY

    def cap(b, fill):
        x, y, w, h = b
        d.rounded_rectangle((x * S, y * S, (x + w) * S - 1, (y + h) * S - 1), 13 * S, fill=fill)

    for r, row in enumerate(rows):
        for c, ch in enumerate(row):
            b = box(c, 1, r)
            cap(b, KEY)
            if shift and r > 0 and ch.isalpha():
                ch = ch.upper()
            d.text(((b[0] + b[2] / 2) * S, (b[1] + b[3] / 2 + 1) * S), ch, font=big, fill=WHITE, anchor="mm")
    for c0, cols, kind in KB_FUNCS:
        b = box(c0, cols, 4)
        x, y, w, h = b
        cap(b, FKEY)
        cx, cy = x + w / 2, y + h / 2 + 3
        tag = {"shift": "L2", "left": "L1", "right": "R1", "send": "R2"}.get(kind)
        if tag:
            d.text(((x + w - 8) * S, (y + 6) * S), tag, font=tiny, fill=HINT, anchor="ra")
        if kind in ("space", "back"):  # the button hints are the glyphs of the pad
            g, k = (x + w - 22, y + 8), S
            if kind == "space":
                pts = [(g[0] + 7, g[1]), (g[0] + 14, g[1] + 12), (g[0], g[1] + 12), (g[0] + 7, g[1])]
                d.line([(a * k, b_ * k) for a, b_ in pts], fill=(110, 210, 160, 255), width=2 * k, joint="curve")
            else:
                d.rectangle((g[0] * k, g[1] * k, (g[0] + 13) * k, (g[1] + 13) * k), outline=(235, 140, 205, 255), width=2 * k)
        if kind == "shift":
            col = CORAL if shift == 2 else WHITE
            pts = [(cx, cy - 15), (cx + 15, cy + 1), (cx + 7, cy + 1), (cx + 7, cy + 14), (cx - 7, cy + 14),
                   (cx - 7, cy + 1), (cx - 15, cy + 1)]
            pts = [(a * S, b_ * S) for a, b_ in pts]
            if shift:
                d.polygon(pts, fill=col)
            else:
                d.line(pts + [pts[0]], fill=col, width=3 * S, joint="curve")
            if shift == 2:
                d.rounded_rectangle(((cx - 7) * S, (cy + 18) * S, (cx + 7) * S, (cy + 21) * S), 1 * S, fill=col)
        elif kind == "page":
            d.text((cx * S, (cy - 3) * S), page, font=mid, fill=WHITE, anchor="mm")
        elif kind == "space":
            d.line([((cx - 22) * S, (cy - 2) * S), ((cx - 22) * S, (cy + 8) * S), ((cx + 22) * S, (cy + 8) * S),
                    ((cx + 22) * S, (cy - 2) * S)], fill=WHITE, width=4 * S, joint="curve")
        elif kind == "back":
            pts = [(cx - 20, cy), (cx - 8, cy - 12), (cx + 18, cy - 12), (cx + 18, cy + 12), (cx - 8, cy + 12)]
            d.line([(a * S, b_ * S) for a, b_ in pts + [pts[0]]], fill=WHITE, width=3 * S, joint="curve")
            d.line([((cx - 1) * S, (cy - 5) * S), ((cx + 11) * S, (cy + 5) * S)], fill=WHITE, width=3 * S)
            d.line([((cx + 11) * S, (cy - 5) * S), ((cx - 1) * S, (cy + 5) * S)], fill=WHITE, width=3 * S)
        elif kind in ("left", "right"):
            s = 1 if kind == "right" else -1
            d.polygon([((cx + s * 9) * S, cy * S), ((cx - s * 7) * S, (cy - 11) * S), ((cx - s * 7) * S, (cy + 11) * S)],
                      fill=WHITE)
        elif kind == "send":
            d.text((cx * S, (cy - 3) * S), "Send", font=mid, fill=WHITE, anchor="mm")
    save(img, name, KB_W, KB_H)


def keyboard_art():
    keyboard("kb_lower", LOWER, 0, "@#:")
    keyboard("kb_upper", LOWER, 1, "@#:")
    keyboard("kb_caps", LOWER, 2, "@#:")
    keyboard("kb_symbols", SYMBOLS, 0, "ABC")
    for name, cols in (("kb_hl_1", 1), ("kb_hl_15", 1.5), ("kb_hl_3", 3)):  # the highlighted key: coral, the label shows through
        w = int(KB_COL * cols - 8)
        img, d = canvas(w, KB_KEY)
        d.rounded_rectangle((0, 0, w * S - 1, KB_KEY * S - 1), 13 * S, fill=(255, 122, 89, 125), outline=CORAL, width=4 * S)
        save(img, name, w, KB_KEY)
    rounded("kb_field", KB_W, 72, 18, (24, 26, 31, 250), (70, 74, 82, 255), 2)


def kb_glyph(d, k):
    """A small keyboard: a rounded body and three rows of keys, for the chat's input row."""
    d.rounded_rectangle((4 * k, 14 * k, 60 * k, 50 * k), 6 * k, outline=WHITE, width=int(4 * k))
    for row, (n, y) in enumerate(((6, 23), (6, 31), (0, 0))):
        for i in range(n):
            x = 11 + i * 8.4
            d.rounded_rectangle((x * k, y * k, (x + 4.6) * k, (y + 4.6) * k), 1 * k, fill=WHITE)
    d.rounded_rectangle((20 * k, 40 * k, 44 * k, 44 * k), 2 * k, fill=WHITE)


def x_glyph(d, k, cx, cy, r, colour):
    d.line((cx - r, cy - r, cx + r, cy + r), fill=colour, width=int(2.4 * k))
    d.line((cx + r, cy - r, cx - r, cy + r), fill=colour, width=int(2.4 * k))


def chat_art():
    """The Chat and Mentions tabs (discord_app.inc kStripY and friends)."""
    rounded("tabs_bg", 519, 64, 20, (22, 24, 29, 255))
    img, d = canvas(167, 56)
    d.rounded_rectangle((0, 0, 167 * S - 1, 56 * S - 1), 16 * S, fill=CARD)
    d.rounded_rectangle((52 * S, 51 * S, 115 * S, 54 * S), 1.5 * S, fill=CORAL)  # the open tab's underline
    save(img, "tab_on", 167, 56)
    ring_dot("badge_dot", 30, CORAL)
    img, d = canvas(64, 64)
    d.rounded_rectangle((0, 0, 64 * S - 1, 64 * S - 1), 16 * S, fill=(52, 55, 63, 255))
    d.text((32 * S, 33 * S), "#", font=ImageFont.truetype(FONT, 38 * S), fill=(178, 183, 194, 255), anchor="mm")
    save(img, "hash_icon", 64, 64)
    OUTLINE = (255, 255, 255, 14)
    rounded("msg_row", 519, 126, 16, CARD, OUTLINE, 1)
    rounded("msg_row_me", 519, 126, 16, (58, 40, 40, 255), (255, 122, 89, 70), 1)  # a message that mentions you
    rounded("men_row", 519, 172, 16, CARD, OUTLINE, 1)
    rounded("men_focus", 519, 172, 16, None, CORAL, 3)
    img, d = canvas(150, 48)
    d.rounded_rectangle((0, 0, 150 * S - 1, 48 * S - 1), 24 * S, fill=CORAL)
    d.ellipse((12 * S, 12 * S, 36 * S, 36 * S), outline=WHITE, width=2 * S)
    x_glyph(d, S, 24 * S, 24 * S, 4.5 * S, WHITE)
    d.text((46 * S, 25 * S), "Open chat", font=ImageFont.truetype(FONT, 20 * S), fill=WHITE, anchor="lm")
    save(img, "btn_open", 150, 48)
    text_pill("chip_you", 62, 30, CORAL, "@you", 17)
    rounded("chat_field", 519, 72, 18, (24, 26, 31, 250), (70, 74, 82, 255), 2)
    icon("kb_glyph", kb_glyph, 40)


def main():
    os.makedirs(OUT, exist_ok=True)
    # A: the call panel.
    # The panel in three slices: discord_app.inc stretches the middle to fit what it holds.
    img, d = canvas(568, 80)
    d.rounded_rectangle((0, 0, 568 * S - 1, 80 * S - 1), 28 * S, fill=DOCK)
    whole = img.resize((568, 80), Image.LANCZOS)
    whole.crop((0, 0, 568, 40)).save(os.path.join(OUT, "dock_top.png"), optimize=True)
    whole.crop((0, 40, 568, 80)).save(os.path.join(OUT, "dock_bottom.png"), optimize=True)
    rounded("dock_mid", 568, 8, 0, DOCK)
    ring_dot("avatar_none", 106, (60, 64, 72, 255))
    rounded("row", 519, 132, 16, CARD)
    rounded("tile", 122, 140, 16, TILE)
    rounded("tile_live", 122, 140, 16, CORAL)
    rounded("tile_leave", 122, 140, 16, LEAVE)
    rounded("tile_focus", 134, 152, 21, None, WHITE, 3)
    rounded("line", 519, 2, 0, LINE)
    rounded("kb_caret", 4, 40, 2, WHITE)  # the keyboard's caret (tinted coral)
    # The mini settings (tabs, rows, inline dropdown): discord_app.inc kTabW and friends.
    for w in (92, 76, 66, 76, 96):
        rounded("pill_%d" % w, w, 40, 20, WHITE)  # tinted: coral when its tab is open
    text_pill("chip_l1", 38, 26, None, "L1", 12, outline=True)
    text_pill("chip_r1", 38, 26, None, "R1", 12, outline=True)
    text_pill("chip_l1r1", 82, 28, None, "L1 / R1", 12, outline=True)
    rounded("setting_row", 519, 62, 14, CARD)
    rounded("setting_focus", 519, 62, 14, None, CORAL, 2)
    toggle("toggle_on", True)
    toggle("toggle_off", False)
    chevron("chevron_down", False)
    chevron("chevron_up", True)
    rounded("slider_track", 170, 8, 4, (70, 74, 82, 255))
    rounded("slider_fill", 170, 8, 4, CORAL)
    ring_dot("slider_knob", 26)
    for k in range(2, 7):
        rounded("dropdown_%d" % k, 499, 12 + 48 * k, 14, (24, 26, 31, 255))
    rounded("option_on", 483, 44, 10, CORAL)
    icon("check", check, 32)
    ring("ring", 106, 4, CORAL)
    glyph("glyph_cross", 37, (122, 160, 255, 255), "cross")
    glyph("glyph_circle", 37, (255, 92, 110, 255), "circle")
    glyph("glyph_triangle", 37, (110, 210, 160, 255), "triangle")
    glyph("glyph_square", 37, (236, 140, 200, 255), "square")
    keyboard_art()
    chat_art()
    text_pill("chip_options", 78, 28, None, "OPTIONS", 12, outline=True)
    # C: talking-only card, 16 px above/below the 56 px rows, 4 px row gaps.
    for n in range(1, 4):
        rounded("glance_%d" % n, 315, 28 + 60 * n, 18, GLANCE)
    ring("ring_small", 56, 3, CORAL)
    live_art()
    # Retired talking-card chrome; no current UI references these pieces.
    for name in ("glance_0", "glance_line", "you", "pill_live"):
        old = os.path.join(OUT, name + ".png")
        if os.path.exists(old):
            os.remove(old)
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
