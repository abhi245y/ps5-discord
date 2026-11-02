// SPDX-License-Identifier: AGPL-3.0-or-later
// Round avatars for the in-game overlay: a Discord avatar PNG in, a size x size RGBA PNG
// out, cut to a soft-edged circle (PUI pictures have no corner radius of their own).
#pragma once

#include <stddef.h>
#include <stdio.h>

// 0 on success; -1 for a PNG it cannot read (16-bit, interlaced, over 1024 px) or a write
// error. Reads 8-bit RGB, RGBA, grey and palette PNGs.
int png_circle(const unsigned char *png, size_t n, int size, FILE *out);
// RGBA pixels of an 8-bit, non-interlaced PNG (RGB, RGBA, grey, grey+alpha or palette), at
// most 1024 px a side. The caller frees *rgba. 0 on success.
int png_decode(const unsigned char *png, size_t n, unsigned char **rgba, int *w, int *h);
