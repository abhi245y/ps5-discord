// Control port HTTP API.
// LAN:        GET / (status page), GET /api/state, POST /api/quit
// Local only: GET /api/chat, /api/events?since=N, /api/messages?channel=&guild=&before=,
//             POST /api/send {"channel","content","nonce"}
#pragma once

// Serves forever on an already listening socket.
void web_serve(int listen_fd);
