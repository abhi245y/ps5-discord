// Discord "zlib-stream" transport compression: one zlib context per connection,
// every complete message ends with the sync-flush marker 00 00 ff ff.
#pragma once
#include <stddef.h>
#include <zlib.h>

typedef struct {
    z_stream z;
    unsigned char *in;   // pending compressed bytes until a marker arrives
    size_t in_len, in_cap;
    char *out;           // inflated JSON of the last message, NUL-terminated
    size_t out_len, out_cap;
} zstream;

int zstream_init(zstream *s);
// Feed one websocket binary message. Returns 1 when s->out holds a full message,
// 0 when more data is needed, <0 on a zlib error.
int zstream_feed(zstream *s, const unsigned char *data, size_t len);
void zstream_free(zstream *s);
