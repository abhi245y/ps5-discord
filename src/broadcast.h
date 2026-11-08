// SPDX-License-Identifier: AGPL-3.0-or-later
// Broadcast (Go Live), two backends (prefs golive_backend): the wireless capture (the console's
// encoder through a helper payload, the sound from the core's own game-audio capture; the
// controller stays untouched) or a loopback Remote Play session with the console itself, which
// takes the DualSense on USB over for the length of it (its buttons go to the game through the
// session, its mic and speaker serve the call). The frames go to a sink, the Discord stream
// connection.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    // Remote Play's threads; copy and return. video: one Annex-B access unit.
    void (*video)(const uint8_t *annexb, size_t n, int keyframe);
    void (*audio)(const uint8_t *opus, size_t n); // 48 kHz stereo Opus, 10 ms per frame
} bc_sink;

void bc_set_sink(const bc_sink *sink); // NULL: frames are only counted
int bc_start(void); // needs a call (and, for Remote Play, a pairing); 0 when accepted, -1 with bc_error()
void bc_stop(void);
// 1 from taking the USB DualSense until it is re-enumerated for the system again.
int bc_holds_controller(void);
// From a crash handler (async-signal-safe): re-enumerate the controller if Go Live holds it.
void bc_rescue_controller(void);
void bc_request_keyframe(void); // a viewer lost the picture
// Before Go Live: "replug" when the USB controller should be unplugged and plugged in again
// first (ds5_usb_replug_state), "unknown" when that cannot be told, else "ok".
const char *bc_controller(void);
void bc_profile(int *w, int *h, int *fps); // the picture's size and rate (prefs golive_quality)
const char *bc_state(void); // "off", "starting", "live" or "error"
const char *bc_error(void); // why it stopped or could not start; "" when none
// 1 when Go Live would (or does) run as Remote Play, which needs a pairing; 0 for the wireless capture.
int bc_needs_pairing(void);
// Who says whether a DualSense is on USB (default on the console: ds5_usb_present; none on the host).
void bc_set_usb_probe(int (*probe)(void));

