// SPDX-License-Identifier: AGPL-3.0-or-later
// Core manages call lifetime and saved preference; supervised worker owns USB.
#include "controller_voice.h"
#include "prefs.h"
#include "voice.h"
#include "voice_audio.h"

#include <stdatomic.h>

static atomic_int paused, suppressed, owned, state;
enum { OFF, STARTING, ON, MISSING, STOPPING, ERROR };
#ifdef __PROSPERO__
static int worker_busy(void);
#endif

void cv_set_tv(int tv)
{
    (void)prefs_set("controller_audio", !tv);
}

int cv_tv(void)
{
    return !prefs_get("controller_audio");
}

int cv_holds_controller(void)
{
#ifdef __PROSPERO__
    return owned || worker_busy();
#else
    return owned;
#endif
}

void cv_pause(int on)
{
    paused = on;
}

void cv_stop(void)
{
    suppressed = 1;
}

const char *cv_state(void)
{
    if (vau_usb_connected())
        return "on";
    static const char *const names[] = {"off", "starting", "on", "missing", "stopping", "error"};
    return names[atomic_load(&state)];
}

#ifndef __PROSPERO__
void cv_init(void)
{
}
#else
#include "ds5_usb.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WORKER "/data/homebrew/PPSA99072/assets/controller-audio.elf"
#define WORKER_LOCK "/data/ps5-discord/vpad-probe.lock"

static uint64_t milliseconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int worker_busy(void)
{
    int fd = open(WORKER_LOCK, O_RDWR);
    if (fd < 0)
        return errno == ENOENT ? 0 : 1;
    int busy = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);
    return busy;
}

static int launch_worker(void)
{
    FILE *file = fopen(WORKER, "rb");
    if (!file) {
        LOG("controller audio: worker asset unavailable (%d)", errno);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0), result = -1;
    if (fd < 0)
        goto done;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
        goto done;
    struct sockaddr_in to = {.sin_len = sizeof to, .sin_family = AF_INET, .sin_port = htons(9021)};
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc = connect(fd, (struct sockaddr *)&to, sizeof to);
    if (rc != 0 && errno != EINPROGRESS)
        goto done;
    uint64_t deadline = milliseconds() + 3000u;
    size_t total = 0;
    unsigned char bytes[4096];
    while (!atomic_load(&paused) && !atomic_load(&suppressed) && milliseconds() < deadline) {
        struct pollfd wait = {.fd = fd, .events = POLLOUT};
        rc = poll(&wait, 1, 50);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0)
            continue;
        if (wait.revents & (POLLERR | POLLHUP | POLLNVAL))
            goto done;
        int error = 0;
        socklen_t len = sizeof error;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error)
            goto done;
        size_t n = fread(bytes, 1, sizeof bytes, file);
        if (!n) {
            if (!ferror(file) && total)
                result = 0;
            break;
        }
        for (size_t sent = 0; sent < n;) {
            ssize_t count = send(fd, bytes + sent, n - sent, MSG_NOSIGNAL);
            if (count > 0) {
                sent += (size_t)count;
                total += (size_t)count;
            } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                if (milliseconds() >= deadline || atomic_load(&paused) || atomic_load(&suppressed))
                    goto done;
                usleep(1000);
            } else
                goto done;
        }
    }
done:
    if (fd >= 0)
        close(fd);
    fclose(file);
    LOG("controller audio: worker launch %s", result == 0 ? "sent" : "failed");
    return result;
}

static void *supervise(void *unused)
{
    (void)unused;
    int stopping = 0, seen_owner = 0;
    uint64_t started = 0, retry = 0;
    for (;;) {
        usleep(200000);
        int call = voice_wanted();
        if (!call)
            suppressed = 0;
        int want =
            prefs_loaded() && prefs_get("controller_audio") && call && !paused && !suppressed;
        uint64_t now = milliseconds();
        if (owned) {
            if (vau_usb_connected())
                seen_owner = 1;
            if (!want && !stopping) {
                stopping = 1;
                state = STOPPING;
                vau_usb_release();
            }
            if (stopping) {
                if (vau_usb_connected())
                    vau_usb_release();
                if ((seen_owner || now - started >= 4000u) && !worker_busy()) {
                    owned = 0;
                    stopping = seen_owner = 0;
                    state = OFF;
                    retry = now + 1000u;
                }
            } else if (vau_usb_connected())
                state = ON;
            else if ((seen_owner || now - started >= 10000u) && !worker_busy()) {
                owned = 0;
                seen_owner = 0;
                state = ERROR;
                retry = now + 10000u;
            }
            continue;
        }
        if (!want) {
            state = OFF;
            continue;
        }
        if (vau_usb_connected()) {
            state = ON;
            continue;
        }
        if (now < retry || worker_busy())
            continue;
        if (!ds5_usb_present()) {
            state = MISSING;
            retry = now + 2000u;
            continue;
        }
        state = STARTING;
        owned = 1;
        started = now;
        if (launch_worker() != 0) {
            state = ERROR;
            stopping = 1;
            retry = now + 10000u;
        }
    }
    return NULL;
}

void cv_init(void)
{
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, supervise, NULL) != 0) {
        state = ERROR;
        LOG("controller audio: supervisor failed to start");
    }
    pthread_attr_destroy(&attr);
}
#endif
