// SPDX-License-Identifier: AGPL-3.0-or-later
// Real engine sockets: authenticated priority and legacy-client lifetime.
#include "../src/core_key.h"
#include "../src/voice_audio.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int connect_role(int port, int valid)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(fd, (struct sockaddr *)&address, sizeof address) == 0);
    char key[64];
    memset(key, valid ? '0' : 'x',
           sizeof key); // Fixture key: 32 zero bytes, no user key.
    assert(send(fd, key, sizeof key, MSG_NOSIGNAL) == sizeof key);
    return fd;
}

static void wait_flag(int usb, int wanted)
{
    for (int i = 0; i < 200; ++i) {
        if ((usb ? vau_usb_connected() : vau_app_connected()) == wanted)
            return;
        usleep(10000);
    }
    assert(!"engine ownership flag timed out");
}

static void closed(int fd)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    assert(poll(&p, 1, 2000) > 0);
    char byte;
    assert(recv(fd, &byte, 1, 0) == 0);
    close(fd);
}

static void alive(int fd)
{
    char byte;
    assert(recv(fd, &byte, 1, MSG_DONTWAIT) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
}

int main(void)
{
    char directory[] = "/tmp/ps5-discord-pcm-test-XXXXXX";
    assert(mkdtemp(directory));
    const unsigned char raw[32] = {0};
    assert(core_key_make(directory, raw) == 0);
    vau_init();
    int bad = connect_role(VAU_USB_PORT, 0);
    closed(bad);
    assert(!vau_usb_connected());
    int app = connect_role(VAU_PORT, 1);
    wait_flag(0, 1);
    int usb = connect_role(VAU_USB_PORT, 1);
    struct pollfd ready = {.fd = usb, .events = POLLIN};
    assert(poll(&ready, 1, 2000) > 0);
    char ack[2];
    assert(recv(usb, ack, sizeof ack, MSG_WAITALL) == 2 && memcmp(ack, "OK", 2) == 0);
    wait_flag(1, 1);
    alive(app); // USB parks the real app socket instead of disconnecting it.
    assert(send(app, "old", 3, MSG_NOSIGNAL) == 3);
    int busy = connect_role(VAU_USB_PORT, 1);
    closed(busy);
    assert(vau_usb_connected());
    int replacement = connect_role(VAU_PORT, 1);
    closed(app);
    alive(replacement);
    vau_usb_release();
    closed(usb);
    wait_flag(1, 0);
    wait_flag(0, 1);
    alive(replacement); // Same parked app connection resumes without reconnect.
    close(replacement);
    wait_flag(0, 0);
    char path[256];
    snprintf(path, sizeof path, "%s/control.key", directory);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
    puts("PCM engine: wrong key rejected, USB ACK/priority, busy rejection, "
         "parked app replacement/resume passed");
    return 0;
}
