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

// Include the real engine to exercise its parser/routing and force partial writes.
// Deterministic checks run before vau_init() starts the engine thread.
static ssize_t engine_send(int fd, const void *bytes, size_t count, int flags);
#define send engine_send
#include "../src/voice_audio.c"
#undef send

static int partial_fd = -1;
static size_t partial_left;

static ssize_t engine_send(int fd, const void *bytes, size_t count, int flags)
{
    if (fd == partial_fd) {
        if (!partial_left) {
            errno = EAGAIN;
            return -1;
        }
        if (count > partial_left)
            count = partial_left;
    }
    ssize_t n = send(fd, bytes, count, flags);
    if (fd == partial_fd && n > 0)
        partial_left -= (size_t)n;
    return n;
}

static int accept_pair(int usb)
{
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(pending_fd == -1);
    pending_fd = fds[0];
    pending_usb = usb;
    char key[CORE_KEY_LEN];
    memset(key, '0', sizeof key);
    assert(send(fds[1], key, sizeof key, 0) == sizeof key);
    check_pending();
    assert(pending_fd == -1);
    if (usb) {
        char ack[2];
        assert(recv(fds[1], ack, sizeof ack, MSG_WAITALL) == 2 && memcmp(ack, "OK", 2) == 0);
    }
    return fds[1];
}

static void external_routes(void)
{
    int app = accept_pair(0);
    const int16_t mono[] = {1000, 2000, 3000, 4000};
    assert(vau_ext_acquire(VAU_EXT_BT));
    vau_ext_mic(VAU_EXT_BT, mono, 4);
    // Failed/other-user Remote Play exit neither releases BT nor clears its ring.
    vau_ext_release(VAU_EXT_REMOTE_PLAY);
    assert(vau_ext_get() && ext_mic_count == 4);
    assert(send(app, mono, sizeof mono, 0) == sizeof mono);
    read_mic();
    assert(mic_count == 0); // App mic is suppressed while BT owns the route.

    int usb = accept_pair(1);
    assert(!vau_ext_acquire(VAU_EXT_BT)); // BT yields selection, remains available.
    vau_ext_mic(VAU_EXT_BT, mono, 4);
    assert(ext_mic_count == 0); // A cached BT callback cannot feed USB's mic.
    assert(send(usb, mono, sizeof mono, 0) == sizeof mono);
    read_mic();
    assert(mic_count == 4); // USB PCM still outranks the registered external route.
    vau_ext_release(VAU_EXT_BT); // Headset disconnects while USB is selected.
    vau_usb_release();
    assert(!vau_ext_get() && !vau_usb_connected() && vau_app_connected());
    assert(send(app, mono, sizeof mono, 0) == sizeof mono);
    read_mic();
    assert(mic_count == 4);
    for (int i = 0; i < 4; ++i)
        assert(mic[(mic_head + i) % MIC_CAP] == mono[i]); // App mic is live again.
    close(usb);

    assert(vau_ext_acquire(VAU_EXT_BT));
    usb = accept_pair(1);
    assert(!vau_ext_acquire(VAU_EXT_BT));
    vau_usb_release(); // Connected BT resumes without waiting for another tick.
    assert(vau_ext_acquire(VAU_EXT_BT));
    close(usb);

    assert(vau_ext_acquire(VAU_EXT_REMOTE_PLAY));
    // BT release between RP acquire and setting holding cannot clear RP's route.
    vau_ext_release(VAU_EXT_BT);
    assert(vau_ext_get() && ext_owner == VAU_EXT_REMOTE_PLAY);
    assert(!vau_ext_acquire(VAU_EXT_BT));
    vau_ext_mic(VAU_EXT_BT, mono, 4);
    assert(ext_mic_count == 0);
    vau_ext_mic(VAU_EXT_REMOTE_PLAY, mono, 4);
    assert(ext_mic_count == 4);
    int16_t stereo[2 * DS5_PCM_PREFILL];
    for (size_t i = 0; i < sizeof stereo / sizeof stereo[0]; ++i)
        stereo[i] = 1234;
    ds5_pcm_receive(&ext_out, (const uint8_t *)stereo, sizeof stereo);
    assert(vau_ext_playback(VAU_EXT_BT, stereo, 1) == 0);
    assert(ext_out.count == DS5_PCM_PREFILL);
    assert(vau_ext_playback(VAU_EXT_REMOTE_PLAY, stereo, 1) == 1 && stereo[0] == 1234);
    vau_ext_release(VAU_EXT_REMOTE_PLAY);
    assert(ext_owner == VAU_EXT_BT && ext_mic_count == 0 && ext_out.count == 0);
    vau_ext_release(VAU_EXT_BT);
    assert(!vau_ext_get());
    client_drop();
    close(app);
}

static void partial_playback(void)
{
    for (size_t partial = 1; partial < 4; ++partial) {
        int app = accept_pair(0);
        int16_t frame[2 * VAU_FRAME];
        memset(frame, 0x5a, sizeof frame);
        out_push(frame);
        partial_fd = audio_clients.current;
        partial_left = partial;
        out_flush(); // Real socket write, then forced EAGAIN mid-stereo-frame.
        partial_fd = -1;
        uint8_t received[4 + sizeof frame];
        assert(recv(app, received, partial, MSG_WAITALL) == (ssize_t)partial);
        assert(out_len == sizeof frame - partial);
        int usb = accept_pair(1);
        assert(out_len == 0);
        vau_usb_release();
        close(usb);
        assert(out_len == 4 - partial);
        memset(frame, 0x37, sizeof frame);
        out_push(frame);
        out_flush();
        size_t remaining = 4 - partial + sizeof frame;
        assert(recv(app, received + partial, remaining, MSG_WAITALL) == (ssize_t)remaining);
        for (size_t i = 0; i < partial; ++i)
            assert(received[i] == 0x5a);
        for (size_t i = partial; i < 4; ++i)
            assert(received[i] == 0);
        assert(memcmp(received + 4, frame, sizeof frame) == 0);
        assert(audio_clients.playback_phase == 0);
        client_drop();
        close(app);
    }
}

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
    external_routes();
    partial_playback();
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
         "parked app replacement/resume, external owners, app mic handback, "
         "partial stereo alignment passed");
    return 0;
}
