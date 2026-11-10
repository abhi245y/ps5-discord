// SPDX-License-Identifier: AGPL-3.0-or-later
// The in-game overlay's feed (overlay/, injected into the system UI): five times a second
// the call's state goes into its text channel as one short line (overlay/src/app/
// discord_app.inc has the format), and the call panel's presses come back as commands
// (cmd.bin). With Settings' overlay on, the core starts the overlay service through elfldr.
// Console only; the host build writes nothing.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

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
// Whether the card mode allows a glance; independent of Go Live. Exposed for the selftest.
int overlay_glance_enabled(int mode, int talking);

// settings: the mini settings' values, dot-separated, in discord_app.inc's order.
int overlay_line(char *out, int cap, char state, char me, int glance, const char *live, int viewers,
                 const char *call, const char *timer, const char *quality, const char *settings,
                 const overlay_headset *bt, const overlay_person *people, int count);

// send.bin (overlay/src/core/protocol.hpp Outbox, up to 8192 bytes of text): the seq and the
// NUL-terminated text, 0 when the file is missing or not valid. cap must be at least 8193.
// channel (cap at least 24): the text chat's id as digits, "" for the call's own chat; a file with
// anything else there is not valid. Exposed for the selftest.
uint64_t overlay_read_outbox(const char *path, char *text, size_t cap, char *channel, size_t channel_cap);

// ---- the chat feed (chat.bin, overlay/src/app/discord_app.inc has the format) ----
#define OVERLAY_FEED_MAX 16384 // bytes; the overlay reads no more
#define OVERLAY_MSGS 20        // the chat's messages
#define OVERLAY_MENTIONS 10    // the mentions list
typedef struct {
    char id[24], author[24], ahash[40], avatar[64], name[48], time[12], text[160];
    int mine; // mentions the account
} overlay_msg;
typedef struct {
    char id[24], chan[24], author[24], ahash[40], avatar[64], place[80], name[48], text[160];
    time_t at;
    int dm; // a DM message: one entry per DM
} overlay_mention;
typedef struct {
    char kind; // c a channel, d a DM, - none
    char chan[24], title[72], sub[72], icon[64], focus[24];
    int unseen;
    int rev; // Open chat commands so far: the feed changes with each, so the overlay hears of a repeat
    overlay_msg msgs[OVERLAY_MSGS];
    int nmsg; // oldest first
    overlay_mention men[OVERLAY_MENTIONS];
    int nmen; // newest first
} overlay_chat;
// The feed text, at most cap-1 (and OVERLAY_FEED_MAX) bytes: whole messages and mentions only, the oldest
// messages go first when it is too long. Text is cut at 120 bytes, control characters become spaces.
// Returns its length.
int overlay_chat_feed(char *out, int cap, const overlay_chat *c, time_t now);
// "now", "2m", "1h", "3d": how long ago, for the mentions.
void overlay_age(char *out, size_t cap, long seconds);
// "6:14 PM".
void overlay_clock(char *out, size_t cap, const struct tm *tm);
// The mentions, newest first by time, at most OVERLAY_MENTIONS. Adds m (0 when its id is already there, or
// it is older than all ten); a DM message replaces that DM's entry. Returns 1 when added.
int overlay_mention_add(overlay_chat *c, const overlay_mention *m);
// The chat's messages: add at the end (0 when the id is there), update the text, remove by id. 1 when done.
int overlay_msg_add(overlay_chat *c, const overlay_msg *m);
int overlay_msg_edit(overlay_chat *c, const char *id, const char *text);
int overlay_msg_remove(overlay_chat *c, const char *id);
// Writes data to path atomically (path.tmp, then a rename). 0 when written.
int overlay_write_file(const char *path, const char *data);

// The channel the app showed last (api_messages): the Chat tab follows it when there is no call.
void overlay_note_channel(const char *channel_id);
