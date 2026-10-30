// Host self-checks for the pure parts: websocket framing and zlib-stream reassembly.
// make test
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include <stdlib.h>
#include <unistd.h>

#include <cJSON.h>

#include "../src/chat.h"
#include "../src/gateway.h"
#include "../src/http.h"
#include "../src/ratelimit.h"
#include "../src/voice.h"
#include "../src/voice_audio.h"
#include "../src/voice_proto.h"
#include "../src/ws.h"
#include "../src/zstream.h"

static void test_ws_frame(void)
{
    unsigned char out[300];
    // RFC 6455 5.7 example: "Hello", mask 37 fa 21 3d
    size_t n = ws_frame(out, WS_TEXT, "Hello", 5, 0x37fa213d);
    const unsigned char want[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
    assert(n == sizeof want && memcmp(out, want, n) == 0);

    char big[200] = {0};
    n = ws_frame(out, WS_BINARY, big, sizeof big, 0);
    assert(n == 4 + 4 + sizeof big && out[1] == (0x80 | 126) && out[2] == 0 && out[3] == 200);
}

// Compress messages like the gateway does: one deflate stream, sync flush per message.
static size_t pack(z_stream *z, const char *msg, unsigned char *out, size_t cap)
{
    z->next_in = (Bytef *)msg;
    z->avail_in = (uInt)strlen(msg);
    z->next_out = out;
    z->avail_out = (uInt)cap;
    assert(deflate(z, Z_SYNC_FLUSH) == Z_OK);
    return cap - z->avail_out;
}

static void test_zstream(void)
{
    z_stream d = {0};
    assert(deflateInit(&d, Z_DEFAULT_COMPRESSION) == Z_OK);
    zstream s;
    assert(zstream_init(&s) == 0);
    unsigned char buf[4096];

    const char *a = "{\"op\":10,\"d\":{\"heartbeat_interval\":41250}}";
    size_t n = pack(&d, a, buf, sizeof buf);
    assert(zstream_feed(&s, buf, n) == 1 && strcmp(s.out, a) == 0);

    // Second message split across two websocket messages: shared context, partial first.
    const char *b = "{\"op\":0,\"t\":\"READY\",\"s\":1,\"d\":{}}";
    n = pack(&d, b, buf, sizeof buf);
    assert(zstream_feed(&s, buf, n - 3) == 0);
    assert(zstream_feed(&s, buf + n - 3, 3) == 1 && strcmp(s.out, b) == 0);

    // Output larger than one 64 KiB step.
    static char large[200000];
    for (size_t i = 0; i < sizeof large - 1; i++)
        large[i] = 'a' + (char)(i * 7 % 26);
    static unsigned char packed[300000];
    n = pack(&d, large, packed, sizeof packed);
    assert(zstream_feed(&s, packed, n) == 1 && s.out_len == strlen(large) && strcmp(s.out, large) == 0);

    zstream_free(&s);
    deflateEnd(&d);
}

static void test_chat(void)
{
    cJSON *ready = cJSON_Parse(
        "{\"user\":{\"id\":\"1\",\"username\":\"me\"},"
        "\"users\":[{\"id\":\"2\",\"username\":\"bob\",\"global_name\":\"Bob\"}],"
        "\"guilds\":[{\"id\":\"10\",\"properties\":{\"name\":\"G\",\"owner_id\":\"9\"},"
        "\"roles\":[{\"id\":\"10\",\"permissions\":\"3072\"},{\"id\":\"16\",\"permissions\":\"0\"}],"
        "\"channels\":["
        "{\"id\":\"11\",\"name\":\"general\",\"type\":0,\"position\":0},"
        "{\"id\":\"12\",\"name\":\"Voice\",\"type\":2,\"position\":1},"
        "{\"id\":\"13\",\"name\":\"staff\",\"type\":0,\"permission_overwrites\":"
        "[{\"id\":\"10\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"}]},"
        "{\"id\":\"14\",\"name\":\"news\",\"type\":5,\"permission_overwrites\":"
        "[{\"id\":\"10\",\"type\":0,\"allow\":\"0\",\"deny\":\"2048\"}]},"
        "{\"id\":\"15\",\"name\":\"club\",\"type\":0,\"permission_overwrites\":"
        "[{\"id\":\"10\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"},"
        "{\"id\":\"16\",\"type\":0,\"allow\":\"1024\",\"deny\":\"0\"}]}]}],"
        "\"merged_members\":[[{\"user_id\":\"1\",\"roles\":[\"16\"]}]],"
        "\"private_channels\":[{\"id\":\"20\",\"type\":1,\"recipient_ids\":[\"2\"]}]}");
    cJSON_AddItemToObject(ready, "read_state", cJSON_Parse(
        "{\"entries\":[{\"id\":\"11\"},{\"id\":\"20\",\"mention_count\":null},"
        "{\"id\":\"14\",\"mention_count\":3}]}"));
    chat_dispatch("READY", ready);
    cJSON_Delete(ready);
    // Someone passes through voice: remembered, then nobody refers to them any more.
    const long v0 = chat_tree_version();
    cJSON *vs = cJSON_Parse("{\"user_id\":\"7\",\"channel_id\":\"12\",\"guild_id\":\"10\","
                            "\"member\":{\"user\":{\"id\":\"7\",\"username\":\"passerby\"}}}");
    chat_dispatch("VOICE_STATE_UPDATE", vs);
    cJSON_ReplaceItemInObject(vs, "channel_id", cJSON_CreateNull());
    chat_dispatch("VOICE_STATE_UPDATE", vs);
    cJSON_Delete(vs);
    assert(v0 > 0 && chat_tree_version() == v0 + 2); // each change moves the version (/api/chat?v=)
    cJSON *typing = cJSON_Parse("{\"channel_id\":\"11\",\"guild_id\":\"10\",\"user_id\":\"2\"}");
    chat_dispatch("TYPING_START", typing);    // nothing the tree holds: no new version
    chat_dispatch("PRESENCE_UPDATE", typing); // a server member's status: not kept either
    cJSON_Delete(typing);
    assert(chat_tree_version() == v0 + 2);
    char *t = chat_tree_json();
    assert(!strstr(t, "passerby")); // pruned: not a friend, DM recipient or in voice
    char vs_text[48];
    snprintf(vs_text, sizeof vs_text, "\"version\":%ld", v0 + 2);
    assert(strstr(t, vs_text));
    assert(t && strstr(t, "\"name\":\"G\"") && strstr(t, "general"));
    assert(strstr(t, "\"rooms\":[{\"id\":\"12\"")); // voice channels become rooms
    assert(strstr(t, "\"global_name\":\"Bob\"")); // DM recipients land in users
    assert(!strstr(t, "staff"));                       // denied VIEW_CHANNEL: hidden
    assert(strstr(t, "\"club\""));                    // a role of ours allows it back
    assert(strstr(strstr(t, "\"news\""), "\"can_send\":false")); // read-only
    assert(strstr(strstr(t, "\"general\""), "\"can_send\":true"));
    cJSON *decoded = cJSON_Parse(t);
    cJSON *reads = cJSON_GetObjectItem(decoded, "reads");
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetObjectItem(reads, "11"), "mentions")) == 0);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetObjectItem(reads, "20"), "mentions")) == 0);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetObjectItem(reads, "14"), "mentions")) == 3);
    cJSON_Delete(decoded);
    free(t);

    cJSON *m = cJSON_Parse("{\"id\":\"30\",\"channel_id\":\"11\",\"content\":\"hi\","
                           "\"author\":{\"id\":\"2\",\"username\":\"bob\",\"email\":\"x\"}}");
    chat_dispatch("MESSAGE_CREATE", m);
    cJSON_Delete(m);
    char *e = chat_events_json(0, 0);
    assert(e && strstr(e, "\"next\":1") && strstr(e, "\"content\":\"hi\"") && !strstr(e, "email"));
    free(e);
    e = chat_events_json(1, 0);
    assert(e && strstr(e, "\"events\":[]"));
    free(e);
    time_t before = time(NULL);
    e = chat_events_json(1, 1000); // nothing new: the long poll waits, then answers empty
    assert(e && strstr(e, "\"events\":[]") && time(NULL) - before >= 1);
    free(e);
    e = chat_events_json(99, 5000); // past the end (core restarted): answers at once
    assert(e && time(NULL) - before <= 3);
    free(e);
    t = chat_tree_json();
    assert(strstr(t, "\"last_message_id\":\"30\"")); // the channel moved to the top
    free(t);

    cJSON *g = cJSON_Parse("{\"id\":\"40\",\"name\":\"New\",\"owner_id\":\"1\",\"members\":[{\"user\":{\"id\":\"1\"},"
                           "\"roles\":[]}],\"channels\":[{\"id\":\"41\",\"name\":\"lobby\",\"type\":0}]}");
    chat_dispatch("GUILD_CREATE", g);
    t = chat_tree_json();
    assert(strstr(t, "\"name\":\"New\"") && strstr(t, "lobby")); // a server created live shows up
    free(t);
    cJSON *ch = cJSON_Parse("{\"id\":\"42\",\"guild_id\":\"40\",\"name\":\"fresh\",\"type\":0}");
    chat_dispatch("CHANNEL_CREATE", ch);
    t = chat_tree_json();
    assert(strstr(t, "fresh")); // a channel made while running shows up
    free(t);
    chat_dispatch("CHANNEL_DELETE", ch);
    cJSON_Delete(ch);
    t = chat_tree_json();
    assert(!strstr(t, "fresh"));
    free(t);
    chat_dispatch("GUILD_DELETE", g);
    cJSON_Delete(g);
    t = chat_tree_json();
    assert(!strstr(t, "lobby"));
    free(t);
}

// The "notify" the newest event got (chat.c notify_kind).
static int last_notify(const char *message)
{
    cJSON *m = cJSON_Parse(message);
    chat_dispatch("MESSAGE_CREATE", m);
    cJSON_Delete(m);
    char *e = chat_events_json(0, 0);
    cJSON *j = cJSON_Parse(e), *ev = cJSON_GetObjectItem(j, "events");
    int n = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetArrayItem(ev, cJSON_GetArraySize(ev) - 1), "notify"));
    cJSON_Delete(j);
    free(e);
    return n;
}

// Discord's notification settings, then the app's: 0 nothing, 1 server, 2 mention, 3 DM.
static void test_notify(void)
{
    cJSON *ready = cJSON_Parse(
        "{\"user\":{\"id\":\"1\",\"username\":\"me\"},"
        "\"guilds\":[{\"id\":\"10\",\"properties\":{\"name\":\"G\",\"owner_id\":\"1\","
        "\"default_message_notifications\":1},\"roles\":[{\"id\":\"10\",\"permissions\":\"3072\"}],"
        "\"channels\":[{\"id\":\"11\",\"name\":\"general\",\"type\":0},"
        "{\"id\":\"15\",\"name\":\"club\",\"type\":0}]}],"
        "\"private_channels\":[{\"id\":\"20\",\"type\":1,\"recipient_ids\":[\"2\"]},"
        "{\"id\":\"21\",\"type\":1,\"recipient_ids\":[\"2\"]}],"
        "\"user_guild_settings\":{\"entries\":["
        "{\"guild_id\":\"10\",\"muted\":false,\"message_notifications\":3,\"suppress_everyone\":true,"
        "\"channel_overrides\":[{\"channel_id\":\"11\",\"muted\":false,\"message_notifications\":0},"
        "{\"channel_id\":\"15\",\"muted\":true,\"mute_config\":{\"end_time\":\"2001-01-01T00:00:00+00:00\"},"
        "\"message_notifications\":3}]},"
        "{\"guild_id\":null,\"channel_overrides\":[{\"channel_id\":\"20\",\"muted\":true,\"mute_config\":null}]}]}}");
    chat_dispatch("READY", ready);
    cJSON_Delete(ready);
    const char *bob = "\"author\":{\"id\":\"2\",\"username\":\"bob\"}";
    char m[512];
    snprintf(m, sizeof m, "{\"id\":\"50\",\"type\":0,\"guild_id\":\"10\",\"channel_id\":\"11\",\"content\":\"a\",%s}", bob);
    assert(last_notify(m) == 1); // the channel says all messages; the app follows Discord
    snprintf(m, sizeof m, "{\"id\":\"51\",\"type\":0,\"guild_id\":\"10\",\"channel_id\":\"15\",\"content\":\"b\",%s}", bob);
    assert(last_notify(m) == 0); // its mute ran out; the server's default is mentions only
    snprintf(m, sizeof m, "{\"id\":\"52\",\"type\":0,\"guild_id\":\"10\",\"channel_id\":\"15\",\"content\":\"<@1>\","
             "\"mentions\":[{\"id\":\"1\"}],%s}", bob);
    assert(last_notify(m) == 2);
    snprintf(m, sizeof m, "{\"id\":\"53\",\"type\":0,\"guild_id\":\"10\",\"channel_id\":\"15\",\"content\":\"@everyone\","
             "\"mention_everyone\":true,%s}", bob);
    assert(last_notify(m) == 0); // @everyone suppressed for this server
    snprintf(m, sizeof m, "{\"id\":\"54\",\"type\":7,\"guild_id\":\"10\",\"channel_id\":\"11\",\"content\":\"\",%s}", bob);
    assert(last_notify(m) == 0); // a join, not a message
    snprintf(m, sizeof m, "{\"id\":\"55\",\"type\":0,\"channel_id\":\"20\",\"content\":\"c\",%s}", bob);
    assert(last_notify(m) == 0); // a muted DM
    snprintf(m, sizeof m, "{\"id\":\"56\",\"type\":0,\"channel_id\":\"21\",\"content\":\"d\",%s}", bob);
    assert(last_notify(m) == 3);
}

static cJSON *parse_free(char *s)
{
    cJSON *j = cJSON_Parse(s);
    free(s);
    assert(j);
    return j;
}

static void test_gcm(void)
{
    // Expected packet from an independent AES-GCM (python cryptography): key 00..1f,
    // counter 0x01020304, nonce = counter then 8 zero bytes, AAD = the 12-byte header.
    uint8_t key[32];
    for (int i = 0; i < 32; i++)
        key[i] = (uint8_t)i;
    const uint8_t want[47] = {
        0x80, 0x78, 0x00, 0x2a, 0x00, 0x00, 0x03, 0xe8, 0xde, 0xad, 0xbe, 0xef, // header (clear)
        0x4e, 0x25, 0xa8, 0x2e, 0x37, 0xf7, 0x0a, 0x10, 0x96, 0x82, 0xb7, 0x8b, 0x81, 0xf2, 0x84, // ciphertext
        0xed, 0xbe, 0xa5, 0x34, 0xe2, 0x79, 0xd0, 0xcb, 0xe0, 0xe6, 0x7c, 0xbe, 0x90, 0xc5, 0xe8, 0x5f, // ... tag
        0x01, 0x02, 0x03, 0x04}; // counter, big-endian
    const uint8_t plain[27] = {0x80, 0x78, 0x00, 0x2a, 0x00, 0x00, 0x03, 0xe8, 0xde, 0xad, 0xbe, 0xef,
                               'o', 'p', 'u', 's', ' ', 'f', 'r', 'a', 'm', 'e', ' ', '0', '1', '2', '3'};
    uint8_t sealed[64], opened[64];
    assert(voice_gcm_seal(key, 0x01020304, plain, sizeof plain, sealed, sizeof sealed) == sizeof want);
    assert(memcmp(sealed, want, sizeof want) == 0);
    assert(voice_gcm_open(key, sealed, sizeof want, opened, sizeof opened) == sizeof plain);
    assert(memcmp(opened, plain, sizeof plain) == 0);
    sealed[13] ^= 1; // a flipped ciphertext bit: the tag fails
    assert(voice_gcm_open(key, sealed, sizeof want, opened, sizeof opened) == 0);
    sealed[13] ^= 1;
    sealed[sizeof want - 1] ^= 1; // a flipped counter changes the nonce: the tag fails
    assert(voice_gcm_open(key, sealed, sizeof want, opened, sizeof opened) == 0);

    // CSRCs and the extension preamble are part of the clear header (rtpsize).
    uint8_t csrc[] = {0x82, 0x78, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 'x'};
    assert(voice_rtp_header_len(csrc, sizeof csrc) == 20);
    uint8_t ext[] = {0x90, 0x78, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0xbe, 0xde, 0, 1, 0, 0, 0, 0, 'x'};
    assert(voice_rtp_header_len(ext, sizeof ext) == 16); // the body is encrypted, not clear
    assert(voice_rtp_header_len(ext, 15) == 0);  // preamble cut short
    assert(voice_rtp_header_len(ext, 8) == 0);
}

static void test_ip_discovery(void)
{
    uint8_t req[74], rep[74] = {0};
    assert(voice_ip_discovery_request(req, 0x01020304) == 74);
    assert(req[0] == 0 && req[1] == 1 && req[2] == 0 && req[3] == 70 && req[4] == 1 && req[7] == 4);
    rep[1] = 2;
    rep[3] = 70;
    memcpy(rep + 8, "203.0.113.7", 11);
    rep[72] = 0xc3;
    rep[73] = 0x50; // port 50000
    char addr[64];
    uint16_t port = 0;
    assert(voice_ip_discovery_parse(rep, 74, addr, &port) == 0 && strcmp(addr, "203.0.113.7") == 0 && port == 50000);
    rep[1] = 1;
    assert(voice_ip_discovery_parse(rep, 74, addr, &port) != 0); // a request, not a reply
    rep[1] = 2;
    memset(rep + 8, 'x', 64);
    assert(voice_ip_discovery_parse(rep, 74, addr, &port) != 0); // no terminating NUL
    assert(voice_ip_discovery_parse(rep, 73, addr, &port) != 0);
}

static void test_voice_json(void)
{
    cJSON *j = parse_free(voice_json_identify("41", "2", "sess", "tok", 1));
    cJSON *d = cJSON_GetObjectItem(j, "d");
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(j, "op")) == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(d, "server_id")), "41") == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(d, "session_id")), "sess") == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(d, "token")), "tok") == 0);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(d, "max_dave_protocol_version")) == 1);
    cJSON_Delete(j);

    j = parse_free(voice_json_heartbeat(123456, 9));
    d = cJSON_GetObjectItem(j, "d");
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(j, "op")) == 3);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(d, "t")) == 123456);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(d, "seq_ack")) == 9);
    cJSON_Delete(j);

    j = parse_free(voice_json_select_udp("203.0.113.7", 50000, VOICE_MODE_GCM));
    d = cJSON_GetObjectItem(j, "d");
    cJSON *data = cJSON_GetObjectItem(d, "data");
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(j, "op")) == 1);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(d, "protocol")), "udp") == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(data, "address")), "203.0.113.7") == 0);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(data, "port")) == 50000);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(data, "mode")), "aead_aes256_gcm_rtpsize") == 0);
    cJSON_Delete(j);

    j = parse_free(voice_json_resume("41", "sess", "tok", 7));
    d = cJSON_GetObjectItem(j, "d");
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(j, "op")) == 7);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(d, "server_id")), "41") == 0);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(d, "seq_ack")) == 7);
    cJSON_Delete(j);

    j = parse_free(voice_json_transition(23, 0x1234));
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(j, "op")) == 23);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "d"), "transition_id")) == 0x1234);
    cJSON_Delete(j);
}

static void test_bin_frame(void)
{
    const uint8_t frame[] = {0x00, 0x2a, 25, 'a', 'b', 'c'};
    voice_bin b;
    assert(voice_bin_parse(frame, sizeof frame, &b) == 0);
    assert(b.seq == 42 && b.op == 25 && b.len == 3 && memcmp(b.payload, "abc", 3) == 0);
    assert(voice_bin_parse(frame, 2, &b) != 0);
    const uint8_t wrap[] = {0xff, 0xff, 30};
    assert(voice_bin_parse(wrap, sizeof wrap, &b) == 0 && b.seq == 65535 && b.op == 30 && b.len == 0);
    uint8_t out[8];
    assert(voice_bin_build(out, sizeof out, 26, (const uint8_t *)"kp", 2) == 3 && out[0] == 26 && out[1] == 'k');
    assert(voice_bin_build(out, 2, 26, (const uint8_t *)"kp", 2) == 0);
}

// The voice state machine without a socket: a join, the server's confirmation (no server
// details yet, so no thread starts), a leave. Runs before any thread exists.
static void test_voice_state(void)
{
    voice_op4 op;
    cJSON *j = parse_free(voice_state_json());
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "state")), "idle") == 0);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(j, "mute")) && cJSON_IsFalse(cJSON_GetObjectItem(j, "deaf")));
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(j, "since_ms")) == 0);
    cJSON_Delete(j);
    voice_refuse("Not in a call.");
    j = parse_free(voice_state_json());
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "refused")), "Not in a call.") == 0);
    cJSON_Delete(j);
    voice_refuse("");
    assert(voice_set_flags(0, -1) == -1); // not in a call
    assert(voice_take_request(&op) == 0);

    voice_set_self("1");
    voice_join(NULL, "55"); // a DM call
    assert(voice_take_request(&op) == 1 && strcmp(op.channel, "55") == 0 && op.guild[0] == 0 && op.mute == 1);
    assert(voice_take_request(&op) == 0); // sent once
    j = parse_free(voice_state_json());
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "state")), "joining") == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "channel")), "55") == 0);
    cJSON_Delete(j);

    cJSON *vs = cJSON_Parse("{\"user_id\":\"9\",\"channel_id\":\"77\",\"session_id\":\"x\"}");
    voice_on_state(vs); // someone else: ignored
    cJSON_Delete(vs);
    vs = cJSON_Parse("{\"user_id\":\"1\",\"channel_id\":\"55\",\"session_id\":\"abc\",\"guild_id\":null}");
    voice_on_state(vs); // our state: joining until the server details arrive
    cJSON_Delete(vs);
    j = parse_free(voice_state_json());
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "state")), "joining") == 0);
    cJSON_Delete(j);

    assert(voice_set_flags(0, 1) == 0);
    assert(voice_take_request(&op) == 1 && strcmp(op.channel, "55") == 0 && op.mute == 0 && op.deaf == 1);
    assert(voice_set_flags(1, 0) == 0);
    voice_take_request(&op);

    voice_leave();
    assert(voice_take_request(&op) == 1 && op.channel[0] == 0); // leave: op 4 with null channel
    j = parse_free(voice_state_json());
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "state")), "idle") == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "channel")), "") == 0);
    cJSON_Delete(j);

    // A fresh gateway session leaves only the ghost our own previous run left, and
    // rejoins the wanted call. The same account in a call elsewhere is left alone.
    char dir[] = "/tmp/voice-ready-XXXXXX";
    assert(mkdtemp(dir));
    voice_init(dir);
    cJSON *ready = cJSON_Parse("{\"session_id\":\"A\",\"guilds\":[]}");
    voice_on_ready(ready); // first run: nothing to leave
    cJSON_Delete(ready);
    assert(voice_take_request(&op) == 0);
    ready = cJSON_Parse("{\"session_id\":\"B\",\"guilds\":[{\"id\":\"9\",\"voice_states\":"
                        "[{\"user_id\":\"1\",\"session_id\":\"X\",\"channel_id\":\"56\"}]}]}");
    voice_on_ready(ready); // in voice on another device: no leave
    cJSON_Delete(ready);
    assert(voice_take_request(&op) == 0);
    ready = cJSON_Parse("{\"session_id\":\"C\",\"guilds\":[{\"id\":\"9\",\"voice_states\":"
                        "[{\"user_id\":\"1\",\"session_id\":\"B\",\"channel_id\":\"56\"}]}]}");
    voice_on_ready(ready); // our previous run's ghost: leave it
    cJSON_Delete(ready);
    assert(voice_take_request(&op) == 1 && op.channel[0] == 0 && strcmp(op.guild, "9") == 0);
    voice_join("9", "56");
    voice_take_request(&op);
    ready = cJSON_Parse("{\"session_id\":\"D\",\"guilds\":[]}");
    voice_on_ready(ready); // reconnected mid-call: rejoin
    cJSON_Delete(ready);
    assert(voice_take_request(&op) == 1 && strcmp(op.channel, "56") == 0 && strcmp(op.guild, "9") == 0);
    char file[64];
    snprintf(file, sizeof file, "%s/voice_session", dir);
    remove(file);
    rmdir(dir);
    j = parse_free(voice_state_json());
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "state")), "joining") == 0);
    cJSON_Delete(j);
    voice_leave();
    voice_take_request(&op);
}

static void test_calls(void)
{
    cJSON *c = cJSON_Parse("{\"channel_id\":\"20\",\"message_id\":\"77\",\"ringing\":[\"2\"],"
                           "\"voice_states\":[{\"user_id\":\"2\",\"channel_id\":\"20\",\"self_deaf\":true}]}");
    chat_dispatch("CALL_CREATE", c);
    cJSON_Delete(c);
    char *t = chat_tree_json();
    assert(strstr(t, "\"calls\":{\"20\":{\"ringing\":[\"2\"],\"message_id\":\"77\",\"caller\":\"\"}}"));
    assert(strstr(t, "\"self_deaf\":true"));  // the voice state keeps deafen for the app
    free(t);
    assert(chat_call_active("20") && !chat_call_active("99"));
    char me[32];
    assert(chat_me_id(me, sizeof me) && strcmp(me, "1") == 0);
    char *e = chat_events_json(0, 0);
    assert(e && strstr(e, "\"t\":\"CALL\"") && strstr(e, "\"channel\":\"20\""));
    free(e);

    // The ring message's author becomes the caller, and a re-sent call keeps it.
    c = cJSON_Parse("{\"id\":\"77\",\"channel_id\":\"20\",\"type\":3,\"content\":\"\",\"author\":{\"id\":\"2\"}}");
    chat_dispatch("MESSAGE_CREATE", c);
    cJSON_Delete(c);
    c = cJSON_Parse("{\"channel_id\":\"20\",\"message_id\":\"77\",\"ringing\":[]}");
    chat_dispatch("CALL_UPDATE", c);
    cJSON_Delete(c);
    t = chat_tree_json();
    assert(strstr(t, "\"calls\":{\"20\":{\"ringing\":[],\"message_id\":\"77\",\"caller\":\"2\"}}"));
    free(t);

    c = cJSON_Parse("{\"channel_id\":\"20\"}");
    chat_dispatch("CALL_DELETE", c);
    cJSON_Delete(c);
    t = chat_tree_json();
    assert(strstr(t, "\"calls\":{}") && !chat_call_active("20"));
    free(t);
}

static void test_ratelimit(void)
{
    char limited[] = "{\"message\":\"You are being rate limited.\",\"retry_after\":0.75,\"global\":false}";
    char too_long[] = "{\"retry_after\":30}";
    char ok[] = "{\"id\":\"1\"}";
    http_response a = {.status = 429, .body = limited}, b = {.status = 429, .body = too_long},
                  c = {.status = 200, .body = ok}, d = {.status = 429, .body = ok};
    assert(ratelimit_retry_after(&a, 5.0) == 0.75); // retried after Discord's wait
    assert(ratelimit_retry_after(&b, 5.0) == 0);    // too long to hold the request
    assert(ratelimit_retry_after(&c, 5.0) == 0);    // not limited
    assert(ratelimit_retry_after(&d, 5.0) == 0);    // limited without a wait: not retried
    assert(ratelimit_retry_after(NULL, 5.0) == 0);
}

// Voice audio: Opus, VAD, RTP (with the extension and padding), the transport packet, the
// playback queue and mixer, the loopback rule of the audio socket.
static void test_audio(void)
{
    OpusEncoder *e = vau_encoder_new();
    OpusDecoder *d = vau_decoder_new();
    assert(e && d);
    // A square tone, ten frames. The decoded energy of the later frames must be near the input.
    int16_t pcm[VAU_FRAME], out[2 * VAU_FRAME];
    uint8_t opus[VAU_MAX_PKT];
    double in_e = 0, out_e = 0;
    for (int f = 0; f < 10; f++) {
        for (int i = 0; i < VAU_FRAME; i++)
            pcm[i] = (f * VAU_FRAME + i) % 100 < 50 ? 6000 : -6000;
        int n = opus_encode(e, pcm, VAU_FRAME, opus, sizeof opus);
        assert(n > 0 && (f < 5 || n < 200)); // 64 kbps is about 160 bytes per 20 ms once warmed up
        assert(opus_decode(d, opus, n, out, VAU_FRAME, 0) == VAU_FRAME);
        if (f >= 5)
            for (int i = 0; i < VAU_FRAME; i++) {
                in_e += (double)pcm[i] * pcm[i];
                out_e += (double)out[2 * i] * out[2 * i];
            }
    }
    double ratio = out_e / in_e;
    assert(ratio > 0.5 && ratio < 2.0);
    opus_encoder_destroy(e);
    opus_decoder_destroy(d);

    // VAD: speech starts it (soft speech at the controller's mic too), and it holds for
    // 800 ms after the last speech frame so it does not drop out between words.
    vau_vad v = {0};
    int16_t quiet[VAU_FRAME] = {0}, loud[VAU_FRAME], soft[VAU_FRAME];
    for (int i = 0; i < VAU_FRAME; i++) {
        loud[i] = i % 100 < 50 ? 6000 : -6000;
        soft[i] = i % 100 < 50 ? 200 : -200;
    }
    assert(vau_vad_frame(&v, quiet, VAU_FRAME) == 0);
    assert(vau_vad_frame(&v, loud, VAU_FRAME) == 1);
    for (int i = 0; i < 39; i++)
        assert(vau_vad_frame(&v, quiet, VAU_FRAME) == 1);
    assert(vau_vad_frame(&v, quiet, VAU_FRAME) == 0);
    assert(vau_vad_frame(&v, soft, VAU_FRAME) == 1);

    // RTP: a plain header, then one with CSRC-less extension (body 4 bytes) and 2 padding octets.
    uint8_t h[12];
    vau_rtp_header(h, 0xfffe, 0x01020304, 0xaabbccdd);
    vau_rtp r;
    assert(vau_rtp_parse(h, 12, &r) == 0 && r.seq == 0xfffe && r.ts == 0x01020304 && r.ssrc == 0xaabbccdd);
    assert(r.off == 12 && r.len == 0);
    uint8_t ext[] = {0xb0, 0x78, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0xbe, 0xde, 0, 1, 0x10, 0x20, 0x30, 0x40,
                     'a', 'b', 'c', 0, 2};
    assert(vau_rtp_parse(ext, sizeof ext, &r) == 0 && r.off == 20 && r.len == 3 && ext[r.off] == 'a');
    ext[1] = 0x79; // not Opus
    assert(vau_rtp_parse(ext, sizeof ext, &r) == -1);
    assert(vau_rtp_parse(h, 11, &r) == -1);

    // Transport: a sealed packet with an extension opens to its Opus payload (ext stripped).
    uint8_t key[32];
    for (int i = 0; i < 32; i++)
        key[i] = (uint8_t)(i * 7 + 3);
    uint8_t sealed[64], got[64];
    ext[1] = 0x78;
    size_t sn = voice_gcm_seal(key, 7, ext, sizeof ext, sealed, sizeof sealed);
    assert(sn == sizeof ext + 20);
    uint32_t ssrc = 0;
    assert(vau_open_packet(NULL, key, sealed, sn, got, sizeof got, &ssrc) == 3 && memcmp(got, "abc", 3) == 0);
    assert(ssrc == 2); // the ssrc bytes of the ext packet above
    key[0] ^= 1;
    assert(vau_open_packet(NULL, key, sealed, sn, got, sizeof got, &ssrc) == 0);
    key[0] ^= 1;
    // The silence frame is never decrypted, and our own send path packs the same way.
    uint8_t silence[3] = {0xf8, 0xff, 0xfe};
    size_t built = vau_build_packet(NULL, key, 9, 1, 960, 3, silence, 3, sealed, sizeof sealed);
    assert(built == 12 + 3 + 20);
    assert(vau_open_packet(NULL, key, sealed, built, got, sizeof got, &ssrc) == 3 && ssrc == 9);

    // Playback: two frames to start, the oldest drops when full; the mixer adds and clamps.
    vau_fifo *f = calloc(1, sizeof *f);
    assert(f);
    int16_t one[2 * VAU_FRAME], two[2 * VAU_FRAME], mixed[2 * VAU_FRAME];
    for (int i = 0; i < 2 * VAU_FRAME; i++)
        one[i] = 1000, two[i] = -300;
    vau_fifo_push(f, one);
    assert(vau_fifo_pop(f, mixed) == 0); // not primed yet
    vau_fifo_push(f, one);
    assert(vau_fifo_pop(f, mixed) == 1 && mixed[0] == 1000);
    for (int i = 0; i < VAU_FIFO + 2; i++)
        vau_fifo_push(f, two);
    assert(f->count == VAU_FIFO);
    free(f);
    int32_t acc[2 * VAU_FRAME] = {0};
    vau_mix_add(acc, one);
    vau_mix_add(acc, one);
    vau_mix_add(acc, two);
    vau_mix_out(acc, mixed);
    assert(mixed[0] == 1700);
    for (int i = 0; i < 2 * VAU_FRAME; i++)
        acc[i] = i % 2 ? 40000 : -40000;
    vau_mix_out(acc, mixed);
    assert(mixed[0] == -32768 && mixed[1] == 32767);

    // The audio socket serves loopback only.
    assert(vau_loopback(htonl(0x7f000001)));
    assert(!vau_loopback(htonl(0x0a000001)));

    cJSON *j = parse_free(voice_json_speaking(1, 42));
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(j, "op")) == 5);
    cJSON *dd = cJSON_GetObjectItem(j, "d");
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(dd, "speaking")) == 1);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(dd, "ssrc")) == 42);
    cJSON_Delete(j);
}

// The gateway's reading of close codes and its wait between connections (gateway_run).
static void test_gateway_rules(void)
{
    assert(gateway_close_action(4004) == GATEWAY_STOP); // signed out
    for (int code = 4010; code <= 4014; code++)
        assert(gateway_close_action(code) == GATEWAY_STOP);
    assert(gateway_close_action(4007) == GATEWAY_FRESH && gateway_close_action(4009) == GATEWAY_FRESH);
    assert(gateway_close_action(1000) == GATEWAY_KEEP && gateway_close_action(4000) == GATEWAY_KEEP);
    assert(gateway_close_action(-1) == GATEWAY_KEEP); // dropped: resume
    const int waits[] = {1, 2, 4, 8, 15, 15};
    for (int i = 1; i < 6; i++)
        assert(gateway_next_wait(waits[i - 1]) == waits[i]); // never above 15 s
}

static char *dup_text(const char *s, size_t n)
{
    char *b = malloc(n + 1);
    memcpy(b, s, n);
    b[n] = 0;
    return b;
}

// HTTP/1.0 replies as exchange() collects them (reads until close, in any pieces).
static void test_http_parse(void)
{
    const char ok[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"a\":\"x\\r\\n\\r\\ny\"}";
    http_response r = http_parse_reply(dup_text(ok, sizeof ok - 1), sizeof ok - 1);
    assert(r.status == 200 && r.body && strcmp(r.body, "{\"a\":\"x\\r\\n\\r\\ny\"}") == 0);
    free(r.body);
    const char bin[] = "HTTP/1.0 200 OK\r\n\r\n\x89PNG\0\0\x01"; // a picture: body_len, not strlen
    r = http_parse_reply(dup_text(bin, sizeof bin - 1), sizeof bin - 1);
    assert(r.status == 200 && r.body_len == 7 && memcmp(r.body, "\x89PNG\0\0\x01", 7) == 0);
    free(r.body);
    const char empty[] = "HTTP/1.1 204 No Content\r\n\r\n";
    r = http_parse_reply(dup_text(empty, sizeof empty - 1), sizeof empty - 1);
    assert(r.status == 204 && r.body_len == 0 && r.body && !r.body[0]);
    free(r.body);
    const char cut[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"; // the connection ended in the head
    r = http_parse_reply(dup_text(cut, sizeof cut - 1), sizeof cut - 1);
    assert(r.status == -1 && !r.body);
    const char junk[] = "<html>no status line</html>\r\n\r\n";
    r = http_parse_reply(dup_text(junk, sizeof junk - 1), sizeof junk - 1);
    assert(r.status == -1 && !r.body);
}

struct rtp_seen {
    size_t count, bytes;
    int markers, fu_starts, fu_ends;
    uint8_t first[4];
};

static void seen_payload(void *user, const uint8_t *p, size_t len, int marker)
{
    struct rtp_seen *s = user;
    assert(len <= 102); // mtu 100, + the FU indicator and header
    if (s->count == 0)
        memcpy(s->first, p, 4);
    s->count++;
    s->bytes += len;
    s->markers += marker;
    if ((p[0] & 0x1f) == 28) {
        s->fu_starts += !!(p[1] & 0x80);
        s->fu_ends += !!(p[1] & 0x40);
        assert((p[1] & 0x1f) == 5); // the fragmented NAL's own type (IDR)
    }
}

// Go Live's packetiser: SPS whole, a 250-byte IDR in three FU-A fragments, one marker.
static void test_h264_packetize(void)
{
    uint8_t au[4 + 8 + 3 + 250];
    size_t n = 0;
    memcpy(au + n, "\0\0\0\1", 4), n += 4;
    memcpy(au + n, "\x67\x42\0\x1f\xaa\xbb\xcc\xdd", 8), n += 8; // SPS (type 7), 4-byte start code
    memcpy(au + n, "\0\0\1", 3), n += 3;                            // 3-byte between slices
    au[n] = 0x65; // IDR (type 5), NRI 3
    for (int i = 1; i < 250; i++)
        au[n + i] = (uint8_t)(i | 1); // no start code inside
    n += 250;
    size_t pos = 0, len;
    const uint8_t *nal = h264_next_nal(au, n, &pos, &len);
    assert(nal == au + 4 && len == 8);
    nal = h264_next_nal(au, n, &pos, &len);
    assert(nal == au + 15 && len == 250);
    assert(!h264_next_nal(au, n, &pos, &len));
    struct rtp_seen s = {0};
    h264_packetize(au, n, 100, seen_payload, &s);
    assert(s.count == 1 + 3 && s.markers == 1 && s.fu_starts == 1 && s.fu_ends == 1);
    assert(s.first[0] == 0x67);                 // the SPS went whole
    assert(s.bytes == 8 + 249 + 3 * 2);         // the IDR's header byte becomes the FU bytes
    struct rtp_seen none = {0};
    h264_packetize((const uint8_t *)"\1\2\3", 3, 100, seen_payload, &none); // no start code: nothing
    assert(none.count == 0);
}

int main(void)
{
    test_gateway_rules();
    test_http_parse();
    test_h264_packetize();
    test_chat();
    test_notify();
    test_ratelimit();
    test_ws_frame();
    test_zstream();
    test_gcm();
    test_ip_discovery();
    test_voice_json();
    test_bin_frame();
    test_voice_state();
    test_calls();
    test_audio();
    puts("selftest ok");
    return 0;
}
