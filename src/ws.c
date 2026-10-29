#include "ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

size_t ws_frame(unsigned char *out, int opcode, const void *data, size_t len, uint32_t mask)
{
    size_t n = 0;
    out[n++] = 0x80 | (unsigned char)opcode; // FIN, never fragment outgoing frames
    if (len < 126) {
        out[n++] = 0x80 | (unsigned char)len;
    } else if (len < 65536) {
        out[n++] = 0x80 | 126;
        out[n++] = (unsigned char)(len >> 8);
        out[n++] = (unsigned char)len;
    } else {
        out[n++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--)
            out[n++] = (unsigned char)((uint64_t)len >> (8 * i));
    }
    unsigned char m[4] = {mask >> 24, mask >> 16, mask >> 8, mask};
    memcpy(out + n, m, 4);
    n += 4;
    const unsigned char *p = data;
    for (size_t i = 0; i < len; i++)
        out[n + i] = p[i] ^ m[i & 3];
    return n + len;
}

int ws_send(ws_conn *ws, int opcode, const void *data, size_t len)
{
    unsigned char stack[1024], *buf = len + 14 <= sizeof stack ? stack : malloc(len + 14);
    if (!buf)
        return -1;
    // ponytail: rand() mask; masking only guards browser proxies, not us. Seeded in main.
    size_t n = ws_frame(buf, opcode, data, len, (uint32_t)rand());
    int ret = tls_write_all(&ws->tls, buf, n);
    if (buf != stack)
        free(buf);
    return ret;
}

static int read_exact(ws_conn *ws, void *dst, size_t len)
{
    unsigned char *d = dst;
    while (len > 0) {
        if (ws->rpos == ws->rlen) {
            int r = tls_read(&ws->tls, ws->rbuf, sizeof ws->rbuf);
            if (r <= 0)
                return -1;
            ws->rpos = 0;
            ws->rlen = (size_t)r;
        }
        size_t take = ws->rlen - ws->rpos < len ? ws->rlen - ws->rpos : len;
        memcpy(d, ws->rbuf + ws->rpos, take);
        ws->rpos += take;
        d += take;
        len -= take;
    }
    return 0;
}

// Discord sends READY compressed, a few MB at most.
#define WS_MAX_MESSAGE ((size_t)16 << 20)

static int msg_reserve(ws_conn *ws, size_t need)
{
    if (need <= ws->msg_cap)
        return 0;
    size_t cap = ws->msg_cap ? ws->msg_cap : 65536;
    while (cap < need)
        cap *= 2;
    unsigned char *p = realloc(ws->msg, cap);
    if (!p)
        return -1;
    ws->msg = p;
    ws->msg_cap = cap;
    return 0;
}

int ws_open(ws_conn *ws, const char *url)
{
    char host[256], path[512];
    memset(ws, 0, sizeof *ws);
    mbedtls_net_init(&ws->tls.net); // fd -1, so tls_close after an early failure is harmless
    mbedtls_ssl_init(&ws->tls.ssl);
    const char *rest = strncmp(url, "wss://", 6) == 0 ? url + 6 : url;
    size_t hlen = strcspn(rest, "/?");
    if (hlen == 0 || hlen >= sizeof host)
        return -1;
    memcpy(host, rest, hlen);
    host[hlen] = 0;
    snprintf(path, sizeof path, "%s%s", rest[hlen] == '/' ? "" : "/", rest + hlen);

    // host may carry ":port" (voice endpoints do); the default is 443.
    char name[256], port[8] = "443";
    snprintf(name, sizeof name, "%s", host);
    char *colon = strrchr(name, ':');
    if (colon) {
        *colon = 0;
        snprintf(port, sizeof port, "%s", colon + 1);
    }
    if (tls_connect(&ws->tls, name, port) != 0)
        return -1;

    char req[1024];
    // ponytail: fixed Sec-WebSocket-Key; we do not verify Sec-WebSocket-Accept either.
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
                     "Origin: https://discord.com\r\n"
                     "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
                     "Chrome/152.0.0.0 Safari/537.36\r\n\r\n",
                     path, host);
    if (tls_write_all(&ws->tls, req, (size_t)n) != 0)
        return -1;

    // Read the response headers byte by byte so frame bytes after them stay in rbuf.
    char resp[2048];
    size_t len = 0;
    while (len < sizeof resp - 1) {
        if (read_exact(ws, resp + len, 1) != 0)
            return -1;
        len++;
        if (len >= 4 && memcmp(resp + len - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    resp[len] = 0;
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0) {
        LOG("ws: upgrade refused: %.*s", (int)strcspn(resp, "\r"), resp);
        return -1;
    }
    return 0;
}

int ws_recv(ws_conn *ws, int timeout_ms)
{
    if (ws->rpos == ws->rlen) {
        int w = tls_wait(&ws->tls, timeout_ms);
        if (w <= 0)
            return w;
    }
    ws->msg_len = 0;
    int msg_op = 0;
    for (;;) {
        unsigned char h[2];
        if (read_exact(ws, h, 2) != 0)
            return -1;
        int fin = h[0] & 0x80, op = h[0] & 0x0f;
        uint64_t len = h[1] & 0x7f;
        if (h[1] & 0x80) // servers must not mask
            return -1;
        if (len >= 126) {
            unsigned char ext[8];
            int k = len == 126 ? 2 : 8;
            if (read_exact(ws, ext, (size_t)k) != 0)
                return -1;
            len = 0;
            for (int i = 0; i < k; i++)
                len = len << 8 | ext[i];
        }
        if (op >= 8) { // control frame, max 125 bytes, may arrive mid-message
            unsigned char ctl[125];
            if (len > sizeof ctl || read_exact(ws, ctl, (size_t)len) != 0)
                return -1;
            if (op == WS_PING) {
                ws_send(ws, WS_PONG, ctl, (size_t)len);
            } else if (op == WS_CLOSE) {
                ws->close_code = len >= 2 ? ctl[0] << 8 | ctl[1] : 1005;
                LOG("ws: server closed %d %.*s", ws->close_code, len > 2 ? (int)len - 2 : 0, ctl + 2);
                return WS_CLOSE;
            }
            continue;
        }
        if (op != 0)
            msg_op = op;
        if (len > WS_MAX_MESSAGE - ws->msg_len) { // also keeps msg_len + len + 1 from wrapping
            LOG("ws: message over %zu MiB, dropping the connection", WS_MAX_MESSAGE >> 20);
            return -1;
        }
        if (msg_reserve(ws, ws->msg_len + (size_t)len + 1) != 0 ||
            read_exact(ws, ws->msg + ws->msg_len, (size_t)len) != 0)
            return -1;
        ws->msg_len += (size_t)len;
        if (fin) {
            ws->msg[ws->msg_len] = 0; // text messages can be used as C strings
            return msg_op;
        }
    }
}

void ws_close(ws_conn *ws, uint16_t code)
{
    unsigned char c[2] = {code >> 8, code & 0xff};
    ws_send(ws, WS_CLOSE, c, 2);
    tls_close(&ws->tls);
    free(ws->msg);
    ws->msg = NULL;
    ws->msg_cap = ws->msg_len = 0;
}
