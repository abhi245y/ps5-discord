// Chat state for the UI: guild/channel/DM lists from READY and recent message events.
// Thread-safe: the gateway thread writes, the web thread reads.
#pragma once
#include <cJSON.h>

void chat_set_token(const char *token);
const char *chat_token(void); // valid while the session lives

// Every gateway dispatch: READY builds the account, the rest keep it current
// (presence, voice states, read state, DMs) and feed the message event ring.
void chat_dispatch(const char *type, cJSON *d);

// {"me", "guilds", "dms", "users", "friends", "presence", "voice", "reads", "calls", "my_voice"}; caller frees
char *chat_tree_json(void);
// The account's id into out. 0 until READY: the "not signed in" check of the voice routes.
int chat_me_id(char *out, size_t cap);
// 1 when a call exists in this DM (ringing or running), from the calls entries.
int chat_call_active(const char *channel_id);
// Where a message worth an alert goes (not quiet, not muted), and a ring: who and where,
// what, the sender's avatar URL, urgent for a ring. main.c: app_alert (notify.h).
void chat_set_alert(void (*sink)(const char *context, const char *text, const char *icon, int urgent));
// A key pressed (or repeating) on the console's keyboard: a "KEY" event for the app.
void chat_key(int usage, int mods, int led);
char *chat_events_json(long since, int wait_ms); // {"next":N, "events":[...]}; waits up to wait_ms for one; caller frees
