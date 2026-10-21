// Discord voice connection: join/leave a server voice channel or a DM call, and stay
// present (muted by default). The state machine behind /api/voice (docs/voice-api.md).
#pragma once
#include <cJSON.h>

// Opcode 4 (VOICE_STATE_UPDATE) for the gateway thread to send. Empty channel = leave.
typedef struct {
    char guild[32];   // empty for DM calls
    char channel[32];
    int mute, deaf;
} voice_op4;

void voice_set_self(const char *user_id);                      // READY
void voice_init(const char *data_dir); // where our last gateway session id is kept
void voice_on_ready(cJSON *ready);      // READY: rejoin the wanted call or leave our own ghost
void voice_join(const char *guild_id, const char *channel_id); // guild_id NULL for DM calls
void voice_leave(void);
// mute/deaf: 0 or 1, -1 keeps the current value. 0, or -1 when not in a call.
int voice_set_flags(int mute, int deaf);
// Gateway thread, between reads: 1 with *out when op 4 must be sent now.
int voice_take_request(voice_op4 *out);
// Gateway dispatch: VOICE_STATE_UPDATE and VOICE_SERVER_UPDATE. Other users are ignored.
void voice_on_state(cJSON *d);
void voice_on_server(cJSON *d);
// The reason the last voice command was refused ("" clears): shown as "refused" in GET /api/voice.
void voice_refuse(const char *why);
// GET /api/voice body; caller frees.
char *voice_state_json(void);
