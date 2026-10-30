// SPDX-License-Identifier: AGPL-3.0-or-later
// The gateway's session choices (src/gateway.c, included for its statics) over a fake
// WebSocket: HELLO identifies without a session and resumes with one, an invalid session
// drops it only when Discord says it is not resumable, and a heartbeat that goes
// unanswered ends the connection.
#include <unistd.h>
// docs: 1-5 s before a fresh identify; not in a test
#define sleep(s) ((void)(s))
#include "../src/gateway.c"
#undef sleep

#include <assert.h>

// main.c's, which this test does not link.
int core_sign_out(void)
{
    return -1;
}
void core_quit(int forget_token)
{
    (void)forget_token;
}
void app_alert(const char *context, const char *text, const char *icon, int urgent)
{
    (void)context, (void)text, (void)icon, (void)urgent;
}

// The fake socket: what the gateway sent last, and what it reads (HELLO once, then silence).
static int sent_op = -1, heartbeats;
static char sent_session[128];
static double sent_seq = -2;
static const char *script;

int ws_open(ws_conn *ws, const char *url)
{
    (void)url;
    memset(ws, 0, sizeof *ws);
    return 0;
}

int ws_send(ws_conn *ws, int opcode, const void *data, size_t len)
{
    (void)ws, (void)opcode, (void)len;
    cJSON *m = cJSON_Parse(data);
    assert(m);
    sent_op = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(m, "op"));
    heartbeats += sent_op == OP_HEARTBEAT;
    cJSON *d = cJSON_GetObjectItem(m, "d");
    const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(d, "session_id"));
    snprintf(sent_session, sizeof sent_session, "%s", sid ? sid : "");
    cJSON *seq = cJSON_GetObjectItem(d, "seq");
    sent_seq = cJSON_IsNumber(seq) ? seq->valuedouble : -2;
    cJSON_Delete(m);
    return 0;
}

int ws_recv(ws_conn *ws, int timeout_ms)
{
    if (script) {
        size_t n = strlen(script);
        free(ws->msg);
        ws->msg = (unsigned char *)strdup(script);
        ws->msg_len = n;
        script = NULL;
        return WS_TEXT;
    }
    usleep((useconds_t)timeout_ms * 1000);
    return 0;
}

void ws_close(ws_conn *ws, uint16_t code)
{
    (void)code;
    free(ws->msg);
    ws->msg = NULL;
}

static int64_t elapsed_since(int64_t t)
{
    return now_ms() - t;
}

int main(void)
{
    gateway g = {.token = "t", .seq = -1};

    // No session: HELLO identifies.
    assert(on_message(&g, "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}") == 0);
    assert(sent_op == OP_IDENTIFY && g.heartbeat_ms == 45000 && g.acked);

    // A session from READY: the next HELLO resumes it at the last sequence.
    assert(on_message(&g, "{\"op\":0,\"s\":7,\"t\":\"READY\",\"d\":{\"session_id\":\"abc\","
                          "\"resume_gateway_url\":\"wss://r.example\",\"user\":{\"id\":\"1\"}}}") == 0);
    assert(strcmp(g.session_id, "abc") == 0 && g.seq == 7);
    assert(on_message(&g, "{\"op\":10,\"d\":{\"heartbeat_interval\":0}}") == 0);
    assert(sent_op == OP_RESUME && strcmp(sent_session, "abc") == 0 && sent_seq == 7);
    assert(g.heartbeat_ms == 1000); // 0 would heartbeat on every turn

    // Invalid session, resumable: keep it and reconnect. Not resumable: identify next time.
    g.reconnect = 0;
    assert(on_message(&g, "{\"op\":9,\"d\":true}") == 0 && g.reconnect && strcmp(g.session_id, "abc") == 0);
    g.reconnect = 0;
    assert(on_message(&g, "{\"op\":9,\"d\":false}") == 0 && g.reconnect && !g.session_id[0] && !g.resume_url[0]);
    assert(on_message(&g, "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}") == 0 && sent_op == OP_IDENTIFY);

    // Discord asks for a heartbeat: one goes out at once and waits for its ACK.
    assert(on_message(&g, "{\"op\":1,\"d\":null}") == 0 && sent_op == OP_HEARTBEAT && !g.acked);
    assert(on_message(&g, "{\"op\":11}") == 0 && g.acked);

    // A connection whose heartbeat is never acknowledged ends (client side: 0) after the
    // second beat is due, within about two intervals of the 1 s floor.
    gateway z = {.token = "t", .seq = -1};
    script = "{\"op\":10,\"d\":{\"heartbeat_interval\":1000}}";
    heartbeats = 0;
    int64_t t = now_ms();
    assert(run_connection(&z) == 0);
    assert(heartbeats == 1 && elapsed_since(t) >= 1000 && elapsed_since(t) < 4000);

    puts("gateway: identify or resume on HELLO, invalid sessions, heartbeat timeout passed");
    return 0;
}
