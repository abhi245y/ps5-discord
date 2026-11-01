// SPDX-License-Identifier: AGPL-3.0-or-later
// A Bluetooth headset on a USB Bluetooth adapter (console only; the host build has a stub).
// One radio thread owns the adapter (third_party/bluev). While a headset is connected it
// is the call's audio device (voice_audio.h vau_ext_*): the game's sound and the call
// play on it, its mic feeds the call. AirPods: A2DP plus Apple's microphone stream (AACP,
// AAC-ELD); other headsets: hands-free (planned).
#pragma once

#include <cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts the radio thread. data_dir: where pairings are kept; firmware_dir: adapter
// firmware (rtl8761bu_fw.bin). Once, at startup.
void hs_init(const char *data_dir, const char *firmware_dir);

// From any thread; the radio thread acts on them in order. 0 queued, -1 refused.
int hs_power(int on);              // open or close the adapter
int hs_scan(void);                 // 30 s inquiry
int hs_connect(const char *addr);  // "AA:BB:CC:DD:EE:FF"; pairs when needed
int hs_disconnect(void);
int hs_forget(const char *addr);
// The core is quitting: hang up the headset and close the adapter (up to 3 s). The
// switch's saved state stays, so the next start turns it on again.
void hs_shutdown(void);

// {"on":..,"enabled":..,"state":..,"message":..,"adapter":..,"headset":{..},"devices":[..]}
// state: off idle scanning connecting connected. A message that starts with a capital is
// for the user (why it is off, or why connecting failed); the others are for the log.
cJSON *hs_state_json(void);

#ifdef __cplusplus
}
#endif
