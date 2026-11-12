// SPDX-License-Identifier: AGPL-3.0-or-later
// Existing core PCM protocol: src/voice_audio.c + src/core_key.h.
// Authentication matches app/src/platform/ps5/voice_audio.cpp:connect_core.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include "pcm_stream.h"

static int g_core_fd = -1;
static ds5_pcm_stream g_core_pcm; /* Owned exclusively by the USB thread. */

static int usb_call_wait(int fd, short events, uint64_t deadline)
{
    while (running() && now_ms() < deadline) {
        struct pollfd p = {.fd = fd, .events = events};
        int rc = poll(&p, 1, 20);
        if (rc > 0)
            return (p.revents & (POLLERR | POLLHUP | POLLNVAL)) ? -1 : 0;
        if (rc < 0 && errno != EINTR)
            return -1;
    }
    return -1;
}

static int usb_call_connect(void)
{
    char key[64] = {0};
    int fd = -1, ok = 0;
    const char *stage = "read core key";
    FILE *file = fopen("/data/homebrew/PPSA99072/control.key", "rb");
    if (!file)
        goto done;
    size_t n = fread(key, 1, sizeof key, file);
    int valid = n == sizeof key && fgetc(file) == EOF && !ferror(file);
    if (fclose(file) != 0)
        valid = 0;
    for (size_t i = 0; i < n; ++i)
        if (!isxdigit((unsigned char)key[i]))
            valid = 0;
    if (!valid)
        goto done;
    stage = "connect core PCM";
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        goto done;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
        goto done;
    struct sockaddr_in address = {.sin_len = sizeof address,
        .sin_family = AF_INET, .sin_port = htons(9045)};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    uint64_t deadline = now_ms() + 1500;
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        int error = 0;
        socklen_t size = sizeof error;
        if (errno != EINPROGRESS || usb_call_wait(fd, POLLOUT, deadline) != 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0)
            goto done;
    }
    stage = "send core key";
    size_t sent = 0;
    while (sent < sizeof key && running()) {
        ssize_t count = send(fd, key + sent, sizeof key - sent, MSG_NOSIGNAL);
        if (count > 0)
            sent += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
                 usb_call_wait(fd, POLLOUT, deadline) == 0)
            continue;
        else
            goto done;
        if (now_ms() >= deadline)
            goto done;
    }
    if (sent != sizeof key)
        goto done;
    stage = "wait for USB PCM ownership acknowledgement";
    char ack[2];
    size_t received = 0;
    while (received < sizeof ack && running()) {
        ssize_t count = recv(fd, ack + received, sizeof ack - received, 0);
        if (count > 0)
            received += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
                 usb_call_wait(fd, POLLIN, deadline) == 0)
            continue;
        else
            goto done;
        if (now_ms() >= deadline)
            goto done;
    }
    if (received != sizeof ack || memcmp(ack, "OK", sizeof ack) != 0)
        goto done;
    int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) != 0)
        goto done;
    g_core_fd = fd;
    ok = 1;
done:
    /* Key remains console-local and is never logged or copied to the host. */
    for (volatile char *p = key; p < key + sizeof key; ++p)
        *p = 0;
    if (!ok) {
        log_line("call PCM setup failed at %s errno=%d; USB not taken", stage, errno);
        if (fd >= 0)
            close(fd);
        return -1;
    }
    log_line("call PCM connected to 127.0.0.1:9045; core key accepted and USB ownership acknowledged");
    return 0;
}

static void usb_call_mic(void *user, const int16_t *mono, size_t frames, int muted)
{
    (void)user;
    (void)ds5_pcm_microphone(&g_core_pcm, mono, frames, muted);
}

static size_t usb_playback(void *user, int16_t *stereo, size_t frames)
{
    (void)user;
    return ds5_pcm_playback(&g_core_pcm, stereo, frames);
}

static void usb_call_close(void)
{
    /* Called only after the USB owner is joined. */
    if (g_core_fd < 0)
        return;
    log_line("call PCM mic_frames=%llu mic_dropped=%llu sent_bytes=%llu received_bytes=%llu playback_frames=%llu playback_dropped=%llu",
        (unsigned long long)g_core_pcm.mic_frames,
        (unsigned long long)g_core_pcm.mic_dropped_frames,
        (unsigned long long)g_core_pcm.sent_bytes,
        (unsigned long long)g_core_pcm.received_bytes,
        (unsigned long long)g_core_pcm.playback_frames,
        (unsigned long long)g_core_pcm.playback_dropped_frames);
    close_logged(&g_core_fd, "core PCM");
}
