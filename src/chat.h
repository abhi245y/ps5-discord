// Chat state for the UI: guild/channel/DM lists from READY and recent message events.
// Thread-safe: the gateway thread writes, the web thread reads.
#pragma once
#include <cJSON.h>

void chat_set_token(const char *token);
const char *chat_token(void); // valid while the session lives

// Every gateway dispatch: READY builds the account, the rest keep it current
// (presence, voice states, read state, DMs) and feed the message event ring.
void chat_dispatch(const char *type, cJSON *d);

// {"me", "guilds", "dms", "users", "friends", "presence", "voice", "reads", "my_voice"}; caller frees
char *chat_tree_json(void);
// A key pressed (or repeating) on the console's keyboard: a "KEY" event for the app.
void chat_key(int usage, int mods, int led);
char *chat_events_json(long since, int wait_ms); // {"next":N, "events":[...]}; waits up to wait_ms for one; caller frees
