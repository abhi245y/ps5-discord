// SPDX-License-Identifier: AGPL-3.0-or-later
#include "png_circle.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int paeth(int a, int b, int c)
{
    const int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

int png_decode(const unsigned char *png, size_t n, unsigned char **rgba, int *w, int *h)
{
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (n < 33 || memcmp(png, sig, 8) != 0)
        return -1;
    int width = 0, height = 0, type = -1, channels = 0;
    unsigned char palette[256 * 4];
    memset(palette, 255, sizeof palette);
    unsigned char *idat = NULL;
    size_t idat_n = 0;
    for (size_t at = 8; at + 12 <= n;) {
        const uint32_t len = be32(png + at);
        const unsigned char *kind = png + at + 4, *data = png + at + 8;
        if (len > n - at - 12)
            break;
        if (!memcmp(kind, "IHDR", 4) && len >= 13) {
            width = (int)be32(data);
            height = (int)be32(data + 4);
            type = data[9];
            if (data[8] != 8 || data[12] != 0 || width <= 0 || height <= 0 || width > 1024 || height > 1024)
                break; // only 8-bit, not interlaced, small
        } else if (!memcmp(kind, "PLTE", 4)) {
            for (uint32_t i = 0; i < len / 3 && i < 256; i++)
                memcpy(palette + i * 4, data + i * 3, 3);
        } else if (!memcmp(kind, "tRNS", 4) && type == 3) {
            for (uint32_t i = 0; i < len && i < 256; i++)
                palette[i * 4 + 3] = data[i];
        } else if (!memcmp(kind, "IDAT", 4)) {
            unsigned char *grown = realloc(idat, idat_n + len);
            if (!grown)
                break;
            idat = grown;
            memcpy(idat + idat_n, data, len);
            idat_n += len;
        } else if (!memcmp(kind, "IEND", 4))
            break;
        at += 12 + len;
    }
    channels = type == 6 ? 4 : type == 2 ? 3 : type == 4 ? 2 : type == 0 || type == 3 ? 1 : 0;
    if (!idat || !channels) {
        free(idat);
        return -1;
    }
    const size_t stride = (size_t)width * channels, raw_n = (stride + 1) * (size_t)height;
    unsigned char *raw = malloc(raw_n), *out = malloc((size_t)width * height * 4);
    z_stream z = {0};
    int ok = raw && out && inflateInit(&z) == Z_OK;
    if (ok) {
        z.next_in = idat;
        z.avail_in = (uInt)idat_n;
        z.next_out = raw;
        z.avail_out = (uInt)raw_n;
        const int r = inflate(&z, Z_FINISH);
        ok = (r == Z_STREAM_END || r == Z_OK) && z.avail_out == 0;
        inflateEnd(&z);
    }
    free(idat);
    for (int y = 0; ok && y < height; y++) { // undo the row filters in place
        unsigned char *row = raw + y * (stride + 1) + 1, *up = y ? row - stride - 1 : NULL;
        const int f = row[-1];
        for (size_t x = 0; x < stride; x++) {
            const int a = x >= (size_t)channels ? row[x - channels] : 0, b = up ? up[x] : 0,
                      c = up && x >= (size_t)channels ? up[x - channels] : 0;
            row[x] = (unsigned char)(row[x] + (f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : f == 4 ? paeth(a, b, c) : 0));
        }
        ok = f <= 4;
    }
    for (int i = 0; ok && i < width * height; i++) {
        const unsigned char *s = raw + (i / width) * (stride + 1) + 1 + (size_t)(i % width) * channels;
        unsigned char *d = out + (size_t)i * 4;
        if (type == 3)
            memcpy(d, palette + s[0] * 4, 4);
        else if (channels <= 2) {
            d[0] = d[1] = d[2] = s[0];
            d[3] = channels == 2 ? s[1] : 255;
        } else {
            memcpy(d, s, 3);
            d[3] = channels == 4 ? s[3] : 255;
        }
    }
    free(raw);
    if (!ok) {
        free(out);
        return -1;
    }
    *rgba = out;
    *w = width;
    *h = height;
    return 0;
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static int chunk(FILE *f, const char *kind, const unsigned char *data, uint32_t len)
{
    unsigned char head[8];
    put32(head, len);
    memcpy(head + 4, kind, 4);
    uLong crc = crc32(0, head + 4, 4);
    crc = crc32(crc, data, len);
    unsigned char tail[4];
    put32(tail, (uint32_t)crc);
    return fwrite(head, 1, 8, f) == 8 && (!len || fwrite(data, 1, len, f) == len) && fwrite(tail, 1, 4, f) == 4 ? 0 : -1;
}

int png_circle(const unsigned char *png, size_t n, int size, FILE *out)
{
    unsigned char *src = NULL;
    int w = 0, h = 0;
    if (size < 8 || size > 256 || png_decode(png, n, &src, &w, &h) != 0)
        return -1;
    // Scaled (box average over the source pixels each one covers), then the circle's edge.
    const size_t row = (size_t)size * 4 + 1;
    unsigned char *raw = calloc(row * (size_t)size, 1);
    if (!raw) {
        free(src);
        return -1;
    }
    const float r = size / 2.0f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            const int x0 = x * w / size, x1 = (x + 1) * w / size > x0 ? (x + 1) * w / size : x0 + 1;
            const int y0 = y * h / size, y1 = (y + 1) * h / size > y0 ? (y + 1) * h / size : y0 + 1;
            unsigned sum[4] = {0, 0, 0, 0}, count = 0;
            for (int sy = y0; sy < y1 && sy < h; sy++)
                for (int sx = x0; sx < x1 && sx < w; sx++, count++)
                    for (int c = 0; c < 4; c++)
                        sum[c] += src[((size_t)sy * w + sx) * 4 + c];
            unsigned char *d = raw + (size_t)y * row + 1 + (size_t)x * 4;
            for (int c = 0; c < 4; c++)
                d[c] = (unsigned char)(count ? sum[c] / count : 0);
            const float dx = x + 0.5f - r, dy = y + 0.5f - r, edge = r - sqrtf(dx * dx + dy * dy) + 0.5f;
            const float cover = edge < 0 ? 0 : edge > 1 ? 1 : edge;
            d[3] = (unsigned char)(d[3] * cover + 0.5f);
        }
    free(src);
    // Stored (uncompressed) deflate blocks: no compressor needed, and avatars are small.
    const size_t raw_n = row * (size_t)size, blocks = (raw_n + 65534) / 65535;
    const size_t z_n = 2 + raw_n + blocks * 5 + 4;
    unsigned char *z = malloc(z_n);
    int ok = z != NULL;
    if (ok) {
        size_t at = 0;
        z[at++] = 0x78;
        z[at++] = 0x01;
        for (size_t done = 0; done < raw_n;) {
            const size_t len = raw_n - done > 65535 ? 65535 : raw_n - done;
            z[at++] = done + len == raw_n ? 1 : 0;
            z[at++] = (unsigned char)len;
            z[at++] = (unsigned char)(len >> 8);
            z[at++] = (unsigned char)~len;
            z[at++] = (unsigned char)(~len >> 8);
            memcpy(z + at, raw + done, len);
            at += len;
            done += len;
        }
        put32(z + at, (uint32_t)adler32(adler32(0, NULL, 0), raw, (uInt)raw_n));
        static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        unsigned char ihdr[13] = {0};
        put32(ihdr, (uint32_t)size);
        put32(ihdr + 4, (uint32_t)size);
        ihdr[8] = 8;
        ihdr[9] = 6; // RGBA
        ok = fwrite(sig, 1, 8, out) == 8 && chunk(out, "IHDR", ihdr, 13) == 0 &&
             chunk(out, "IDAT", z, (uint32_t)z_n) == 0 && chunk(out, "IEND", NULL, 0) == 0;
    }
    free(z);
    free(raw);
    return ok ? 0 : -1;
}
