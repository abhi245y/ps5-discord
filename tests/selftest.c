// Host self-checks for the pure parts: websocket framing and zlib-stream reassembly.
// make test
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <zlib.h>

#include <stdlib.h>
#include <unistd.h>

#include <cJSON.h>

#include "../src/chat.h"
#include "../src/gateway.h"
#include "../src/http.h"
#include "../src/overlay.h"
#include "../src/png_circle.h"
#include "../src/ratelimit.h"
#include "../src/resample.h"
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

// Discord's receiver reports about our stream: report blocks, NACK, PLI, REMB, and the
// RTCP transport encryption (8 clear bytes).
static void test_rtcp(void)
{
    const uint32_t ours = 8760;
    uint8_t p[] = {
        0x81, 201, 0, 7, 0, 0, 0, 1,                 // RR, one block, from SSRC 1
        0, 0, 0x22, 0x38, 64, 0xff, 0xff, 0xfe,      // about 8760: fraction 64/256, cumulative -2
        0, 0, 0, 0, 0, 0, 0x01, 0x2c, 0, 0, 0, 0, 0, 0, 0, 0, // jitter 300
        0x81, 205, 0, 3, 0, 0, 0, 1, 0, 0, 0x22, 0x38, // generic NACK for 8760
        0x00, 0x10, 0x00, 0x05,                      // PID 16 and two more (BLP bits 0, 2)
        0x81, 206, 0, 2, 0, 0, 0, 1, 0, 0, 0x22, 0x38, // PLI for 8760
        0x8f, 206, 0, 5, 0, 0, 0, 1, 0, 0, 0, 0, 'R', 'E', 'M', 'B', 1, (2 << 2) | 0, 0x01, 0x00, 0, 0, 0x22, 0x38,
    };
    rtcp_stats st = {0};
    assert(rtcp_parse(p, sizeof p, ours, &st) == 0);
    assert(st.reports == 1 && st.max_fraction == 64 && st.cumulative == -2 && st.max_jitter == 300);
    assert(st.nack_packets == 3 && st.pli == 1 && st.fir == 0 && st.remb_bps == 256 << 2);
    rtcp_stats other = {0};
    assert(rtcp_parse(p, sizeof p, 1234, &other) == 0 && !other.reports && !other.nack_packets && !other.pli);
    assert(rtcp_parse(p, sizeof p - 3, ours, &other) == -1); // truncated
    uint8_t key[32] = {7}, opened[128], plain[16] = {0x80, 201, 0, 1, 0, 0, 0, 9, 1, 2, 3, 4, 5, 6, 7, 8};
    assert(voice_gcm_open_rtcp(key, plain, sizeof plain, opened, sizeof opened) == 0); // no room for tag and counter
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

// The headset's rate conversion: a 1 kHz tone keeps its level from 64 to 48 kHz and from
// 16 to 48 kHz; 12 kHz is removed on the way from 48 to 16 kHz (above its 8 kHz limit).
static double tone_amp(int fin, int up, int down, double f)
{
    static float in[64000], out[64000];
    rs_state r;
    assert(rs_init(&r, up, down) == 0);
    for (int i = 0; i < fin; i++)
        in[i] = 0.5f * (float)sin(2 * M_PI * f * i / fin);
    const int m = rs_process(&r, in, fin, out, 64000), fout = fin * up / down;
    assert(m == fout);
    double s = 0, c = 0;
    for (int i = 200; i < m; i++) {
        s += out[i] * sin(2 * M_PI * f * i / fout);
        c += out[i] * cos(2 * M_PI * f * i / fout);
    }
    return 2 * sqrt(s * s + c * c) / (m - 200);
}

static void test_overlay_line(void)
{
    char line[4096];
    overlay_person p[3] = {{"1", "1_ab", "Ana", 'm'}, {"2", "", "Ki|m,o~", '-'}, {"3", "3_cd", "Mira", 't'}};
    overlay_headset bt = {1, 2, "68:A5:93:5E:57:B2", {"68:A5:93:5E:57:B2", "00:1B:66:00:00:02"}, {"Air;Pods^", "Buds"}};
    overlay_line(line, sizeof line, 'c', 'o', 1, "", 0, "Raid Night", "42:10", "720p", "0.5.5", &bt, p, 3);
    // Talking first; the line's separators in a name become spaces.
    assert(strcmp(line, "3|c|o|1||0|Raid Night|42:10|720p|0.5.5|1~68:A5:93:5E:57:B2~68:A5:93:5E:57:B2^Air Pods ;"
                        "00:1B:66:00:00:02^Buds|3~3_cd~Mira~t,1~1_ab~Ana~m,2~~Ki m o ~-") == 0);
    overlay_headset none = {0};
    overlay_line(line, sizeof line, 'c', 'm', 0, "18:42", 3, "R", "", "", "", &none, p, 0);
    assert(strcmp(line, "3|c|m|0|18:42|3|R||||0~~|") == 0);
    // More than fits: whole people only.
    overlay_person many[40];
    for (int i = 0; i < 40; i++) {
        snprintf(many[i].id, sizeof many[i].id, "1234567890123456789");
        snprintf(many[i].avatar, sizeof many[i].avatar, "1234567890123456789_0123456789abcdef0123456789abcdef");
        snprintf(many[i].name, sizeof many[i].name, "Ünïcödé %02d", i);
        many[i].flag = '-';
    }
    const int n = overlay_line(line, sizeof line, 'c', 'o', 1, "", 0, "A room", "1:00:00", "1080p", "", &none, many, 40);
    assert(n <= 2047 && (int)strlen(line) == n && line[n - 2] == '~' && line[n - 1] == '-');
}

// send.bin: the overlay's typed message, and the JSON body it is posted with.
static void write_outbox_to(const char *path, uint64_t magic, uint64_t seq, uint32_t len, const char *channel,
                            const char *text)
{
    static struct {
        uint64_t signature, seq;
        uint32_t len, reserved;
        char channel[24];
        char text[8192];
    } box;
    assert(sizeof box == 8240); // protocol.hpp Outbox
    memset(&box, 0, sizeof box);
    memcpy(box.channel, channel, strlen(channel) < sizeof box.channel ? strlen(channel) : sizeof box.channel);
    box.signature = magic;
    box.seq = seq;
    box.len = len;
    memcpy(box.text, text, strlen(text) < sizeof box.text ? strlen(text) : sizeof box.text);
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(&box, 1, sizeof box, f) == sizeof box);
    fclose(f);
}

static void write_outbox(const char *path, uint64_t magic, uint64_t seq, uint32_t len, const char *text)
{
    write_outbox_to(path, magic, seq, len, "", text);
}

static char outbox_chan[24];
static uint64_t read_outbox(const char *path, char *text, size_t cap)
{
    return overlay_read_outbox(path, text, cap, outbox_chan, sizeof outbox_chan);
}

static void test_outbox(void)
{
    static char text[8193];
    const char *path = "build/host/send-test.bin";
    const uint64_t magic = 0x31434f504c564f44ULL;
    const char *msg = "caf\xc3\xa9 \xe2\x82\xac \"quoted\" \\ line\nbreak, and more than 48 characters long";
    write_outbox(path, magic, 77, (uint32_t)strlen(msg), msg);
    assert(read_outbox(path, text, sizeof text) == 77 && strcmp(text, msg) == 0);
    assert(read_outbox(path, text, sizeof text) == 77); // the same file again: the same seq (the feed skips it)
    write_outbox(path, magic ^ 1, 78, 3, "abc"); // wrong magic
    assert(read_outbox(path, text, sizeof text) == 0);
    write_outbox(path, magic, 79, 8193, "abc"); // longer than the buffer
    assert(read_outbox(path, text, sizeof text) == 0);
    write_outbox(path, magic, 80, 0, ""); // empty
    assert(read_outbox(path, text, sizeof text) == 0);
    write_outbox(path, magic, 81, 3, "abc");
    assert(read_outbox(path, text, 3) == 0); // no room for the NUL
    memset(text, 0x78, 8192);
    text[8192] = 0;
    write_outbox(path, magic, 82, 8192, text);
    assert(read_outbox(path, text, sizeof text) == 82 && strlen(text) == 8192);
    FILE *f = fopen(path, "wb"); // a short file
    assert(f && fwrite("short", 1, 5, f) == 5);
    fclose(f);
    assert(read_outbox(path, text, sizeof text) == 0);
    remove(path);
    assert(read_outbox(path, text, sizeof text) == 0); // missing
    // The chat's channel: digits, "" for the call's own chat; anything else is not valid (it goes into a URL).
    write_outbox_to(path, magic, 90, 2, "1234567890123456789", "hi");
    assert(read_outbox(path, text, sizeof text) == 90 && strcmp(text, "hi") == 0 && strcmp(outbox_chan, "1234567890123456789") == 0);
    write_outbox_to(path, magic, 91, 2, "", "hi");
    assert(read_outbox(path, text, sizeof text) == 91 && outbox_chan[0] == 0);
    write_outbox_to(path, magic, 92, 2, "12/../34", "hi");
    assert(read_outbox(path, text, sizeof text) == 0);
    write_outbox_to(path, magic, 93, 2, "12 34", "hi");
    assert(read_outbox(path, text, sizeof text) == 0);
    write_outbox_to(path, magic, 94, 2, "123456789012345678901234", "hi"); // 24 digits: no NUL, too long
    assert(read_outbox(path, text, sizeof text) == 0);
    write_outbox_to(path, magic, 95, 2, "12345678901234567890123", "hi"); // 23: the longest
    assert(read_outbox(path, text, sizeof text) == 95);
    write_outbox_to(path, magic, 96, 2, "-1", "hi");
    assert(read_outbox(path, text, sizeof text) == 0);
    remove(path);

    char *json = chat_message_json("123", msg, NULL, NULL, NULL);
    cJSON *j = cJSON_Parse(json);
    assert(j && strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "content")), msg) == 0);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(j, "tts")) && cJSON_GetObjectItem(j, "flags")->valueint == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "mobile_network_type")), "unknown") == 0);
    assert(!cJSON_GetObjectItem(j, "nonce") && !cJSON_GetObjectItem(j, "sticker_ids"));
    assert(strstr(json, "\\\"quoted\\\"") && strstr(json, "\\n") && !strchr(json, '\n')); // escaped, one line
    cJSON_Delete(j);
    free(json);
    json = chat_message_json("123", "hi", "42", "7", "9");
    j = cJSON_Parse(json);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "nonce")), "42") == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(cJSON_GetObjectItem(j, "sticker_ids"), 0)), "7") == 0);
    cJSON *ref = cJSON_GetObjectItem(j, "message_reference");
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(ref, "channel_id")), "123") == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(ref, "message_id")), "9") == 0);
    cJSON_Delete(j);
    free(json);
}

// A 6x6 RGB PNG (Sub and Paeth filtered rows) through png_circle, and its output decoded.
static void test_png_circle(void)
{
    unsigned char raw[6 * 19];
    for (int y = 0; y < 6; y++) {
        raw[y * 19] = y % 2 ? 4 : 1; // Paeth, Sub: the deltas below are what each filter adds
        for (int x = 0; x < 18; x++)
            raw[y * 19 + 1 + x] = x < 3 ? (x == 0 ? 200 : 40) : 0;
    }
    unsigned char z[256];
    z_stream d = {0};
    assert(deflateInit(&d, 9) == Z_OK);
    d.next_in = raw;
    d.avail_in = sizeof raw;
    d.next_out = z;
    d.avail_out = sizeof z;
    assert(deflate(&d, Z_FINISH) == Z_STREAM_END);
    const size_t zn = sizeof z - d.avail_out;
    deflateEnd(&d);
    unsigned char png[512];
    size_t n = 0;
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    memcpy(png, sig, 8);
    n = 8;
    const unsigned char ihdr[13] = {0, 0, 0, 6, 0, 0, 0, 6, 8, 2, 0, 0, 0};
    struct { const char *kind; const unsigned char *data; size_t len; } chunks[] = {
        {"IHDR", ihdr, 13}, {"IDAT", z, zn}, {"IEND", NULL, 0}};
    for (int i = 0; i < 3; i++) {
        png[n++] = 0; png[n++] = 0; png[n++] = (unsigned char)(chunks[i].len >> 8); png[n++] = (unsigned char)chunks[i].len;
        memcpy(png + n, chunks[i].kind, 4);
        if (chunks[i].len)
            memcpy(png + n + 4, chunks[i].data, chunks[i].len);
        const uLong crc = crc32(0, png + n, 4 + (uInt)chunks[i].len);
        n += 4 + chunks[i].len;
        png[n++] = (unsigned char)(crc >> 24); png[n++] = (unsigned char)(crc >> 16);
        png[n++] = (unsigned char)(crc >> 8); png[n++] = (unsigned char)crc;
    }
    unsigned char *px = NULL;
    int w = 0, h = 0;
    assert(png_decode(png, n, &px, &w, &h) == 0 && w == 6 && h == 6);
    assert(px[0] == 200 && px[1] == 40 && px[3] == 255 && px[4] == 200); // Sub: each pixel adds its left one
    free(px);
    char *buf = NULL;
    size_t len = 0;
    FILE *out = open_memstream(&buf, &len);
    assert(png_circle(png, n, 32, out) == 0);
    fclose(out);
    assert(png_decode((unsigned char *)buf, len, &px, &w, &h) == 0 && w == 32 && h == 32);
    assert(px[3] == 0);                          // a corner: outside the circle
    assert(px[(16 * 32 + 16) * 4 + 3] == 255);   // the middle
    free(px);
    free(buf);
}

static void test_resample(void)
{
    assert(fabs(tone_amp(64000, 3, 4, 1000) - 0.5) < 0.01);
    assert(fabs(tone_amp(16000, 3, 1, 1000) - 0.5) < 0.01);
    assert(fabs(tone_amp(48000, 1, 3, 1000) - 0.5) < 0.01);
    assert(tone_amp(48000, 1, 3, 12000) < 0.05);
    rs_state r;
    assert(fabs(tone_amp(8000, 6, 1, 1000) - 0.5) < 0.01);
    assert(rs_init(&r, 7, 1) == -1);
}

// ---- the overlay's Chat and Mentions feed ----
static int lines_of(const char *feed, char kind)
{
    int n = 0;
    for (const char *p = feed; *p; p = strchr(p, '\n') + 1)
        n += *p == kind;
    return n;
}

static void test_overlay_feed(void)
{
    char age[12];
    overlay_age(age, sizeof age, 0);
    assert(strcmp(age, "now") == 0);
    overlay_age(age, sizeof age, -30);
    assert(strcmp(age, "now") == 0);
    overlay_age(age, sizeof age, 59);
    assert(strcmp(age, "now") == 0);
    overlay_age(age, sizeof age, 60);
    assert(strcmp(age, "1m") == 0);
    overlay_age(age, sizeof age, 14 * 60 + 30);
    assert(strcmp(age, "14m") == 0);
    overlay_age(age, sizeof age, 3599);
    assert(strcmp(age, "59m") == 0);
    overlay_age(age, sizeof age, 3600);
    assert(strcmp(age, "1h") == 0);
    overlay_age(age, sizeof age, 3 * 3600 + 5);
    assert(strcmp(age, "3h") == 0);
    overlay_age(age, sizeof age, 86400 * 2 + 5);
    assert(strcmp(age, "2d") == 0);
    char clock[12];
    struct tm tm = {.tm_hour = 18, .tm_min = 14};
    overlay_clock(clock, sizeof clock, &tm);
    assert(strcmp(clock, "6:14 PM") == 0);
    tm = (struct tm){.tm_hour = 0, .tm_min = 5};
    overlay_clock(clock, sizeof clock, &tm);
    assert(strcmp(clock, "12:05 AM") == 0);
    tm = (struct tm){.tm_hour = 12, .tm_min = 0};
    overlay_clock(clock, sizeof clock, &tm);
    assert(strcmp(clock, "12:00 PM") == 0);
    tm = (struct tm){.tm_hour = 9, .tm_min = 7};
    overlay_clock(clock, sizeof clock, &tm);
    assert(strcmp(clock, "9:07 AM") == 0);

    // Mentions: newest first by time, no id twice, ten at most, a DM is one entry.
    static overlay_chat c;
    memset(&c, 0, sizeof c);
    overlay_mention m = {0};
    snprintf(m.id, sizeof m.id, "5");
    snprintf(m.chan, sizeof m.chan, "50");
    m.at = 1000;
    assert(overlay_mention_add(&c, &m) == 1);
    snprintf(m.id, sizeof m.id, "6");
    m.at = 3000;
    assert(overlay_mention_add(&c, &m) == 1);
    snprintf(m.id, sizeof m.id, "7");
    m.at = 2000;
    assert(overlay_mention_add(&c, &m) == 1);
    assert(c.nmen == 3 && !strcmp(c.men[0].id, "6") && !strcmp(c.men[1].id, "7") && !strcmp(c.men[2].id, "5"));
    assert(overlay_mention_add(&c, &m) == 0 && c.nmen == 3); // the same id
    for (int i = 0; i < 12; i++) {
        snprintf(m.id, sizeof m.id, "1%02d", i);
        m.at = 5000 + i;
        overlay_mention_add(&c, &m);
    }
    assert(c.nmen == OVERLAY_MENTIONS && !strcmp(c.men[0].id, "111") && !strcmp(c.men[9].id, "102"));
    snprintf(m.id, sizeof m.id, "9");
    m.at = 10; // older than the ten kept
    assert(overlay_mention_add(&c, &m) == 0 && c.nmen == OVERLAY_MENTIONS);
    memset(&c, 0, sizeof c);
    m.dm = 1;
    snprintf(m.chan, sizeof m.chan, "20");
    snprintf(m.id, sizeof m.id, "21");
    m.at = 100;
    overlay_mention_add(&c, &m);
    snprintf(m.id, sizeof m.id, "22");
    m.at = 200;
    assert(overlay_mention_add(&c, &m) == 1 && c.nmen == 1 && !strcmp(c.men[0].id, "22"));
    m.dm = 0;
    snprintf(m.id, sizeof m.id, "23");
    m.at = 150; // a channel mention is its own entry
    overlay_mention_add(&c, &m);
    assert(c.nmen == 2 && !strcmp(c.men[0].id, "22") && !strcmp(c.men[1].id, "23"));

    // Messages: no id twice, twenty at most (the oldest go), edit and delete by id.
    memset(&c, 0, sizeof c);
    overlay_msg om = {0};
    for (int i = 1; i <= 25; i++) {
        snprintf(om.id, sizeof om.id, "%d", i);
        snprintf(om.text, sizeof om.text, "text %d", i);
        assert(overlay_msg_add(&c, &om) == 1);
    }
    assert(c.nmsg == OVERLAY_MSGS && !strcmp(c.msgs[0].id, "6") && !strcmp(c.msgs[19].id, "25"));
    assert(overlay_msg_add(&c, &om) == 0 && c.nmsg == OVERLAY_MSGS);
    assert(overlay_msg_edit(&c, "10", "edited") && !strcmp(c.msgs[4].text, "edited") && !overlay_msg_edit(&c, "1", "x"));
    assert(overlay_msg_remove(&c, "10") && c.nmsg == 19 && !strcmp(c.msgs[4].id, "11") && !overlay_msg_remove(&c, "10"));

    // The feed: separators in user text become spaces, fields are cut, the record layout holds.
    static char feed[OVERLAY_FEED_MAX];
    memset(&c, 0, sizeof c);
    c.kind = 'c';
    snprintf(c.chan, sizeof c.chan, "555");
    snprintf(c.title, sizeof c.title, "#raid\tnight");
    snprintf(c.sub, sizeof c.sub, "Night Shift");
    c.unseen = 2;
    c.rev = 7;
    snprintf(c.focus, sizeof c.focus, "902");
    snprintf(om.id, sizeof om.id, "901");
    snprintf(om.avatar, sizeof om.avatar, "11_ab");
    snprintf(om.name, sizeof om.name, "Mi\nra|~,");
    snprintf(om.time, sizeof om.time, "6:14 PM");
    om.mine = 1;
    char big[400];
    memset(big, 'x', sizeof big);
    big[0] = 'a';
    big[sizeof big - 1] = 0;
    memcpy(om.text, "a\tb\r\nc, d | e ", 14);
    memcpy(om.text + 14, big, sizeof om.text - 15);
    om.text[sizeof om.text - 1] = 0;
    memset(&c.msgs[0], 0, sizeof c.msgs[0]);
    c.msgs[0] = om;
    c.nmsg = 1;
    overlay_mention m2 = {0};
    snprintf(m2.id, sizeof m2.id, "903");
    snprintf(m2.chan, sizeof m2.chan, "555");
    snprintf(m2.place, sizeof m2.place, "Night Shift \xc2\xb7 #raid-night");
    snprintf(m2.name, sizeof m2.name, "Kenji");
    snprintf(m2.text, sizeof m2.text, "you coming?");
    m2.at = 1000;
    c.men[0] = m2;
    c.nmen = 1;
    int n = overlay_chat_feed(feed, sizeof feed, &c, 1000 + 14 * 60);
    assert(n == (int)strlen(feed) && feed[n - 1] == '\n');
    const char *start = "H\tc\t555\t#raid night\tNight Shift\t\t2\t902\t7\nM\t901\t11_ab\tMi ra|~,\t6:14 PM\t1\ta b  c, d | e axxx";
    assert(!strncmp(feed, start, strlen(start)));
    assert(lines_of(feed, 'H') == 1 && lines_of(feed, 'M') == 1 && lines_of(feed, 'N') == 1);
    const char *line = feed;
    for (int i = 0; i < 3; i++, line = strchr(line, '\n') + 1) { // every record: its tabs, nothing else
        int tabs = 0;
        for (const char *q = line; *q != '\n'; q++)
            tabs += *q == '\t';
        assert(tabs == (*line == 'H' ? 8 : *line == 'M' ? 6 : 7));
    }
    const char *mline = strstr(feed, "\nM\t") + 1;
    const char *text = mline + strlen("M\t901\t11_ab\tMi ra|~,\t6:14 PM\t1\t");
    assert((int)strcspn(text, "\n") == 120);
    assert(strstr(feed, "\nN\t903\t555\tNight Shift \xc2\xb7 #raid-night\t\tKenji\t14m\tyou coming?\n"));
    // Text is cut between characters, never inside one.
    snprintf(om.text, sizeof om.text, "abc"); // then two-byte characters: the 120th byte is inside one
    for (int i = 0; i < 70; i++)
        strcat(om.text, "\xc3\xa9");
    c.msgs[0] = om;
    n = overlay_chat_feed(feed, sizeof feed, &c, 1000);
    mline = strstr(feed, "\nM\t") + 1;
    text = mline + strlen("M\t901\t11_ab\tMi ra|~,\t6:14 PM\t1\t");
    assert((int)strcspn(text, "\n") == 119 && !strncmp(text, "abc\xc3\xa9", 5));
    // Too much: whole records only, the newest messages stay, the feed stays under the cap.
    memset(&c.msgs, 0, sizeof c.msgs);
    for (int i = 0; i < OVERLAY_MSGS; i++) {
        snprintf(c.msgs[i].id, sizeof c.msgs[i].id, "%d", 100 + i);
        snprintf(c.msgs[i].name, sizeof c.msgs[i].name, "Person %d", i);
        memset(c.msgs[i].text, 'y', 150);
    }
    c.nmsg = OVERLAY_MSGS;
    n = overlay_chat_feed(feed, 1500, &c, 1000);
    assert(n > 0 && n < 1500 && n == (int)strlen(feed) && feed[n - 1] == '\n' && lines_of(feed, 'M') < OVERLAY_MSGS);
    assert(lines_of(feed, 'M') >= 4 && strstr(feed, "\nM\t119\t")); // the newest is there
    assert(!strstr(feed, "\nM\t100\t") && lines_of(feed, 'N') == 1);
    n = overlay_chat_feed(feed, sizeof feed, &c, 1000);
    assert(lines_of(feed, 'M') == OVERLAY_MSGS && n < OVERLAY_FEED_MAX);
    n = overlay_chat_feed(feed, 120, &c, 1000); // no room for the messages: the header (and mentions) only if they fit
    assert(n == (int)strlen(feed) && n < 120 && lines_of(feed, 'M') == 0);
    n = overlay_chat_feed(feed, 10, &c, 1000); // not even the header
    assert(n == 0 && feed[0] == 0);
    memset(&c, 0, sizeof c);
    n = overlay_chat_feed(feed, sizeof feed, &c, 1000); // nothing: just the header
    assert(!strcmp(feed, "H\t-\t\t\t\t\t0\t\t0\n"));

    // chat.bin goes in atomically: a temp file and a rename, and nothing is left over.
    const char *path = "build/host/chat-test.bin";
    char tmp[64], back[64];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    assert(overlay_write_file(path, "one\n") == 0 && overlay_write_file(path, "two\n") == 0);
    FILE *f = fopen(path, "rb");
    assert(f && fgets(back, sizeof back, f) && !strcmp(back, "two\n"));
    fclose(f);
    assert(!fopen(tmp, "rb"));
    remove(path);
    assert(overlay_write_file("build/host/no-such-dir/chat.bin", "x") == -1);

    // Messages from Discord's JSON: names for mentions, our own mention taken out, kinds and times.
    cJSON *ready = cJSON_Parse(
        "{\"user\":{\"id\":\"1\",\"username\":\"me\"},"
        "\"users\":[{\"id\":\"2\",\"username\":\"bob\",\"global_name\":\"Bob\",\"avatar\":\"abc\"},"
        "{\"id\":\"3\",\"username\":\"cy\"}],"
        "\"guilds\":[{\"id\":\"10\",\"properties\":{\"name\":\"Night Shift\",\"owner_id\":\"9\"},"
        "\"roles\":[{\"id\":\"10\",\"permissions\":\"3072\"},{\"id\":\"77\",\"name\":\"Raiders\",\"permissions\":\"0\"}],"
        "\"channels\":[{\"id\":\"11\",\"name\":\"raid-night\",\"type\":0},{\"id\":\"12\",\"name\":\"Raid Voice\",\"type\":2}]}],"
        "\"merged_members\":[[{\"user_id\":\"1\",\"roles\":[\"77\"]}]],"
        "\"private_channels\":[{\"id\":\"20\",\"type\":1,\"recipient_ids\":[\"2\"]},"
        "{\"id\":\"21\",\"type\":3,\"name\":\"Crew\",\"recipient_ids\":[\"2\",\"3\"]}]}");
    chat_dispatch("READY", ready);
    cJSON_Delete(ready);
    chat_place p;
    assert(chat_place_of("11", &p) && !strcmp(p.server, "Night Shift") && !strcmp(p.name, "raid-night") && !p.dm && !p.voice);
    assert(chat_place_of("12", &p) && p.voice && !strcmp(p.name, "Raid Voice"));
    assert(chat_place_of("20", &p) && p.dm && !strcmp(p.name, "Bob") && !strcmp(p.user, "2") && !strcmp(p.avatar, "abc"));
    assert(chat_place_of("21", &p) && p.dm && !strcmp(p.name, "Crew") && !p.user[0]);
    assert(!chat_place_of("99", &p));
    chat_msg cm;
    cJSON *j = cJSON_Parse("{\"id\":\"900\",\"channel_id\":\"11\",\"type\":0,\"content\":\"<@1> hi <@2>!\","
                           "\"timestamp\":\"2026-11-06T18:14:03.123000+00:00\",\"mentions\":[{\"id\":\"1\"}],"
                           "\"author\":{\"id\":\"2\",\"username\":\"bob\",\"global_name\":\"Bob\",\"avatar\":\"abc\"}}");
    assert(chat_msg_read(j, &cm) && cm.mine && !strcmp(cm.text, "hi @Bob!") && !strcmp(cm.name, "Bob") &&
           !strcmp(cm.avatar, "abc") && !strcmp(cm.author, "2"));
    struct tm want = {.tm_year = 126, .tm_mon = 10, .tm_mday = 6, .tm_hour = 18, .tm_min = 14, .tm_sec = 3};
    assert(cm.at == timegm(&want));
    cJSON_Delete(j);
    j = cJSON_Parse("{\"id\":\"901\",\"channel_id\":\"11\",\"type\":0,\"content\":\"plain\",\"mentions\":[],"
                    "\"author\":{\"id\":\"3\",\"username\":\"cy\"},\"member\":{\"nick\":\"Cyrus\"}}");
    assert(chat_msg_read(j, &cm) && !cm.mine && !strcmp(cm.name, "Cyrus") && !strcmp(cm.text, "plain") && cm.avatar[0] == 0);
    cJSON_Delete(j);
    j = cJSON_Parse("{\"id\":\"902\",\"channel_id\":\"11\",\"type\":0,\"content\":\"\",\"sticker_items\":[{\"id\":\"1\"}],"
                    "\"author\":{\"id\":\"3\",\"username\":\"cy\"}}");
    assert(chat_msg_read(j, &cm) && !strcmp(cm.text, "Sticker"));
    cJSON_Delete(j);
    j = cJSON_Parse("{\"id\":\"903\",\"channel_id\":\"11\",\"type\":7,\"content\":\"\",\"author\":{\"id\":\"3\",\"username\":\"cy\"}}");
    assert(!chat_msg_read(j, &cm)); // a join notice is not a message
    cJSON_Delete(j);
    j = cJSON_Parse("{\"id\":\"904\",\"channel_id\":\"11\",\"content\":\"no author\"}");
    assert(!chat_msg_read(j, &cm));
    cJSON_Delete(j);
    j = cJSON_Parse("{\"id\":\"906\",\"channel_id\":\"12\",\"type\":0,\"content\":\"<@&77> hi\",\"mention_roles\":[\"77\"],"
                    "\"author\":{\"id\":\"3\",\"username\":\"cy\"}}"); // a voice room's own text chat, REST (no server id)
    assert(chat_msg_read(j, &cm) && cm.mine && !strcmp(cm.text, "@Raiders hi"));
    cJSON_Delete(j);
    j = cJSON_Parse("{\"id\":\"905\",\"channel_id\":\"11\",\"type\":0,\"content\":\"hey @everyone\",\"mention_everyone\":true,"
                    "\"author\":{\"id\":\"3\",\"username\":\"cy\"}}");
    assert(chat_msg_read(j, &cm) && cm.mine);
    cJSON_Delete(j);
}

static int sink_calls, sink_mention;
static char sink_type[32];
static void record_sink(const char *type, cJSON *d, int mention)
{
    (void)d;
    snprintf(sink_type, sizeof sink_type, "%s", type);
    sink_mention = mention;
    sink_calls++;
}

// Which gateway messages reach the overlay, and which of them count as mentions.
static void test_overlay_sink(void)
{
    chat_set_message_sink(record_sink);
    struct {
        const char *id, *chan, *guild, *mentions, *author;
        int type, want;
    } cases[] = {
        {"1", "11", ",\"guild_id\":\"10\"", "[{\"id\":\"1\"}]", "2", 0, 1},   // @you in a server
        {"2", "11", ",\"guild_id\":\"10\"", "[]", "2", 0, 0},                // chatter
        {"3", "20", "", "[]", "2", 0, 1},                                    // a DM
        {"4", "20", "", "[]", "1", 0, 0},                                    // our own message
        {"5", "20", "", "[]", "2", 7, 0},                                    // a join notice
        {"6", "11", ",\"guild_id\":\"10\"", "[{\"id\":\"1\"}]", "2", 19, 1}, // a reply that mentions us
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char json[512];
        snprintf(json, sizeof json,
                 "{\"id\":\"%s\",\"channel_id\":\"%s\",\"type\":%d,\"content\":\"x\",\"mentions\":%s%s,"
                 "\"author\":{\"id\":\"%s\",\"username\":\"u\"}}",
                 cases[i].id, cases[i].chan, cases[i].type, cases[i].mentions, cases[i].guild, cases[i].author);
        cJSON *d = cJSON_Parse(json);
        sink_calls = 0;
        chat_dispatch("MESSAGE_CREATE", d);
        assert(sink_calls == 1 && !strcmp(sink_type, "MESSAGE_CREATE") && sink_mention == cases[i].want);
        cJSON_Delete(d);
    }
    cJSON *d = cJSON_Parse("{\"id\":\"1\",\"channel_id\":\"11\",\"content\":\"edit\"}");
    sink_calls = 0;
    chat_dispatch("MESSAGE_UPDATE", d);
    assert(sink_calls == 1 && !strcmp(sink_type, "MESSAGE_UPDATE") && !sink_mention);
    chat_dispatch("MESSAGE_DELETE", d);
    assert(sink_calls == 2 && !strcmp(sink_type, "MESSAGE_DELETE"));
    chat_dispatch("TYPING_START", d); // not a message
    assert(sink_calls == 2);
    cJSON_Delete(d);
    chat_set_message_sink(NULL);
}

int main(void)
{
    test_resample();
    test_overlay_line();
    test_outbox();
    test_png_circle();
    test_gateway_rules();
    test_http_parse();
    test_h264_packetize();
    test_chat();
    test_notify();
    test_ratelimit();
    test_ws_frame();
    test_zstream();
    test_gcm();
    test_rtcp();
    test_ip_discovery();
    test_voice_json();
    test_bin_frame();
    test_voice_state();
    test_calls();
    test_audio();
    test_overlay_feed();
    test_overlay_sink();
    puts("selftest ok");
    return 0;
}
