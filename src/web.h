// Control port HTTP API.
// LAN:        GET / (status page), GET /api/state, POST /api/quit
// Local only: GET /api/chat, /api/events?since=N, /api/messages?channel=&guild=&before=,
//             POST /api/send {"channel","content","nonce"}, GET /api/mentions,
//             GET /api/image?url= (Discord CDN only), POST /api/ack {"channel","message"}
//             POST /api/prefs {"key": value, ...} (prefs.h), POST /api/signout
#pragma once

// Serves forever on an already listening socket.
void web_serve(int listen_fd);
void core_sign_out(void); // main.c: does not return
