// SPDX-License-Identifier: GPL-3.0-only
// H.264 sections 7.3.2.1.1 and E.1.1. Only existing progressive VUI timing is changed;
// no SPS syntax is inserted, and HRD streams are refused rather than changing their model.
#include "h264_timing.h"
#include <string.h>

struct bits { uint8_t *p; size_t count, at; int bad; };
static uint32_t get(struct bits *b, unsigned n)
{
    if (n > 32 || b->at + n > b->count) { b->bad = 1; return 0; }
    uint32_t v = 0;
    while (n--) { v = (v << 1) | ((b->p[b->at / 8] >> (7 - b->at % 8)) & 1); b->at++; }
    return v;
}
static uint32_t ue(struct bits *b)
{
    unsigned zero = 0;
    while (!get(b, 1) && !b->bad) if (++zero >= 31) { b->bad = 1; return 0; }
    return b->bad ? 0 : ((1u << zero) - 1) + get(b, zero);
}
static int32_t se(struct bits *b)
{
    uint32_t v = ue(b);
    return v & 1 ? (int32_t)(v / 2 + 1) : -(int32_t)(v / 2);
}
static void put(struct bits *b, size_t at, uint32_t value, unsigned n)
{
    while (n--) {
        uint8_t mask = (uint8_t)(1u << (7 - at % 8));
        b->p[at / 8] = (uint8_t)((b->p[at / 8] & ~mask) | (((value >> n) & 1u) ? mask : 0));
        at++;
    }
}
static int patch_sps(uint8_t *nal, size_t n, uint8_t out[512], size_t *out_len, unsigned fps)
{
    if (n < 5 || n > 256 || (nal[0] & 0x9f) != 7) return -1;
    uint8_t rbsp[256]; size_t size = 0; unsigned zeros = 0;
    for (size_t i = 1; i < n; i++) {
        uint8_t v = nal[i];
        if (zeros >= 2 && v == 3) {
            if (i + 1 == n || nal[i + 1] > 3) return -1;
            zeros = 0; continue;
        }
        rbsp[size++] = v;
        zeros = v == 0 ? zeros + 1 : 0;
    }
    struct bits b = {rbsp, size * 8, 0, 0};
    unsigned profile = get(&b, 8); get(&b, 16); ue(&b);
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 || profile == 135) {
        unsigned chroma = ue(&b);
        if (chroma > 3) return -1;
        if (chroma == 3) get(&b, 1);
        ue(&b); ue(&b); get(&b, 1);
        if (get(&b, 1)) for (unsigned i = 0; i < (chroma == 3 ? 12u : 8u); i++) {
            if (get(&b, 1)) {
                int last = 8, next = 8;
                for (unsigned j = 0; j < (i < 6 ? 16u : 64u); j++) {
                    if (next) next = (int)(((int64_t)last + se(&b)) % 256 + 256) % 256;
                    if (next) last = next;
                }
            }
        }
    } else if (profile != 66 && profile != 77 && profile != 88) return -1;
    ue(&b);
    unsigned poc = ue(&b);
    if (!poc) ue(&b);
    else if (poc == 1) {
        get(&b, 1); se(&b); se(&b);
        unsigned cycle = ue(&b);
        if (cycle > 255) return -1;
        for (unsigned i = 0; i < cycle; i++) se(&b);
    } else if (poc != 2) return -1;
    ue(&b); get(&b, 1); ue(&b); ue(&b);
    if (!get(&b, 1)) return -1; // progressive only
    get(&b, 1);
    if (get(&b, 1)) for (unsigned i = 0; i < 4; i++) ue(&b);
    if (!get(&b, 1)) return -1; // VUI present
    if (get(&b, 1) && get(&b, 8) == 255) get(&b, 32); // aspect ratio
    if (get(&b, 1)) get(&b, 1); // overscan
    if (get(&b, 1)) {
        get(&b, 3); get(&b, 1);
        if (get(&b, 1)) get(&b, 24); // colour description
    }
    if (get(&b, 1)) { ue(&b); ue(&b); } // chroma location
    if (!get(&b, 1)) return -1; // existing timing only
    size_t timing = b.at;
    uint32_t tick = get(&b, 32), scale = get(&b, 32);
    get(&b, 1);
    if (!tick || !scale || get(&b, 1) || get(&b, 1) || b.bad) return -1; // no NAL/VCL HRD
    put(&b, timing, 1, 32);
    put(&b, timing + 32, fps * 2, 32);
    put(&b, timing + 64, 1, 1);
    size_t written = 1; zeros = 0; out[0] = nal[0];
    for (size_t i = 0; i < size; i++) {
        uint8_t v = rbsp[i];
        if (zeros >= 2 && v <= 3) { out[written++] = 3; zeros = 0; }
        out[written++] = v;
        zeros = v == 0 ? zeros + 1 : 0;
    }
    *out_len = written;
    return 0;
}
static size_t prefix(const uint8_t *p, size_t n, size_t at)
{
    for (; at + 3 <= n; at++)
        if (!p[at] && !p[at + 1] && p[at + 2] == 1) return at;
    return n;
}
int capture_h264_timing(uint8_t *data, size_t *len, size_t capacity, unsigned fps)
{
    if ((fps != 30 && fps != 60) || *len > capacity) return -1;
    size_t at = prefix(data, *len, 0);
    while (at < *len) {
        size_t nal = at + 3, end = prefix(data, *len, nal);
        // A four-byte start code's leading zero belongs to the next NAL, not this SPS.
        size_t tail = end;
        while (tail > nal && !data[tail - 1]) tail--;
        if (nal >= *len) return -1;
        if ((data[nal] & 31) == 7) {
            uint8_t patched[512]; size_t n = 0, old = tail - nal;
            if (patch_sps(data + nal, old, patched, &n, fps) || n > capacity - (*len - old)) return -1;
            memmove(data + nal + n, data + tail, *len - tail);
            memcpy(data + nal, patched, n);
            *len = *len - old + n;
            end = end - old + n;
        }
        at = end;
    }
    return 0;
}
