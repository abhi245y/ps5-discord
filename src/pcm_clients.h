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
} pcm_clients;
#define PCM_CLIENTS_INIT {-1, -1, 0, 0, 0}

static inline void pcm_clients_discard_parked(pcm_clients *c)
{
    char bytes[4096];
    for (int budget = 0; c->parked >= 0 && budget < 8; ++budget) {
        ssize_t n = recv(c->parked, bytes, sizeof bytes, MSG_DONTWAIT);
        if (n > 0) {
            c->parked_phase ^= (int)(n & 1);
            continue;
        }
        if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(c->parked);
            c->parked = -1;
        }
        break;
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
        return 0;
    }
    if (usb) {
        c->parked = c->current;
        c->parked_phase = 0; // Engine supplies existing parser phase after take.
    } else if (c->current >= 0)
        close(c->current);
    c->current = fd;
    c->usb = usb;
    c->resume_skip = 0;
    return 1;
}

static inline void pcm_clients_drop(pcm_clients *c)
{
    pcm_clients_discard_parked(c);
    if (c->current >= 0)
        close(c->current);
    c->current = c->parked;
    c->resume_skip = c->parked >= 0 ? c->parked_phase : 0;
    c->parked = -1;
    c->parked_phase = 0;
    c->usb = 0;
}
#endif
