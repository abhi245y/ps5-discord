// SPDX-License-Identifier: AGPL-3.0-or-later
// Broadcast (Go Live): the game's picture and sound come from a loopback Remote Play
// session with the console itself; the DualSense on USB is taken over for the length
// of it (its buttons go to the game through the session, its mic and speaker serve the
// call). The frames go to a sink, the Discord stream connection.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    // Remote Play's threads; copy and return. video: one Annex-B access unit.
    void (*video)(const uint8_t *annexb, size_t n, int keyframe);
    void (*audio)(const uint8_t *opus, size_t n); // 48 kHz stereo Opus, 10 ms per frame
} bc_sink;

void bc_set_sink(const bc_sink *sink); // NULL: frames are only counted
int bc_start(void); // needs a call and a pairing; 0 when accepted, -1 with bc_error()
void bc_stop(void);
// 1 from taking the USB DualSense until it is re-enumerated for the system again.
int bc_holds_controller(void);
void bc_request_keyframe(void); // a viewer lost the picture
void bc_profile(int *w, int *h, int *fps); // the picture's size and rate (prefs golive_quality)
const char *bc_state(void); // "off", "starting", "live" or "error"
const char *bc_error(void); // why it stopped or could not start; "" when none

