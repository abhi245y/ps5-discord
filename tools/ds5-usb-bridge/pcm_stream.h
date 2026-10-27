// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef DS5_PCM_STREAM_H
#define DS5_PCM_STREAM_H
#include "ds5_usb.h"

#define DS5_PCM_RX_FRAMES 9600u // 200 ms maximum stereo backlog.
#define DS5_PCM_PREFILL 2880u // 60 ms jitter reserve.

typedef struct {
    uint8_t tx[2 * DS5_USB_MAX_MIC_FRAMES];
    size_t tx_size, tx_offset;
    uint8_t carry[4];
    unsigned carry_size;
    int16_t rx[DS5_PCM_RX_FRAMES][2];
    unsigned read, count;
    int primed;
    uint64_t mic_frames, mic_dropped_frames, playback_frames, playback_dropped_frames;
    uint64_t sent_bytes, received_bytes;
} ds5_pcm_stream;

// Queue one microphone grain. A pending partial TCP write is never discarded.
// Returns 1 queued, 0 dropped because an older grain is pending, -1 invalid size.
int ds5_pcm_microphone(ds5_pcm_stream *stream, const int16_t *mono, size_t frames, int muted);
void ds5_pcm_receive(ds5_pcm_stream *stream, const uint8_t *bytes, size_t count);
size_t ds5_pcm_playback(ds5_pcm_stream *stream, int16_t *stereo, size_t frames);
// One bounded nonblocking socket pump. 0 healthy, 1 peer closed, -1 error.
int ds5_pcm_pump(ds5_pcm_stream *stream, int fd);
#endif
