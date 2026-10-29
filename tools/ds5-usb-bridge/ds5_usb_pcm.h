// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef DS5_USB_PCM_H
#define DS5_USB_PCM_H
#include "ds5_usb.h"
#include "ds5_usb_format.h"

static inline int ds5_usb_sample(const uint8_t *p)
{
    int value = ds5_le16(p);
    return value >= 32768 ? value - 65536 : value;
}

static inline int ds5_usb_gain(int value, uint32_t gain_q15)
{
    int64_t scaled = (int64_t)value * gain_q15 / 32768;
    return scaled > 32767 ? 32767 : scaled < -32768 ? -32768 : (int)scaled;
}

static inline void ds5_usb_output_packet(uint8_t packet[384], const int16_t stereo[96],
                                        int headphones, uint32_t gain_q15)
{
    memset(packet, 0, 384);
    for (unsigned s = 0; s < 48; s++) {
        int left = stereo[2 * s], right = stereo[2 * s + 1];
        if (headphones) {
            ds5_put_le(packet + s * 8, (uint16_t)ds5_usb_gain(left, gain_q15), 2);
            ds5_put_le(packet + s * 8 + 2, (uint16_t)ds5_usb_gain(right, gain_q15), 2);
        } else {
            int mono = (left + right) / 2;
            ds5_put_le(packet + s * 8 + 2, (uint16_t)ds5_usb_gain(mono, gain_q15), 2);
        }
    }
}

static inline int ds5_usb_parse_controller(const uint8_t *report, size_t size,
                                          ds5_usb_controller *out)
{
    if (size != 64 || report[0] != 1)
        return 0;
    static const uint32_t hats[8] = {DS5_UP, DS5_UP | DS5_RIGHT, DS5_RIGHT, DS5_RIGHT | DS5_DOWN,
        DS5_DOWN, DS5_DOWN | DS5_LEFT, DS5_LEFT, DS5_LEFT | DS5_UP};
    *out = (ds5_usb_controller){.left_x = (int16_t)((report[1] - 128) * 256),
        .left_y = (int16_t)((report[2] - 128) * 256), .right_x = (int16_t)((report[3] - 128) * 256),
        .right_y = (int16_t)((report[4] - 128) * 256), .left_trigger = report[5], .right_trigger = report[6]};
    if ((report[8] & 15) < 8)
        out->buttons = hats[report[8] & 15];
    static const struct { uint8_t byte, mask; uint32_t button; } map[] = {
        {8, 0x10, DS5_SQUARE}, {8, 0x20, DS5_CROSS}, {8, 0x40, DS5_CIRCLE}, {8, 0x80, DS5_TRIANGLE},
        {9, 1, DS5_L1}, {9, 2, DS5_R1}, {9, 4, DS5_L2}, {9, 8, DS5_R2},
        {9, 0x10, DS5_CREATE}, {9, 0x20, DS5_OPTIONS}, {9, 0x40, DS5_L3}, {9, 0x80, DS5_R3},
        {10, 1, DS5_PS}, {10, 2, DS5_TOUCHPAD}, {10, 4, DS5_MUTE}
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (report[map[i].byte] & map[i].mask)
            out->buttons |= map[i].button;
    return 1;
}

static inline void ds5_usb_route_report(uint8_t report[63], const ds5_usb_config *config,
                                       int headphones, int muted)
{
    memset(report, 0, 63);
    report[0] = 2;
    report[1] = 0xf0;
    report[2] = 0x83;
    report[5] = headphones ? config->headphone_volume : 0;
    report[6] = headphones ? 0 : config->speaker_volume;
    report[7] = config->mic_volume;
    report[8] = (headphones ? 0 : 0x30) | 1;
    report[9] = muted ? 1 : 0;
    report[10] = muted ? 0x10 : 0;
    report[38] = config->speaker_pre_gain;
    if (!config->audio_only) {
        // The system's driver is detached and its lights went with it: player 1, blue,
        // as the console shows it (report layout: Linux hid-playstation).
        report[2] |= 0x04 | 0x10; // lightbar, player LEDs
        report[39] = 0x02;        // lightbar setup
        report[42] = 0x02;        // light out: end the firmware's own pulse
        report[44] = 0x04;        // the middle LED
        report[47] = 0xff;        // blue
    }
}
#endif
