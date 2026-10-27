#!/usr/bin/env python3
# ps5-discord - PNG to the 3840x2160 BC7 DDS the console wants for pic0/pic1,
# without texconv (Windows only). Needs Pillow and numpy.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
#   tools/png2dds.py in.png out.dds
#
# ponytail: BC7 mode 6 only, endpoints from each block's colour bounding box.
# Fine for soft artwork; a real encoder (texconv, bc7enc) if sharp detail bands.
import struct, sys
import numpy as np
from PIL import Image

W, H = 3840, 2160
WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64])


def blocks(img):
    a = np.asarray(img.convert("RGBA").resize((W, H), Image.LANCZOS), dtype=np.int32)
    return a.reshape(H // 4, 4, W // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)


def endpoints(px):
    lo, hi = px.min(1), px.max(1)  # (n, 4)
    # 7-bit colour plus a shared p-bit per endpoint: pick the p-bit that rounds best.
    def quant(v):
        p = np.round((v & 1).mean(1)).astype(np.int32)[:, None]
        c = np.clip(np.round((v - p) / 2), 0, 127).astype(np.int32)
        return c, p[:, 0]
    return quant(lo), quant(hi)


def encode(px):
    (c0, p0), (c1, p1) = endpoints(px)
    e0 = (c0 << 1) | p0[:, None]
    e1 = (c1 << 1) | p1[:, None]
    palette = ((64 - WEIGHTS)[None, :, None] * e0[:, None, :] + WEIGHTS[None, :, None] * e1[:, None, :] + 32) >> 6
    err = ((px[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(-1)  # (n, 16 pixels, 16 entries)
    idx = err.argmin(-1)
    # Index 0's top bit is implied 0: swap the endpoints where it is set.
    flip = idx[:, 0] >= 8
    c0[flip], c1[flip] = c1[flip].copy(), c0[flip].copy()
    p0[flip], p1[flip] = p1[flip].copy(), p0[flip].copy()
    idx[flip] = 15 - idx[flip]
    out = bytearray()
    for b in range(len(px)):
        v, pos = 1 << 6, 7  # mode 6
        for ch in range(4):
            v |= int(c0[b, ch]) << pos
            v |= int(c1[b, ch]) << (pos + 7)
            pos += 14
        v |= int(p0[b]) << pos
        v |= int(p1[b]) << (pos + 1)
        pos += 2
        for i in range(16):
            n = 3 if i == 0 else 4
            v |= int(idx[b, i]) << pos
            pos += n
        out += v.to_bytes(16, "little")
    return bytes(out)


def decode(data):
    """Mode 6 blocks back to pixels, for the self-check."""
    n = len(data) // 16
    px = np.zeros((n, 16, 4), np.int32)
    for b in range(n):
        v = int.from_bytes(data[b * 16:b * 16 + 16], "little")
        assert v & 0x7F == 1 << 6
        pos, c = 7, np.zeros((2, 4), np.int32)
        for ch in range(4):
            c[0, ch] = (v >> pos) & 127
            c[1, ch] = (v >> (pos + 7)) & 127
            pos += 14
        e = (c << 1) | np.array([[(v >> pos) & 1], [(v >> (pos + 1)) & 1]])
        pos += 2
        for i in range(16):
            nb = 3 if i == 0 else 4
            w = WEIGHTS[(v >> pos) & ((1 << nb) - 1)]
            pos += nb
            px[b, i] = ((64 - w) * e[0] + w * e[1] + 32) >> 6
    return px


def header():
    h = struct.pack("<4sIIIIIII", b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, H, W, W // 4 * 16, 0, 1)
    h += b"\0" * 44
    h += struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    h += struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)
    h += struct.pack("<IIIII", 98, 3, 0, 1, 0)  # BC7_UNORM, 2D, array of 1
    assert len(h) == 148
    return h


def self_check():
    rng = np.random.default_rng(1)
    base = rng.integers(0, 200, (1, 1, 4))  # stays below 255: a straight line, as one block should be
    grad = np.clip(base + np.arange(16).reshape(1, 16, 1) * 3, 0, 255).astype(np.int32)
    back = decode(encode(grad))
    assert np.abs(back - grad).max() <= 4, np.abs(back - grad).max()


if __name__ == "__main__":
    self_check()
    if len(sys.argv) != 3:
        sys.exit("usage: tools/png2dds.py in.png out.dds")
    px = blocks(Image.open(sys.argv[1]))
    data = b"".join(encode(px[i:i + 20000]) for i in range(0, len(px), 20000))  # bounded memory
    sample = px[:: len(px) // 2000][:2000]
    mse = float(((decode(encode(sample)) - sample) ** 2).mean())
    print(f"{sys.argv[2]}: PSNR {10 * np.log10(255 ** 2 / max(mse, 1e-9)):.1f} dB on a sample")
    open(sys.argv[2], "wb").write(header() + data)
