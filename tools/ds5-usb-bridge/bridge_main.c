// SPDX-License-Identifier: AGPL-3.0-or-later
// Direct USB DS5 <-> existing ps5-discord core PCM contract. No LAN audio listener.
#include "ds5_usb.h"
#include "pcm_stream.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef DS5_BRIDGE_DURATION_MS
#define DS5_BRIDGE_DURATION_MS 300000u
#endif
#define CORE_PORT 9042
#define PAD_PORT 9043

extern int32_t sceKernelSendNotificationRequest(int, void *, size_t, int);
static FILE *out_log, *usb_log;
static int core_fd = -1, pad_fd = -1;
static struct sockaddr_in pad_address;
static ds5_pcm_stream pcm;
static uint64_t controller_sent, controller_dropped;

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static void log_message(void *user, const char *message)
{
    (void)user;
    fprintf(stderr, "[ds5-usb-bridge] %s\n", message);
    FILE *logs[] = {out_log, usb_log};
    for (unsigned i = 0; i < 2; i++)
        if (logs[i]) {
            fprintf(logs[i], "%s\n", message);
            fflush(logs[i]);
        }
}

static void notify(const char *message)
{
    struct { char reserved[45]; char message[3075]; } request = {0};
    snprintf(request.message, sizeof request.message, "%s", message);
    sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
    log_message(NULL, message);
}

static void stop_signal(int sig)
{
    (void)sig;
    ds5_usb_request_stop();
}

static int connect_core(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
        close(fd);
        return -1;
    }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(CORE_PORT)};
#if defined(__FreeBSD__) || defined(__PROSPERO__)
    address.sin_len = sizeof address;
#endif
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&address, sizeof address)) {
        if (errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        struct pollfd wait = {.fd = fd, .events = POLLOUT};
        int error = 0;
        socklen_t size = sizeof error;
        if (poll(&wait, 1, 3000) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) {
            close(fd);
            return -1;
        }
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static void controller(void *user, const ds5_usb_controller *state, const uint8_t report[64])
{
    (void)user;
    (void)state; // Direct core integration can consume this decoded state instead.
    if (pad_fd >= 0 && sendto(pad_fd, report, 64, MSG_DONTWAIT | MSG_NOSIGNAL,
                              (struct sockaddr *)&pad_address, sizeof pad_address) == 64)
        controller_sent++;
    else
        controller_dropped++;
}

static void microphone(void *user, const int16_t *mono, size_t frames, int muted)
{
    (void)user;
    ds5_pcm_microphone(&pcm, mono, frames, muted);
}

static size_t playback(void *user, int16_t *stereo, size_t frames)
{
    (void)user;
    return ds5_pcm_playback(&pcm, stereo, frames);
}

static int service(void *user)
{
    (void)user;
    int result = ds5_pcm_pump(&pcm, core_fd);
    if (result)
        log_message(NULL, result > 0 ? "core closed PCM connection; releasing USB" : "PCM socket error; releasing USB");
    return result;
}

static int worker(void)
{
    mkdir("/data/ps5-discord-probe", 0755);
    out_log = fopen("/data/ps5-discord-probe/ds5bridge.txt", "w");
    usb_log = fopen("/mnt/usb0/ds5-usb-bridge.log", "a");
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    log_message(NULL, "start: direct USB audio bridge; core 127.0.0.1:9042; controller UDP 127.0.0.1:9043");
    core_fd = connect_core();
    if (core_fd < 0) {
        notify("DS5 bridge: core PCM socket 127.0.0.1:9042 unavailable. USB was not taken over.");
        if (out_log) fclose(out_log);
        if (usb_log) fclose(usb_log);
        return 1;
    }
    pad_fd = socket(AF_INET, SOCK_DGRAM, 0);
    pad_address = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(PAD_PORT)};
#if defined(__FreeBSD__) || defined(__PROSPERO__)
    pad_address.sin_len = sizeof pad_address;
#endif
    pad_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ds5_usb_config config;
    ds5_usb_default_config(&config);
    config.duration_ms = DS5_BRIDGE_DURATION_MS;
    const ds5_usb_callbacks callbacks = {.log = log_message, .controller = controller,
        .microphone = microphone, .playback = playback, .service = service};
    ds5_usb_stats stats = {0};
    int result = ds5_usb_run(&config, &callbacks, NULL, &stats);
    char summary[512];
    snprintf(summary, sizeof summary,
        "bridge result %d: mic %llu frames (%llu dropped), core RX %llu frames (%llu dropped), pad %llu sent/%llu dropped",
        result, (unsigned long long)pcm.mic_frames, (unsigned long long)pcm.mic_dropped_frames,
        (unsigned long long)pcm.playback_frames, (unsigned long long)pcm.playback_dropped_frames,
        (unsigned long long)controller_sent, (unsigned long long)controller_dropped);
    log_message(NULL, summary);
    close(core_fd);
    if (pad_fd >= 0)
        close(pad_fd);
    if (out_log) fclose(out_log);
    if (usb_log) fclose(usb_log);
    return result == DS5_USB_OK ? 0 : 1;
}

int main(void)
{
    // Packaging build is bounded to five minutes by default. Embedded callers
    // use ds5_usb_run directly with their own call lifecycle and supervision.
    if (!DS5_BRIDGE_DURATION_MS || DS5_BRIDGE_DURATION_MS > 3600000u)
        return 1;
    pid_t child = fork();
    if (child < 0)
        return 1;
    if (!child)
        _exit(worker());
    int status = 0, terminated = 0;
    uint64_t start = now_ms();
    uint64_t deadline = (uint64_t)DS5_BRIDGE_DURATION_MS + 15000u;
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        if (result < 0 && errno != EINTR)
            return 1;
        if (!terminated && now_ms() - start >= deadline - 2000u) {
            kill(child, SIGTERM);
            terminated = 1;
        }
        if (now_ms() - start >= deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            notify("DS5 USB bridge timed out. Reconnect controller.");
            return 1;
        }
        usleep(10000);
    }
}
