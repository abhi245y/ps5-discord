// SPDX-License-Identifier: AGPL-3.0-or-later
// The in-game overlay's feed (overlay/, injected into the system UI): five times a second
// the call's state goes into its text channel as one short line (overlay/src/app/
// discord_app.inc has the format), and the call panel's presses come back as commands
// (cmd.bin). With Settings' overlay on, the core starts the overlay service through elfldr.
// Console only; the host build writes nothing.
#pragma once
#include <stddef.h>
#include <stdint.h>

void overlay_init(void);     // starts the feed thread; once, at startup
void overlay_shutdown(void); // the core is quitting: the overlay hides (it stays loaded)
// Settings: off, starting, running, stopped (needs a reload), reloading.
const char *overlay_state(void);
// Restart the system UI and load the overlay again (about 45 s, in the background). 0 when
// started; -1 when one runs already or the overlay is off.
int overlay_reload(void);

// One person in the call, as the overlay gets them. avatar: the round PNG's stem in the
// overlay's av/ folder, "" until it is there.
typedef struct {
    char id[24], avatar[64], name[64];
    char flag; // 't' talking, 'm' muted, 'd' deafened, '-'
} overlay_person;
// The line (overlay/src/app/discord_app.inc reads it), at most 2047 bytes; talking people
// first. live: the Go Live time ("" when off); quality: Go Live's ("720p · 30 fps").
// Exposed for the selftest.
// The paired headsets (up to four) and the connected one's address ("" when none). on: 0
// Bluetooth off, 1 on, 2 on with AirPods connected (Call mode only applies to those).
typedef struct {
    int on, count;
    char connected[18];
    char addr[4][18], name[4][40];
} overlay_headset;
// settings: the mini settings' values, dot-separated, in discord_app.inc's order.
int overlay_line(char *out, int cap, char state, char me, int glance, const char *live, int viewers,
                 const char *call, const char *timer, const char *quality, const char *settings,
                 const overlay_headset *bt, const overlay_person *people, int count);

// send.bin (overlay/src/core/protocol.hpp Outbox, up to 8192 bytes of text): the seq and the
// NUL-terminated text, 0 when the file is missing or not valid. cap must be at least 8193.
// Exposed for the selftest.
uint64_t overlay_read_outbox(const char *path, char *text, size_t cap);
