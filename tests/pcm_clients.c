// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../src/pcm_clients.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void pair(int fds[2])
{
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
}

int main(void)
{
    pcm_clients c = PCM_CLIENTS_INIT;
    int app[2], usb[2], next[2], busy[2];
    pair(app);
    pair(usb);
    pair(next);
    pair(busy);
    assert(pcm_clients_take(&c, app[0], 0) == 1);
    assert(pcm_clients_take(&c, usb[0], 1) == 1);
    assert(c.current == usb[0] && c.parked == app[0] && c.usb);
    assert(send(app[1], "stale", 5, 0) == 5);
    pcm_clients_discard_parked(&c);
    assert(c.parked_phase == 1); // Odd TCP fragment ends in the middle of S16 sample.
    char bytes[32];
    assert(recv(app[0], bytes, sizeof bytes, MSG_DONTWAIT) < 0 &&
           (errno == EAGAIN || errno == EWOULDBLOCK));
    assert(send(usb[1], "voice", 5, 0) == 5);
    assert(recv(c.current, bytes, sizeof bytes, 0) == 5 && memcmp(bytes, "voice", 5) == 0);
    assert(pcm_clients_take(&c, busy[0], 1) == -1);
    assert(c.current == usb[0] && c.parked == app[0]);
    close(busy[0]);
    close(busy[1]);
    assert(pcm_clients_take(&c, next[0], 0) == 0);
    assert(c.parked_phase == 0);
    assert(recv(app[1], bytes, sizeof bytes, 0) == 0); // restarted app replaced parked one
    close(app[1]);
    assert(send(next[1], "old", 3, 0) == 3);
    pcm_clients_drop(&c);
    assert(c.current == next[0] && c.parked == -1 && !c.usb);
    assert(c.resume_skip == 1); // Discard stale high byte before accepting fresh S16.
    assert(recv(usb[1], bytes, sizeof bytes, 0) == 0);
    close(usb[1]);
    assert(send(next[1], "fresh", 5, 0) == 5);
    assert(recv(c.current, bytes, sizeof bytes, 0) == 5 && memcmp(bytes, "fresh", 5) == 0);
    pcm_clients_drop(&c);
    close(next[1]);

    pair(app);
    pair(usb);
    assert(pcm_clients_take(&c, app[0], 0) == 1);
    assert(pcm_clients_take(&c, usb[0], 1) == 1);
    close(app[1]);
    pcm_clients_discard_parked(&c);
    assert(c.parked == -1 && c.current == usb[0]);
    pcm_clients_drop(&c);
    close(usb[1]);
    assert(c.current == -1 && c.parked == -1);
    pair(app);
    pair(next);
    assert(pcm_clients_take(&c, app[0], 0) == 1);
    assert(pcm_clients_take(&c, next[0], 0) == 1);
    assert(recv(app[1], bytes, sizeof bytes, 0) == 0);
    close(app[1]);
    pcm_clients_drop(&c);
    close(next[1]);
    puts("PCM ownership: park, discard stale mic, reject second USB, replace "
         "app, resume, disconnect passed");
    return 0;
}
