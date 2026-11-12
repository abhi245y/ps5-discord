// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef PCM_CLIENTS_H
#define PCM_CLIENTS_H
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

/* Guarded by the voice engine mutex. Both roles authenticate before entering this mux.
 * USB temporarily parks the app connection; no reconnect race or stale app mic.
 */
typedef struct {
    int current, parked, usb, parked_phase, resume_skip;
    unsigned playback_phase, parked_playback_phase;
} pcm_clients;
#define PCM_CLIENTS_INIT {-1, -1, 0, 0, 0, 0, 0}

static inline void pcm_clients_sent(pcm_clients *c, size_t bytes)
{
    c->playback_phase = (c->playback_phase + (unsigned)(bytes & 3u)) & 3u;
}

/* Complete the partially delivered stereo frame before sending fresh playback. */
static inline size_t pcm_clients_playback_padding(const pcm_clients *c)
{
    return (4u - c->playback_phase) & 3u;
}

static inline void pcm_clients_discard_parked(pcm_clients *c)
{
    char bytes[4096];
    for (int budget = 0; c->parked >= 0 && budget < 256; ++budget) {
        ssize_t n = recv(c->parked, bytes, sizeof bytes, MSG_DONTWAIT);
        if (n > 0) {
            c->parked_phase ^= (int)(n & 1);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(c->parked);
            c->parked = -1;
        }
        return;
    }
    /* A live sender exhausted the 1 MiB drain budget. Reconnect rather than
     * promote a socket that still contains stale mic audio. */
    if (c->parked >= 0) {
        close(c->parked);
        c->parked = -1;
    }
}

/* 1: active client changed, 0: app parked, -1: USB busy (caller retains fd). */
static inline int pcm_clients_take(pcm_clients *c, int fd, int usb)
{
    if (usb && c->usb)
        return -1;
    if (!usb && c->usb) {
        if (c->parked >= 0)
            close(c->parked);
        c->parked = fd;
        c->parked_phase = 0; // Fresh stream starts on a 16-bit sample boundary.
        c->parked_playback_phase = 0;
        return 0;
    }
    if (usb) {
        c->parked = c->current;
        c->parked_phase = 0; // Engine supplies existing parser phase after take.
        c->parked_playback_phase = c->playback_phase;
    } else if (c->current >= 0)
        close(c->current);
    c->current = fd;
    c->usb = usb;
    c->resume_skip = 0;
    c->playback_phase = 0;
    return 1;
}

static inline void pcm_clients_drop(pcm_clients *c)
{
    pcm_clients_discard_parked(c);
    if (c->current >= 0)
        close(c->current);
    c->current = c->parked;
    c->resume_skip = c->parked >= 0 ? c->parked_phase : 0;
    c->playback_phase = c->parked >= 0 ? c->parked_playback_phase : 0;
    c->parked = -1;
    c->parked_phase = 0;
    c->parked_playback_phase = 0;
    c->usb = 0;
}
#endif
