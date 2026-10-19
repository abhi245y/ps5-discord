// Minimal RFC 6455 websocket client on top of tls_conn.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "tls.h"

enum { WS_TEXT = 1, WS_BINARY = 2, WS_CLOSE = 8, WS_PING = 9, WS_PONG = 10 };

typedef struct {
    tls_conn tls;
    unsigned char rbuf[16384];
    size_t rpos, rlen;
    unsigned char *msg; // last reassembled message, NUL-terminated, owned by ws
    size_t msg_len, msg_cap;
    int close_code;     // set when the server closes
} ws_conn;

// url like "wss://gateway.discord.gg/?v=9&encoding=json"; 0 on success.
int ws_open(ws_conn *ws, const char *url);
int ws_send(ws_conn *ws, int opcode, const void *data, size_t len);
// Next data message: returns WS_TEXT/WS_BINARY (payload in ws->msg/msg_len),
// 0 on timeout, WS_CLOSE when the server closed (ws->close_code), <0 on error.
int ws_recv(ws_conn *ws, int timeout_ms);
void ws_close(ws_conn *ws, uint16_t code);

// Exposed for tests: build a masked client frame into out (needs len + 14 bytes).
size_t ws_frame(unsigned char *out, int opcode, const void *data, size_t len, uint32_t mask);
