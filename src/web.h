// Status web UI on the control port: GET / page, GET /api/state, POST /api/quit.
#pragma once

// Serves forever on an already listening socket.
void web_serve(int listen_fd);
