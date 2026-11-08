// SPDX-License-Identifier: AGPL-3.0-or-later
// Go Live's wireless backend (broadcast.c picks it): the picture from the capture helper
// (capture/, discord-capture.elf; protocol in capture_proto.h), the sound from the core's own
// game-audio capture (game_audio.h) encoded here. No Remote Play pairing, no USB controller.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int width, height, fps, bitrate;  // what to ask the encoder for
    int (*keep_going)(void);          // 0: end Go Live (checked every 100 ms)
    void (*video)(const uint8_t *annexb, size_t n, int keyframe); // the sink's, copy and return
    void (*audio)(const uint8_t *opus, size_t n);
    void (*live)(void);               // the first picture arrived
} capture_params;

// Runs one Go Live: launches the helper, serves the socket, ends when keep_going says so or the
// helper goes. NULL when it ended as asked, else why (a user-facing sentence).
const char *capture_run(const capture_params *p);
// A viewer lost the picture; the encoder emits IDRs only when asked. Any thread, never blocks.
void capture_request_keyframe(void);
// The profile the encoder reported; 0 until it did (then the requested one stands).
int capture_actual_profile(int *w, int *h, int *fps);

// Seams for the tests (and the console's defaults).
typedef int (*capture_launcher)(void); // starts the helper (after the connection file exists); 0 on success
void capture_set_launcher(capture_launcher launcher);
void capture_set_conn_file(const char *path);
void capture_set_timeouts(int connect_ms, int stop_ms);
