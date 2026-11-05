// Chat state for the UI: guild/channel/DM lists from READY and recent message events.
// Thread-safe: the gateway thread writes, the web thread reads.
#pragma once
#include <cJSON.h>

void chat_set_token(const char *token);
void chat_token(char *out, size_t cap); // a copy: "" once the session is reset
// The JSON body of a new message in channel_id (content, nonce, tts false, flags 0, mobile_network_type;
// nonce, sticker and reply_to optional, NULL to leave out). Caller frees. Shared by the web API and the overlay.
char *chat_message_json(const char *channel_id, const char *content, const char *nonce, const char *sticker,
                        const char *reply_to);
void chat_reset(void);        // the account is gone (another console user's is next)

// Every gateway dispatch: READY builds the account, the rest keep it current
// (presence, voice states, read state, DMs) and feed the message event ring.
void chat_dispatch(const char *type, cJSON *d);

// {"me", "guilds", "dms", "users", "friends", "presence", "voice", "reads", "calls", "my_voice"}; caller frees
char *chat_tree_json(void); // with "version": chat_tree_version() when it was made
long chat_tree_version(void); // -1 before READY
// The account's id into out. 0 until READY: the "not signed in" check of the voice routes.
int chat_me_id(char *out, size_t cap);
// 1 when a call exists in this DM (ringing or running), from the calls entries.
int chat_call_active(const char *channel_id);
// The call in channel_id for the in-game overlay: its name (the room, a group DM's name, or
// the other person) into name, and the others in it (not us) into out. Returns how many.
typedef struct {
    char id[24];
    char name[64];
    char avatar[40]; // the avatar hash, "" for Discord's default picture
    int muted, deaf; // self or server
} chat_person;
int chat_call_people(const char *channel_id, char *name, size_t name_cap, chat_person *out, int max);
// Where a message worth an alert goes (not quiet, not muted), and a ring: who and where,
// what, the sender's avatar URL, urgent for a ring. main.c: app_alert (notify.h).
void chat_set_alert(void (*sink)(const char *context, const char *text, const char *icon, int urgent));
char *chat_events_json(long since, int wait_ms); // {"next":N, "events":[...]}; waits up to wait_ms for one; caller frees
