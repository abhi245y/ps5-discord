// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef DS5_USB_H
#define DS5_USB_H
#include <stddef.h>
#include <stdint.h>

#define DS5_USB_API_VERSION 1u

#define DS5_USB_RATE 48000u
#define DS5_USB_REPORT_BYTES 64u
#define DS5_USB_MAX_MIC_FRAMES 784u
#define DS5_USB_OUTPUT_FRAMES 768u

enum ds5_usb_button {
    DS5_CROSS = 1u << 0, DS5_CIRCLE = 1u << 1, DS5_SQUARE = 1u << 2, DS5_TRIANGLE = 1u << 3,
    DS5_UP = 1u << 4, DS5_RIGHT = 1u << 5, DS5_DOWN = 1u << 6, DS5_LEFT = 1u << 7,
    DS5_L1 = 1u << 8, DS5_R1 = 1u << 9, DS5_L3 = 1u << 10, DS5_R3 = 1u << 11,
    DS5_CREATE = 1u << 12, DS5_OPTIONS = 1u << 13, DS5_PS = 1u << 14,
    DS5_TOUCHPAD = 1u << 15, DS5_MUTE = 1u << 16, DS5_L2 = 1u << 17, DS5_R2 = 1u << 18
};

typedef struct {
    uint32_t buttons;
    int16_t left_x, left_y, right_x, right_y;
    uint8_t left_trigger, right_trigger;
} ds5_usb_controller;

typedef struct {
    uint32_t duration_ms; // Default 60 s. 0 runs until service/stop requests it.
    uint32_t headphone_gain_q15, speaker_gain_q15; // 32768 is unity; max 65536.
    uint8_t mic_volume, headphone_volume, speaker_volume, speaker_pre_gain;
    uint8_t notifications, stop_on_ps;
} ds5_usb_config;

typedef struct {
    void (*log)(void *user, const char *message);
    // All callbacks execute on the USB owner thread and must not block.
    // Buffers are borrowed for the duration of the callback only.
    void (*controller)(void *user, const ds5_usb_controller *state, const uint8_t report[64]);
    void (*microphone)(void *user, const int16_t *mono, size_t frames, int muted);
    // Fill up to frames stereo S16 frames; return frames supplied. Shortfall is silence.
    size_t (*playback)(void *user, int16_t *stereo, size_t frames);
    void (*route)(void *user, int headphones, int mic_muted);
    // Nonblocking IPC/control work: 0 continue, 1 graceful stop, -1 error.
    int (*service)(void *user);
} ds5_usb_callbacks;

typedef struct {
    uint64_t microphone_frames, playback_frames, controller_reports;
    uint64_t route_changes, playback_silence_frames;
    uint64_t input_full_scale_frames;
    unsigned microphone_peak, playback_peak;
} ds5_usb_stats;

enum { DS5_USB_OK = 0, DS5_USB_ERROR = -1, DS5_USB_BUSY = -2, DS5_USB_BAD_CONFIG = -3 };

void ds5_usb_default_config(ds5_usb_config *config);
// One owner per process/device. Blocks until stop, duration, disconnect or error.
// Run on a worker thread. Has no fork/watchdog; standalone wrapper supplies one.
int ds5_usb_run(const ds5_usb_config *config, const ds5_usb_callbacks *callbacks,
                void *user, ds5_usb_stats *stats);
void ds5_usb_request_stop(void);
#endif
