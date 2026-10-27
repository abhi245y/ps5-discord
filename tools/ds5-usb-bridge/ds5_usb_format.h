// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef DS5_USB_FORMAT_H
#define DS5_USB_FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct ds5_mic_format {
    uint8_t interface_index, interface_number, alt_index, alt_setting;
    uint8_t endpoint, control_index;
    uint16_t packet_bytes;
};

static inline uint16_t ds5_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

// Only accept UAC1 Type I PCM, 48 kHz, signed 16-bit, with the requested channels.
// FreeBSD ioctls take descriptor *indices*, not bInterfaceNumber/bAlternateSetting.
// Return -1 for malformed descriptors, 0 for no compatible mic, 1 for a match.
static inline int ds5_audio_find(const uint8_t *data, size_t size, struct ds5_mic_format *out,
                                unsigned channels, int input)
{
    if (size < 9 || data[0] != 9 || data[1] != 2 || ds5_le16(data + 2) != size)
        return -1;
    struct ds5_mic_format candidate = {0}, selected = {0};
    int index = -1, alt = -1, previous = -1;
    int streaming = 0, pcm = 0, format = 0, found = 0;
    for (size_t offset = 9; offset < size;) {
        const uint8_t *d = data + offset;
        size_t n = d[0];
        if (size - offset < 2 || n < 2 || n > size - offset)
            return -1;
        if (d[1] == 4) {
            if (n < 9)
                return -1;
            if (d[2] != previous) {
                index++;
                alt = 0;
                previous = d[2];
            } else {
                alt++;
            }
            if (index > 254 || alt > 254)
                return -1;
            candidate = (struct ds5_mic_format){.interface_index = (uint8_t)index,
                .interface_number = d[2], .alt_index = (uint8_t)alt,
                .alt_setting = d[3], .control_index = 255};
            streaming = d[3] != 0 && d[5] == 1 && d[6] == 2 && d[7] == 0;
            pcm = format = 0;
        } else if (streaming && d[1] == 0x24) {
            if (n < 3)
                return -1;
            if (d[2] == 1) {
                if (n < 7)
                    return -1;
                pcm = ds5_le16(d + 5) == 1;
            } else if (d[2] == 2) {
                if (n < 8)
                    return -1;
                unsigned frequencies = d[7];
                size_t needed = frequencies ? 8u + 3u * frequencies : 14u;
                if (n < needed)
                    return -1;
                int rate = 0;
                for (unsigned f = 0; f < frequencies; f++) {
                    const uint8_t *p = d + 8 + 3 * f;
                    if ((p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)) == 48000)
                        rate = 1;
                }
                // DualSense advertises one discrete frequency. Do not program a
                // continuous-rate endpoint without its sampling-frequency control.
                format = d[3] == 1 && d[4] == channels && d[5] == 2 && d[6] == 16 &&
                         frequencies == 1 && rate;
            }
        } else if (streaming && d[1] == 5) {
            if (n < 7)
                return -1;
            uint16_t packet = ds5_le16(d + 4);
            if (!found && pcm && format && !!(d[2] & 0x80) == !!input && (d[2] & 0x0f) &&
                (d[3] & 0x33) == 1 && packet >= 48u * channels * 2u && packet <= 1024) {
                candidate.endpoint = d[2];
                candidate.packet_bytes = packet;
                selected = candidate;
                found = 1;
            }
        }
        offset += n;
    }
    if (!found)
        return 0;

    // Locate the AudioControl collection owning this streaming interface.
    // Some USB audio drivers attach to that parent instead of the mic interface.
    index = -1;
    previous = -1;
    int control = 0;
    for (size_t offset = 9; offset < size; offset += data[offset]) {
        const uint8_t *d = data + offset;
        size_t n = d[0];
        if (d[1] == 4) {
            if (d[2] != previous) {
                index++;
                previous = d[2];
            }
            control = d[5] == 1 && d[6] == 1 && d[7] == 0;
        } else if (control && d[1] == 0x24 && n >= 3 && d[2] == 1) {
            if (n < 8 || n < 8u + d[7])
                return -1;
            for (unsigned i = 0; i < d[7]; i++)
                if (d[8 + i] == selected.interface_number)
                    selected.control_index = (uint8_t)index;
        }
    }
    *out = selected;
    return 1;
}

static inline int ds5_mic_find(const uint8_t *data, size_t size, struct ds5_mic_format *out)
{
    return ds5_audio_find(data, size, out, 2, 1);
}

static inline void ds5_put_le(uint8_t *p, uint32_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++)
        p[i] = (uint8_t)(value >> (8 * i));
}

static inline void ds5_wav_header(uint8_t header[44], uint32_t bytes)
{
    memset(header, 0, 44);
    memcpy(header, "RIFF", 4);
    ds5_put_le(header + 4, bytes + 36, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    ds5_put_le(header + 16, 16, 4);
    ds5_put_le(header + 20, 1, 2);
    ds5_put_le(header + 22, 2, 2);
    ds5_put_le(header + 24, 48000, 4);
    ds5_put_le(header + 28, 192000, 4);
    ds5_put_le(header + 32, 4, 2);
    ds5_put_le(header + 34, 16, 2);
    memcpy(header + 36, "data", 4);
    ds5_put_le(header + 40, bytes, 4);
}
#endif
