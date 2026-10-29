// SPDX-License-Identifier: AGPL-3.0-or-later
// Go Live: the Discord side of a broadcast (docs/go-live.md). Asks the gateway for a
// stream in the current call, opens the stream's own voice connection (websocket, UDP,
// its own DAVE group) and sends the broadcast's H.264 picture and Opus game sound.
#pragma once
#include <cJSON.h>

void stream_init(void);       // gives the broadcast (broadcast.h) its frame sink
int stream_start(void);       // in a connected call; 0 when asked, -1 with stream_error()
void stream_stop(void);
// Gateway thread: the next op (18 create, 22 paused, 19 delete) to send, or NULL. Caller frees.
cJSON *stream_take_gateway(void);
int stream_gateway_pending(void); // ops queued for the gateway thread
// Gateway dispatch: STREAM_CREATE, STREAM_SERVER_UPDATE, STREAM_UPDATE, STREAM_DELETE.
void stream_on_dispatch(const char *type, cJSON *d);
const char *stream_state(void); // "off", "starting", "live", "error"
const char *stream_error(void);
int stream_viewers(void);
