#include "gateway.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cJSON.h>

#include "log.h"
#include "props.h"
#include "ws.h"
#include "zstream.h"

#define GATEWAY_URL "wss://gateway.discord.gg"
#define GATEWAY_QUERY "/?encoding=json&v=9&compress=zlib-stream"
// Same value the web client sent in build 641004 (docs/gateway.md).
#define CAPABILITIES 1734653

enum { OP_DISPATCH = 0, OP_HEARTBEAT = 1, OP_IDENTIFY = 2, OP_RESUME = 6, OP_RECONNECT = 7,
       OP_INVALID_SESSION = 9, OP_HELLO = 10, OP_HEARTBEAT_ACK = 11 };

typedef struct {
    const char *token;
    ws_conn ws;
    zstream z;
    int64_t seq; // -1 until the first dispatch
    char session_id[128];
    char resume_url[256];
    int heartbeat_ms;
    int64_t next_heartbeat; // monotonic ms, 0 = not started
    int acked;
    int reconnect; // set by handlers: leave the read loop
} gateway;

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int send_json(gateway *g, cJSON *msg)
{
    char *s = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!s)
        return -1;
    int r = ws_send(&g->ws, WS_TEXT, s, strlen(s));
    free(s);
    return r;
}

static cJSON *op_msg(int op, cJSON *d)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "op", op);
    cJSON_AddItemToObject(m, "d", d ? d : cJSON_CreateNull());
    return m;
}

static int send_heartbeat(gateway *g)
{
    g->acked = 0;
    return send_json(g, op_msg(OP_HEARTBEAT, g->seq >= 0 ? cJSON_CreateNumber((double)g->seq) : NULL));
}

static int send_identify(gateway *g)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "token", g->token);
    cJSON_AddNumberToObject(d, "capabilities", CAPABILITIES);
    cJSON_AddItemToObject(d, "properties", props_create());
    cJSON *pr = cJSON_AddObjectToObject(d, "presence");
    cJSON_AddStringToObject(pr, "status", "unknown");
    cJSON_AddNumberToObject(pr, "since", 0);
    cJSON_AddArrayToObject(pr, "activities");
    cJSON_AddFalseToObject(pr, "afk");
    cJSON_AddFalseToObject(d, "compress");
    cJSON *cs = cJSON_AddObjectToObject(d, "client_state");
    cJSON_AddObjectToObject(cs, "guild_versions");
    LOG("gateway: identify");
    return send_json(g, op_msg(OP_IDENTIFY, d));
}

static int send_resume(gateway *g)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "token", g->token);
    cJSON_AddStringToObject(d, "session_id", g->session_id);
    cJSON_AddNumberToObject(d, "seq", (double)g->seq);
    LOG("gateway: resume seq %lld", (long long)g->seq);
    return send_json(g, op_msg(OP_RESUME, d));
}

static void drop_session(gateway *g)
{
    g->session_id[0] = 0;
    g->resume_url[0] = 0;
    g->seq = -1;
}

static void on_dispatch(gateway *g, const char *t, cJSON *d)
{
    if (strcmp(t, "READY") == 0) {
        const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(d, "session_id"));
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(d, "resume_gateway_url"));
        const char *name = cJSON_GetStringValue(
            cJSON_GetObjectItem(cJSON_GetObjectItem(d, "user"), "username"));
        snprintf(g->session_id, sizeof g->session_id, "%s", sid ? sid : "");
        snprintf(g->resume_url, sizeof g->resume_url, "%s", url ? url : "");
        LOG("gateway: READY as %s, %d guilds, %d DMs, resume via %s", name ? name : "?",
            cJSON_GetArraySize(cJSON_GetObjectItem(d, "guilds")),
            cJSON_GetArraySize(cJSON_GetObjectItem(d, "private_channels")), g->resume_url);
    } else if (strcmp(t, "RESUMED") == 0) {
        LOG("gateway: RESUMED");
    } else {
        // Event names only; message contents stay out of the console log.
        LOG("gateway: %s s=%lld", t, (long long)g->seq);
    }
}

static int on_message(gateway *g, const char *json)
{
    cJSON *m = cJSON_Parse(json);
    if (!m)
        return LOG("gateway: bad json"), -1;
    int op = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(m, "op"));
    cJSON *s = cJSON_GetObjectItem(m, "s"), *d = cJSON_GetObjectItem(m, "d");
    if (cJSON_IsNumber(s))
        g->seq = (int64_t)s->valuedouble;

    int r = 0;
    switch (op) {
    case OP_HELLO:
        g->heartbeat_ms = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(d, "heartbeat_interval"));
        g->next_heartbeat = now_ms() + (int64_t)(g->heartbeat_ms * (rand() / (RAND_MAX + 1.0)));
        g->acked = 1;
        LOG("gateway: HELLO, heartbeat %d ms", g->heartbeat_ms);
        r = g->session_id[0] ? send_resume(g) : send_identify(g);
        break;
    case OP_HEARTBEAT_ACK:
        g->acked = 1;
        break;
    case OP_HEARTBEAT:
        r = send_heartbeat(g);
        break;
    case OP_RECONNECT:
        LOG("gateway: server asked to reconnect");
        g->reconnect = 1;
        break;
    case OP_INVALID_SESSION:
        LOG("gateway: invalid session (resumable: %s)", cJSON_IsTrue(d) ? "yes" : "no");
        if (!cJSON_IsTrue(d)) {
            drop_session(g);
            sleep(1 + rand() % 5); // docs: wait 1-5 s before a fresh identify
        }
        g->reconnect = 1;
        break;
    case OP_DISPATCH: {
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(m, "t"));
        if (t)
            on_dispatch(g, t, d);
        break;
    }
    default:
        LOG("gateway: unhandled op %d", op);
    }
    cJSON_Delete(m);
    return r;
}

// One connection. Returns the server close code, 0 for a client-side reconnect, -1 on error.
static int run_connection(gateway *g)
{
    char url[320];
    snprintf(url, sizeof url, "%s%s", g->resume_url[0] ? g->resume_url : GATEWAY_URL, GATEWAY_QUERY);
    if (ws_open(&g->ws, url) != 0) {
        tls_close(&g->ws.tls);
        return -1;
    }
    if (zstream_init(&g->z) != 0) {
        ws_close(&g->ws, 1000);
        return -1;
    }
    g->next_heartbeat = 0;
    g->reconnect = 0;

    int result = 0;
    for (;;) {
        int timeout = 30000; // until HELLO
        if (g->next_heartbeat) {
            int64_t left = g->next_heartbeat - now_ms();
            timeout = left > 0 ? (int)left : 0;
        }
        int r = ws_recv(&g->ws, timeout);
        if (r == 0) {
            if (!g->next_heartbeat) {
                LOG("gateway: no HELLO");
                result = -1;
                break;
            }
            if (!g->acked) { // zombied connection
                LOG("gateway: heartbeat not acked, reconnecting");
                break;
            }
            if (send_heartbeat(g) != 0) {
                result = -1;
                break;
            }
            g->next_heartbeat += g->heartbeat_ms;
            continue;
        }
        if (r == WS_CLOSE) {
            result = g->ws.close_code;
            break;
        }
        if (r < 0) {
            result = -1;
            break;
        }
        const char *json = (const char *)g->ws.msg;
        if (r == WS_BINARY) {
            int z = zstream_feed(&g->z, g->ws.msg, g->ws.msg_len);
            if (z < 0) {
                LOG("gateway: inflate failed");
                result = -1;
                break;
            }
            if (z == 0)
                continue;
            json = g->z.out;
        }
        if (on_message(g, json) != 0 || g->reconnect)
            break;
    }
    zstream_free(&g->z);
    // 4000 keeps the session resumable; 1000 would end it.
    ws_close(&g->ws, 4000);
    return result;
}

int gateway_run(const char *token)
{
    gateway g = {.token = token, .seq = -1};
    int backoff = 1;
    for (;;) {
        int code = run_connection(&g);
        switch (code) {
        case 4004: // authentication failed
        case 4010: case 4011: case 4012: case 4013: case 4014:
            LOG("gateway: fatal close %d, stopping", code);
            return code;
        case 4007: // invalid seq
        case 4009: // session timed out
            drop_session(&g);
            break;
        }
        if (g.heartbeat_ms) // got HELLO this time: the network is fine
            backoff = 1;
        g.heartbeat_ms = 0;
        LOG("gateway: connection ended (%d), retry in %d s", code, backoff);
        sleep((unsigned)backoff);
        backoff = backoff < 60 ? backoff * 2 : 60;
    }
}
