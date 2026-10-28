// Control port HTTP API.
// LAN:        GET / (status page), GET /api/state, POST /api/quit (the app's Quit too)
// Local only: GET /api/chat, /api/events?since=N, /api/messages?channel=&guild=&before=,
//             POST /api/send {"channel","content","nonce"}, GET /api/mentions,
//             GET /api/image?url= (Discord CDN only), POST /api/ack {"channel","message"}
//             POST /api/prefs {"key": value, ...} (prefs.h), POST /api/signout,
//             GET /api/sticker-packs; /api/send also takes {"sticker": id}
#pragma once

// Serves forever on an already listening socket.
void web_serve(int listen_fd);
int web_app_on_screen(void); // the app asked something in the last few seconds
int core_sign_out(void);         // main.c: ends the session with Discord; 0 when it did
void core_quit(int forget_token); // main.c: stops Go Live and the call, then the core
