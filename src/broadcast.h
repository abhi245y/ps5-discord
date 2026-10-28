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

void bc_init(const char *data_dir); // where the Remote Play pairing (remoteplay.bin) is kept
void bc_set_sink(const bc_sink *sink); // NULL: frames are only counted
int bc_start(void); // needs a call and a pairing; 0 when accepted, -1 with bc_error()
void bc_stop(void);
void bc_request_keyframe(void); // a viewer lost the picture
const char *bc_state(void); // "off", "starting", "live" or "error"
const char *bc_error(void); // why it stopped or could not start; "" when none

// Remote Play pairing with the console itself, once: the user opens Settings > System >
// Remote Play > Link Device on the console and enters its 8-digit PIN in the app.
// pin NULL or "": the core makes the PIN itself through the console's Remote Play service,
// so the user enters nothing; if the console refuses that, the state turns "need_pin".
int bc_paired(void);              // 1 when remoteplay.bin is there
int bc_pair(const char *pin);     // starts pairing; 0 when started, -1 with bc_pair_error()
int bc_unpair(void);              // forgets the pairing here (the console lists it until removed there)
const char *bc_pair_state(void);  // "idle", "pairing", "paired", "need_pin" or "error"
const char *bc_pair_error(void);
// The pairing's console name, console user and when it was made (unix seconds; 0 unknown).
void bc_pair_info(char console[64], char user[64], long long *since);
