// Host self-checks for the pure parts: websocket framing and zlib-stream reassembly.
// make test
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

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

int main(void)
{
    test_ws_frame();
    test_zstream();
    puts("selftest ok");
    return 0;
}
