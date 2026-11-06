// Chat state for the UI: guild/channel/DM lists from READY and recent message events.
// Thread-safe: the gateway thread writes, the web thread reads.
#pragma once
#include <cJSON.h>
#include <time.h>

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

// Every new, edited or deleted message after the tree took it (gateway thread, no lock held; d is only
// valid during the call). mention: a new message that counts as a mention (we are mentioned, or a DM).
void chat_set_message_sink(void (*sink)(const char *type, cJSON *d, int mention));
// Where a channel is, for the in-game overlay: the server's name ("" for a DM), the channel's name (a DM:
// the other person, or the group's name), voice for a voice channel's own text chat, and for a one-to-one
// DM the person's id and avatar hash. 0 when the channel is not in the tree.
typedef struct {
    char server[64], name[64];
    int dm, voice;
    char user[24], avatar[40];
} chat_place;
int chat_place_of(const char *channel_id, chat_place *out);
// A message object (REST or gateway) as the overlay lists it: plain text (mentions as names), the sender,
// the time, and mine: it mentions the account (not our own message). 0 for what is not a plain message.
typedef struct {
    char id[24], channel[24], author[24], name[48], avatar[40], text[160];
    time_t at;
    int mine;
} chat_msg;
int chat_msg_read(cJSON *m, chat_msg *out);
