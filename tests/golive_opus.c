// SPDX-License-Identifier: AGPL-3.0-or-later
// Go Live's Opus framing (src/golive_opus.c): exact 10 ms frames from PCM in any chunk sizes.
#include <assert.h>
#include <math.h>
#include <opus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/golive_opus.h"

struct got {
    uint8_t pkt[4000][400];
    size_t len[4000];
    unsigned n;
};

static void on_frame(void *user, const uint8_t *opus, size_t n)
{
    struct got *g = user;
    assert(n > 0 && n <= 400 && g->n < 4000);
    memcpy(g->pkt[g->n], opus, n);
    g->len[g->n++] = n;
}

static float *tone(size_t frames)
{
    float *p = malloc(frames * 2 * sizeof *p);
    for (size_t i = 0; i < frames; i++) {
        p[2 * i] = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / 48000.0f);
        p[2 * i + 1] = 0.25f * sinf(2.0f * 3.14159265f * 880.0f * (float)i / 48000.0f);
    }
    return p;
}

static struct got whole, chunked;

int main(void)
{
    const size_t total = 48000; // one second
    float *pcm = tone(total);

    // One go, and the same sound in chunks of awkward sizes (down to a single frame, past a whole
    // frame, a record's 1024): the packets must be byte for byte the same.
    gl_opus *a = gl_opus_new(on_frame, &whole), *b = gl_opus_new(on_frame, &chunked);
    assert(a && b);
    gl_opus_feed(a, pcm, total);
    static const size_t sizes[] = {1, 7, 479, 480, 481, 1024, 3, 960, 1000, 2, 4096, 333};
    size_t at = 0, i = 0;
    while (at < total) {
        size_t n = sizes[i++ % (sizeof sizes / sizeof *sizes)];
        if (n > total - at)
            n = total - at;
        // Chunks of one frame, to the end of a 4096 call: the b encoder must not care.
        gl_opus_feed(b, pcm + 2 * at, n);
        at += n;
    }
    assert(whole.n == total / 480 && chunked.n == whole.n);
    assert(gl_opus_frames(a) == whole.n && gl_opus_frames(b) == chunked.n);
    for (unsigned k = 0; k < whole.n; k++)
        assert(whole.len[k] == chunked.len[k] && !memcmp(whole.pkt[k], chunked.pkt[k], whole.len[k]));

    // Every packet is one 10 ms frame of stereo that decodes, and the tone survives.
    int err = 0;
    OpusDecoder *dec = opus_decoder_create(48000, 2, &err);
    assert(dec && err == OPUS_OK);
    double energy = 0;
    for (unsigned k = 0; k < whole.n; k++) {
        assert(opus_packet_get_nb_channels(whole.pkt[k]) == 2);
        assert(opus_packet_get_samples_per_frame(whole.pkt[k], 48000) == 480);
        assert(opus_packet_get_nb_frames(whole.pkt[k], whole.len[k]) == 1);
        int16_t out[480 * 2];
        assert(opus_decode(dec, whole.pkt[k], (opus_int32)whole.len[k], out, 480, 0) == 480);
        if (k >= 5)
            for (int s = 0; s < 480 * 2; s++)
                energy += (double)out[s] * out[s] / (32768.0 * 32768.0);
    }
    const double rms = sqrt(energy / ((whole.n - 5) * 960.0));
    assert(rms > 0.2 && rms < 0.5); // 0.5 and 0.25 amplitude sines: about 0.28 together
    opus_decoder_destroy(dec);

    // A cut: the half frame waiting goes, the rest starts a clean frame.
    struct got cut = {0};
    gl_opus *c = gl_opus_new(on_frame, &cut);
    gl_opus_feed(c, pcm, 100);
    gl_opus_discontinuity(c);
    gl_opus_feed(c, pcm, 379);
    assert(cut.n == 0);
    gl_opus_feed(c, pcm, 101);
    assert(cut.n == 1);

    // Out-of-range and NaN samples are clipped, never undefined.
    float wild[960];
    for (int s = 0; s < 960; s++)
        wild[s] = s % 3 == 0 ? 4.0f : s % 3 == 1 ? -4.0f : nanf("");
    gl_opus_feed(c, wild, 480);
    assert(cut.n == 2);

    gl_opus_free(a);
    gl_opus_free(b);
    gl_opus_free(c);
    free(pcm);
    printf("PASS: golive opus: %u exact 10 ms frames from any chunking, byte-identical, decodable\n", whole.n);
    return 0;
}
