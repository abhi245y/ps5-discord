// SPDX-License-Identifier: AGPL-3.0-or-later
#include "golive_opus.h"

#include <opus.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000
#define FRAME 480 // 10 ms
#define BITRATE 96000
#define MAX_PACKET 1275 // an Opus frame is never longer

struct gl_opus {
    OpusEncoder *enc;
    void (*frame)(void *user, const uint8_t *opus, size_t n);
    void *user;
    int16_t pending[FRAME * 2];
    size_t have; // frames in pending
    unsigned long emitted;
};

gl_opus *gl_opus_new(void (*frame)(void *user, const uint8_t *opus, size_t n), void *user)
{
    gl_opus *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    int err = 0;
    g->enc = opus_encoder_create(RATE, 2, OPUS_APPLICATION_AUDIO, &err);
    if (!g->enc || err != OPUS_OK) {
        free(g);
        return NULL;
    }
    opus_encoder_ctl(g->enc, OPUS_SET_BITRATE(BITRATE));
    opus_encoder_ctl(g->enc, OPUS_SET_COMPLEXITY(5)); // the fixed-point C build: spare the CPU
    g->frame = frame;
    g->user = user;
    return g;
}

void gl_opus_free(gl_opus *g)
{
    if (!g)
        return;
    opus_encoder_destroy(g->enc);
    free(g);
}

static int16_t to_s16(float v)
{
    if (v != v) // NaN
        return 0;
    v *= 32768.0f;
    return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)v;
}

void gl_opus_feed(gl_opus *g, const float *pcm, size_t frames)
{
    while (frames) {
        size_t take = FRAME - g->have;
        if (take > frames)
            take = frames;
        for (size_t i = 0; i < take * 2; i++)
            g->pending[g->have * 2 + i] = to_s16(pcm[i]);
        g->have += take;
        pcm += take * 2;
        frames -= take;
        if (g->have < FRAME)
            return;
        uint8_t out[MAX_PACKET];
        int n = opus_encode(g->enc, g->pending, FRAME, out, sizeof out);
        g->have = 0;
        if (n > 0) {
            g->emitted++;
            g->frame(g->user, out, (size_t)n);
        }
    }
}

void gl_opus_discontinuity(gl_opus *g)
{
    g->have = 0;
    opus_encoder_ctl(g->enc, OPUS_RESET_STATE);
}

unsigned long gl_opus_frames(const gl_opus *g)
{
    return g->emitted;
}
