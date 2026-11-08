// SPDX-License-Identifier: AGPL-3.0-or-later
// Go Live's sound: the game's stereo float PCM (48 kHz) cut into exact 10 ms frames (480
// samples per channel) and encoded to Opus, whatever the sizes the capture's records come in.
// Its own encoder state, never voice_audio.c's mono VOIP one.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct gl_opus gl_opus;

// frame is called once per 10 ms frame, from gl_opus_feed's caller (copy and return).
gl_opus *gl_opus_new(void (*frame)(void *user, const uint8_t *opus, size_t n), void *user);
void gl_opus_free(gl_opus *g);
// Interleaved stereo float; any number of frames (one frame = a left and a right sample).
void gl_opus_feed(gl_opus *g, const float *pcm, size_t frames);
// The sound was cut (a gap, a source that came back): the half frame waiting is dropped and the
// encoder starts clean, so the join does not smear. Timestamps: the stream takes them from its
// own clock (stream.c on_audio), so a dropped stretch is simply a gap, never a shift.
void gl_opus_discontinuity(gl_opus *g);
unsigned long gl_opus_frames(const gl_opus *g); // frames emitted so far
