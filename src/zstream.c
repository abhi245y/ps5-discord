#include "zstream.h"

#include <stdlib.h>
#include <string.h>

// A busy account's READY inflates to a few MB; past this a broken or hostile peer is talking.
#define ZSTREAM_MAX (128u << 20)

static int grow(void **buf, size_t *cap, size_t need)
{
    if (need <= *cap)
        return 0;
    if (need > ZSTREAM_MAX)
        return -1;
    size_t c = *cap ? *cap : 65536;
    while (c < need)
        c *= 2;
    void *p = realloc(*buf, c);
    if (!p)
        return -1;
    *buf = p;
    *cap = c;
    return 0;
}

int zstream_init(zstream *s)
{
    memset(s, 0, sizeof *s);
    return inflateInit(&s->z) == Z_OK ? 0 : -1;
}

int zstream_feed(zstream *s, const unsigned char *data, size_t len)
{
    if (grow((void **)&s->in, &s->in_cap, s->in_len + len) != 0)
        return -1;
    memcpy(s->in + s->in_len, data, len);
    s->in_len += len;
    if (s->in_len < 4 || memcmp(s->in + s->in_len - 4, "\x00\x00\xff\xff", 4) != 0)
        return 0;

    s->z.next_in = s->in;
    s->z.avail_in = (uInt)s->in_len;
    s->out_len = 0;
    do {
        if (grow((void **)&s->out, &s->out_cap, s->out_len + 65536) != 0)
            return -1;
        s->z.next_out = (Bytef *)s->out + s->out_len;
        s->z.avail_out = (uInt)(s->out_cap - s->out_len - 1);
        int r = inflate(&s->z, Z_SYNC_FLUSH);
        if (r != Z_OK && r != Z_BUF_ERROR)
            return -1;
        s->out_len = s->out_cap - 1 - s->z.avail_out;
    } while (s->z.avail_in > 0 || s->z.avail_out == 0);
    s->out[s->out_len] = 0;
    s->in_len = 0;
    return 1;
}

void zstream_free(zstream *s)
{
    inflateEnd(&s->z);
    free(s->in);
    free(s->out);
    memset(s, 0, sizeof *s);
}
