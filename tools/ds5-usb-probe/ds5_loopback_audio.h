// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef DS5_LOOPBACK_AUDIO_H
#define DS5_LOOPBACK_AUDIO_H
#include "ds5_mic_format.h"

#define DS5_LOOP_CAPACITY 8192u
#define DS5_LOOP_PREFILL 3072u
#define DS5_LOOP_LIMIT 32768
#define DS5_LOOP_HEADPHONE_LIMIT 16384

struct ds5_loop_ring {
    int16_t samples[DS5_LOOP_CAPACITY];
    unsigned read, count;
    uint64_t overruns, underruns;
};

static inline int ds5_signed_sample(const uint8_t *p)
{
    int value = ds5_le16(p);
    return value >= 32768 ? value - 65536 : value;
}

static inline void ds5_loop_reset(struct ds5_loop_ring *ring)
{
    ring->read = 0;
    ring->count = DS5_LOOP_PREFILL;
    memset(ring->samples, 0, DS5_LOOP_PREFILL * sizeof ring->samples[0]);
}

static inline void ds5_loop_push(struct ds5_loop_ring *ring, int value)
{
    if (ring->count == DS5_LOOP_CAPACITY) {
        ring->read = (ring->read + 1) % DS5_LOOP_CAPACITY;
        ring->count--;
        ring->overruns++;
    }
    ring->samples[(ring->read + ring->count) % DS5_LOOP_CAPACITY] = (int16_t)value;
    ring->count++;
}

static inline int ds5_loop_pop(struct ds5_loop_ring *ring)
{
    if (!ring->count) {
        ring->underruns++;
        return 0;
    }
    int value = ring->samples[ring->read];
    ring->read = (ring->read + 1) % DS5_LOOP_CAPACITY;
    ring->count--;
    return value;
}

static inline void ds5_loop_packet(struct ds5_loop_ring *ring, uint8_t packet[384],
                                  int headphones, int muted)
{
    memset(packet, 0, 384);
    for (unsigned s = 0; s < 48; s++) {
        int value = ds5_loop_pop(ring);
        value = muted ? 0 : value / (headphones ? 2 : 1);
        int limit = headphones ? DS5_LOOP_HEADPHONE_LIMIT : DS5_LOOP_LIMIT;
        if (value > limit)
            value = limit;
        if (value < -limit)
            value = -limit;
        if (headphones)
            ds5_put_le(packet + s * 8, (uint16_t)value, 2);
        ds5_put_le(packet + s * 8 + 2, (uint16_t)value, 2);
    }
}

// USB report 0x01: common payload starts at byte 1; status[1] is byte 54.
// These masks/offsets follow Sony's Linux hid-playstation input report layout.
static inline int ds5_loop_status(const uint8_t *report, size_t bytes,
                                 int *headphones, int *muted, uint8_t *buttons)
{
    if (bytes != 64 || report[0] != 1)
        return 0;
    *headphones = !!(report[54] & 1);
    *muted = !!(report[54] & 4);
    *buttons = report[10];
    return 1;
}

static inline void ds5_loop_route(uint8_t report[63], int headphones, int muted)
{
    memset(report, 0, 63);
    report[0] = 2;
    report[1] = 0xf0; // Headphone/speaker/mic volume and audio routing.
    report[2] = 0x83; // Speaker pre-gain, mic mute/power and mute LED.
    report[5] = headphones ? 0x7f : 0;
    report[6] = headphones ? 0 : 0x64;
    report[7] = 0x20; // Avoid driving the built-in mic at maximum gain.
    report[8] = (headphones ? 0 : 0x30) | 1; // Force built-in mic on both routes.
    report[9] = muted ? 1 : 0;
    report[10] = muted ? 0x10 : 0;
    report[38] = 2;
}
#endif
