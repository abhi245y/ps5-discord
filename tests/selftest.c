// Host self-checks for the pure parts: websocket framing and zlib-stream reassembly.
// make test
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include <stdlib.h>

#include <cJSON.h>

#include "../src/chat.h"
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
    chat_dispatch("READY", ready);
    cJSON_Delete(ready);
    char *t = chat_tree_json();
    assert(t && strstr(t, "\"name\":\"G\"") && strstr(t, "general"));
    assert(strstr(t, "\"rooms\":[{\"id\":\"12\"")); // voice channels become rooms
    assert(strstr(t, "\"global_name\":\"Bob\"")); // DM recipients land in users
    assert(!strstr(t, "staff"));                       // denied VIEW_CHANNEL: hidden
    assert(strstr(t, "\"club\""));                    // a role of ours allows it back
    assert(strstr(strstr(t, "\"news\""), "\"can_send\":false")); // read-only
    assert(strstr(strstr(t, "\"general\""), "\"can_send\":true"));
    free(t);

    cJSON *m = cJSON_Parse("{\"id\":\"30\",\"channel_id\":\"11\",\"content\":\"hi\","
                           "\"author\":{\"id\":\"2\",\"username\":\"bob\",\"email\":\"x\"}}");
    chat_dispatch("MESSAGE_CREATE", m);
    cJSON_Delete(m);
    char *e = chat_events_json(0);
    assert(e && strstr(e, "\"next\":1") && strstr(e, "\"content\":\"hi\"") && !strstr(e, "email"));
    free(e);
    e = chat_events_json(1);
    assert(e && strstr(e, "\"events\":[]"));
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
    chat_dispatch("GUILD_DELETE", g);
    cJSON_Delete(g);
    t = chat_tree_json();
    assert(!strstr(t, "lobby"));
    free(t);
}

int main(void)
{
    test_chat();
    test_ws_frame();
    test_zstream();
    puts("selftest ok");
    return 0;
}
