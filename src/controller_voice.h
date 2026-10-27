// SPDX-License-Identifier: AGPL-3.0-or-later
// Controller voice: during a call, the DualSense on USB is the headset (its mic, its
// speaker or headphones) while the game keeps getting its input through a loopback
// Remote Play session. Turned on from the app before a game; off with the call.
#pragma once

void cv_init(const char *data_dir); // where the Remote Play pairing (remoteplay.bin) is kept
// on 1: start (needs a call and a pairing); 0: stop. 0 when accepted, -1 with cv_error().
int cv_set(int on);
const char *cv_state(void); // "off", "starting", "on" or "error"
const char *cv_error(void); // why it stopped or could not start; "" when none
