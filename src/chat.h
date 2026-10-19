// Chat state for the UI: guild/channel/DM lists from READY and recent message events.
// Thread-safe: the gateway thread writes, the web thread reads.
#pragma once
#include <cJSON.h>

void chat_set_token(const char *token);
const char *chat_token(void); // valid while the session lives

void chat_load_ready(cJSON *ready);
void chat_event(const char *type, cJSON *d); // MESSAGE_CREATE / UPDATE / DELETE

char *chat_tree_json(void);              // {"me":..., "guilds":[...], "dms":[...]}; caller frees
char *chat_events_json(long since);      // {"next":N, "events":[...]}; caller frees
