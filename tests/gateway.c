// SPDX-License-Identifier: AGPL-3.0-or-later
// The gateway's session choices (src/gateway.c, included for its statics) over a fake
// WebSocket: HELLO identifies without a session and resumes with one, an invalid session
// drops it only when Discord says it is not resumable, and a heartbeat that goes
// unanswered ends the connection. Title names for "Playing" from param.json and param.sfo.
#include <unistd.h>
// docs: 1-5 s before a fresh identify; not in a test
#define sleep(s) ((void)(s))
#include "../src/gateway.c"
#undef sleep

#include <assert.h>
#include <sys/stat.h>

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

// A metadata folder as the console keeps it: dir/<title>/<file>.
static void put(const char *dir, const char *title, const char *file, const void *data, size_t n)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, title);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/%s/%s", dir, title, file);
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(data, 1, n, f) == n);
    fclose(f);
}

static void unput(const char *dir, const char *title, const char *file)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s/%s", dir, title, file);
    unlink(path);
    snprintf(path, sizeof path, "%s/%s", dir, title);
    rmdir(path);
}

static void title_names(void)
{
    char dir[] = "/tmp/gateway-test-XXXXXX", name[128];
    assert(mkdtemp(dir));
    const char *ja = "{\"localizedParameters\":{\"defaultLanguage\":\"ja-JP\",\"ja-JP\":{\"titleName\":\"PRAGMATA\"}}}";
    const char *both = "{\"localizedParameters\":{\"defaultLanguage\":\"ja-JP\",\"ja-JP\":{\"titleName\":\"x\"},"
                       "\"en-US\":{\"titleName\":\"Astro Bot\"}}}";
    put(dir, "PPSA00001", "param.json", ja, strlen(ja));
    put(dir, "PPSA00002", "param.json", both, strlen(both));
    put(dir, "PPSA00003", "param.json", "{}", 2);
    // param.sfo: header, one entry (TITLE, utf-8, 5 bytes of 8), key table, data table.
    unsigned char sfo[64] = {0, 'P', 'S', 'F', 1, 1, 0, 0, 36, 0, 0, 0, 44, 0, 0, 0, 1, 0, 0, 0,
                             0, 0, 4, 2, 5, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0};
    memcpy(sfo + 36, "TITLE", 6);
    memcpy(sfo + 44, "Bloo", 5);
    put(dir, "CUSA00001", "param.sfo", sfo, 52);
    sfo[16] = 2; // two entries, the second past the end
    put(dir, "CUSA00002", "param.sfo", sfo, 52);
    assert(playing_title_name(dir, "PPSA00001", name, sizeof name) == 0 && strcmp(name, "PRAGMATA") == 0);
    assert(playing_title_name(dir, "PPSA00002", name, sizeof name) == 0 && strcmp(name, "Astro Bot") == 0);
    assert(playing_title_name(dir, "PPSA00003", name, sizeof name) != 0);
    assert(playing_title_name(dir, "CUSA00001", name, sizeof name) == 0 && strcmp(name, "Bloo") == 0);
    assert(playing_title_name(dir, "CUSA00002", name, sizeof name) == 0); // TITLE comes first
    sfo[36] = 'X'; // no TITLE, and the second entry's key is out of range
    put(dir, "CUSA00002", "param.sfo", sfo, 52);
    assert(playing_title_name(dir, "CUSA00002", name, sizeof name) != 0);
    assert(playing_title_name(dir, "PPSA09999", name, sizeof name) != 0);
    // 42 three-byte characters: 126 bytes fit in 128 with the NUL, the 43rd is cut whole.
    char json[400] = "{\"localizedParameters\":{\"defaultLanguage\":\"ja-JP\",\"ja-JP\":{\"titleName\":\"";
    for (int i = 0; i < 43; i++)
        strcat(json, "\xe3\x81\x82");
    strcat(json, "\"}}}");
    put(dir, "PPSA00004", "param.json", json, strlen(json));
    assert(playing_title_name(dir, "PPSA00004", name, sizeof name) == 0 && strlen(name) == 126);
    char small[5];
    assert(playing_title_name(dir, "PPSA00004", small, sizeof small) == 0 && strlen(small) == 3); // 4 bytes: one character
    unput(dir, "PPSA00004", "param.json");
    unput(dir, "PPSA00001", "param.json");
    unput(dir, "PPSA00002", "param.json");
    unput(dir, "PPSA00003", "param.json");
    unput(dir, "CUSA00001", "param.sfo");
    unput(dir, "CUSA00002", "param.sfo");
    rmdir(dir);
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

    title_names();
    char out[512];
    assert(playing_icon_url("{\"success\":true,\"metadata\":{\"icon\":\"https://cdn.example/x.png\"}}", out, sizeof out) == 0 &&
           strcmp(out, "https://cdn.example/x.png") == 0);
    assert(playing_icon_url("{\"success\":false,\"metadata\":{\"icon\":\"https://cdn.example/x.png\"}}", out, sizeof out) != 0);
    assert(playing_icon_url("{\"success\":true,\"metadata\":{\"icon\":\"http://cdn.example/x.png\"}}", out, sizeof out) != 0);
    assert(playing_icon_url("not json", out, sizeof out) != 0);
    assert(playing_icon_url("{\"success\":true,\"metadata\":{\"icon\":\"https://cdn.example/x.png\"}}", out, 10) != 0);
    assert(playing_asset("[{\"url\":\"https://cdn.example/x.png\",\"external_asset_path\":\"external/ab/https/cdn.example/x.png\"}]", out,
                         sizeof out) == 0 && strcmp(out, "mp:external/ab/https/cdn.example/x.png") == 0);
    assert(playing_asset("[]", out, sizeof out) != 0 && playing_asset("{}", out, sizeof out) != 0);
    puts("gateway: identify or resume on HELLO, invalid sessions, heartbeat timeout, title names, covers passed");
    return 0;
}
