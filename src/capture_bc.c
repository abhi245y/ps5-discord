// SPDX-License-Identifier: AGPL-3.0-or-later
// Go Live's wireless backend (capture_bc.h). The core listens on loopback, writes a fresh
// random token to a 0600 file, launches the capture helper through elfldr (as the overlay
// service is) and serves its socket. A crashed or wedged helper never takes the core down; a
// crashed core closes the socket, which makes the helper clean up and leave SceRemotePlay.
#include "capture_bc.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "capture_proto.h"
#include "game_audio.h"
#include "golive_opus.h"
#include "log.h"
#include "tls.h"

#define APP_ASSETS "/data/homebrew/PPSA99072/assets"
#define LISTEN_MAX_REJECTS 32

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; // conn_fd and the profile
static int conn_fd = -1;                                 // the authenticated helper, or -1
static int actual_w, actual_h, actual_fps;
static uint64_t last_key_ms;
#ifdef __PROSPERO__
static int send_helper(void);
static capture_launcher launcher = send_helper; // tests (and the host) set their own
#else
static capture_launcher launcher;
#endif
static const char *conn_file = CAP_CONN_FILE;
static int connect_timeout_ms = 20000, stop_timeout_ms = 10000;

static uint64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

void capture_set_launcher(capture_launcher l) { launcher = l; }
void capture_set_conn_file(const char *path) { conn_file = path; }
void capture_set_timeouts(int connect_ms, int stop_ms)
{
    connect_timeout_ms = connect_ms;
    stop_timeout_ms = stop_ms;
}

#ifdef __PROSPERO__
// The helper payload from the app's assets to elfldr (as overlay.c's send_elf).
static int send_helper(void)
{
    const char *path = APP_ASSETS "/" CAP_ELF_NAME;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *elf = size > 0 && size < (8 << 20) ? malloc((size_t)size) : NULL;
    size_t n = elf ? fread(elf, 1, (size_t)size, f) : 0;
    fclose(f);
    int fd = socket(AF_INET, SOCK_STREAM, 0), ok = -1;
    struct sockaddr_in to = {.sin_len = sizeof to, .sin_family = AF_INET, .sin_port = htons(9021)}; // elfldr
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    size_t sent = 0;
    if (fd >= 0 && n == (size_t)size && connect(fd, (struct sockaddr *)&to, sizeof to) == 0) {
        for (ssize_t w; sent < n && (w = send(fd, elf + sent, n - sent, 0)) > 0;)
            sent += (size_t)w;
        ok = sent == n ? 0 : -1;
    }
    if (fd >= 0)
        close(fd);
    free(elf);
    LOG("capture: sent %s to elfldr (%zu of %zu bytes)", path, sent, n);
    return ok;
}
#endif

static int send_msg(int fd, uint8_t type)
{
    uint8_t m[CAP_HEADER_BYTES];
    cap_pack_header(m, type, 0, 0);
    // 8 bytes into an empty loopback buffer; MSG_DONTWAIT so that no lock is held on a stall.
    return send(fd, m, sizeof m, MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)sizeof m ? 0 : -1;
}

void capture_request_keyframe(void)
{
    pthread_mutex_lock(&lock);
    const uint64_t now = mono_ms();
    if (conn_fd >= 0 && now - last_key_ms >= 200) { // viewers ask every second; a burst is one IDR
        last_key_ms = now;
        send_msg(conn_fd, CAP_KEYFRAME);
    }
    pthread_mutex_unlock(&lock);
}

int capture_actual_profile(int *w, int *h, int *fps)
{
    pthread_mutex_lock(&lock);
    *w = actual_w, *h = actual_h, *fps = actual_fps;
    const int known = actual_w > 0;
    pthread_mutex_unlock(&lock);
    return known;
}

// ---- the game's sound: shared capture -> Opus -> sink ---------------------------------------

struct audio {
    const capture_params *p;
    int id;
    _Atomic int stop, live;
    gl_opus *enc;
    pthread_t thread;
    int started;
};

static void audio_frame(void *user, const uint8_t *opus, size_t n)
{
    struct audio *a = user;
    if (a->p->audio)
        a->p->audio(opus, n);
}

static void *audio_main(void *arg)
{
    struct audio *a = arg;
    static __thread float pcm[0x2000 / 4];
    int down = 0;
    while (!a->stop) {
        const int n = ga_read(a->id, pcm, sizeof pcm);
        if (n < 0) { // the capture is down (it reopens itself): a gap, then a clean start
            down = 1;
            usleep(10000);
        } else if (n < 8) {
            usleep(4000);
        } else {
            if (down)
                gl_opus_discontinuity(a->enc);
            down = 0;
            if (a->live)
                gl_opus_feed(a->enc, pcm, (size_t)n / 8); // before the picture is up, nothing is sent
        }
    }
    return NULL;
}

static void audio_start(struct audio *a, const capture_params *p)
{
    memset(a, 0, sizeof *a);
    a->p = p;
    a->id = ga_acquire();
    a->enc = a->id >= 0 ? gl_opus_new(audio_frame, a) : NULL;
    if (!a->enc) {
        LOG("capture: no game sound for Go Live (the picture goes alone)");
        return;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    a->started = pthread_create(&a->thread, &attr, audio_main, a) == 0;
    pthread_attr_destroy(&attr);
}

static void audio_stop(struct audio *a)
{
    a->stop = 1;
    if (a->started)
        pthread_join(a->thread, NULL);
    gl_opus_free(a->enc);
    if (a->id >= 0)
        ga_release(a->id);
}

// ---- the socket ---------------------------------------------------------------------------

static int write_conn_file(unsigned port, const uint8_t token[CAP_TOKEN_BYTES])
{
    char text[80];
    int n = snprintf(text, sizeof text, "%u ", port);
    for (unsigned i = 0; i < CAP_TOKEN_BYTES; i++)
        n += snprintf(text + n, sizeof text - (size_t)n, "%02x", token[i]);
    text[n++] = '\n';
    unlink(conn_file); // never write through an old file's mode
    int fd = open(conn_file, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return -1;
    const int ok = write(fd, text, (size_t)n) == n;
    return close(fd) == 0 && ok ? 0 : -1;
}

// 0 when the connection said hello with our token and version.
static int authenticate(int fd, const uint8_t token[CAP_TOKEN_BYTES])
{
    uint8_t buf[CAP_HEADER_BYTES + 4 + CAP_TOKEN_BYTES];
    size_t got = 0;
    const uint64_t begin = mono_ms();
    while (got < sizeof buf && mono_ms() - begin < 2000) {
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        if (poll(&pf, 1, 100) <= 0)
            continue;
        ssize_t n = recv(fd, buf + got, sizeof buf - got, 0);
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    cap_header h;
    if (got < sizeof buf || cap_parse_header(buf, &h) || h.type != CAP_HELLO || h.len != 4 + CAP_TOKEN_BYTES ||
        cap_get32(buf + CAP_HEADER_BYTES) != CAP_VERSION)
        return -1;
    unsigned char diff = 0; // the whole token is compared, whatever the first byte says
    for (unsigned i = 0; i < CAP_TOKEN_BYTES; i++)
        diff |= buf[CAP_HEADER_BYTES + 4 + i] ^ token[i];
    return diff ? -1 : 0;
}

static void set_conn(int fd)
{
    pthread_mutex_lock(&lock);
    conn_fd = fd;
    last_key_ms = 0;
    pthread_mutex_unlock(&lock);
}

const char *capture_run(const capture_params *p)
{
    static char why_text[128];
    why_text[0] = 0;
    actual_w = actual_h = actual_fps = 0;
    uint8_t token[CAP_TOKEN_BYTES];
    if (tls_random(NULL, token, sizeof token) != 0)
        return "Capture could not start.";
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // loopback only
    socklen_t al = sizeof a;
    if (listener < 0 || bind(listener, (struct sockaddr *)&a, sizeof a) != 0 || listen(listener, 4) != 0 ||
        getsockname(listener, (struct sockaddr *)&a, &al) != 0) {
        if (listener >= 0)
            close(listener);
        return "Capture could not start.";
    }
    if (write_conn_file(ntohs(a.sin_port), token) != 0) {
        close(listener);
        return "Capture could not start.";
    }
    struct audio audio;
    audio_start(&audio, p); // opens in the background while the helper loads
    const char *result = "Capture did not start.";
    int fd = -1, launches = 0;
    uint64_t begin = mono_ms(), next_launch = begin;
    unsigned rejects = 0;
    // The helper: launched now and again if it has not called after 8 s (one still cleaning up
    // from the last Go Live holds its single-instance lock and exits; a second launch is harmless).
    while (fd < 0) {
        const uint64_t now = mono_ms();
        if (!p->keep_going()) {
            result = NULL;
            goto out;
        }
        if (now - begin >= (uint64_t)connect_timeout_ms || rejects >= LISTEN_MAX_REJECTS)
            goto out;
        if (launches < 3 && now >= next_launch) {
            launches++;
            next_launch = now + 8000;
            if (launcher && launcher() != 0)
                LOG("capture: the helper could not be sent to the console");
        }
        struct pollfd pf = {.fd = listener, .events = POLLIN};
        if (poll(&pf, 1, 100) <= 0)
            continue;
        int c = accept(listener, NULL, NULL);
        if (c < 0)
            continue;
        if (authenticate(c, token) == 0)
            fd = c;
        else {
            LOG("capture: a connection without the right token was dropped");
            rejects++;
            close(c);
        }
    }
    close(listener); // no one else may connect now
    listener = -1;
    unlink(conn_file);
    LOG("capture: helper connected after %llu ms", (unsigned long long)(mono_ms() - begin));

    {
        uint8_t m[CAP_HEADER_BYTES + 24];
        cap_pack_header(m, CAP_START, 0, 24);
        const uint32_t selector = p->height >= 1080 ? 4 : 3, flag = p->fps >= 60;
        cap_put32(m + 8, selector), cap_put32(m + 12, flag), cap_put32(m + 16, (uint32_t)p->bitrate);
        cap_put32(m + 20, (uint32_t)p->width), cap_put32(m + 24, (uint32_t)p->height), cap_put32(m + 28, (uint32_t)p->fps);
        if (send(fd, m, sizeof m, MSG_NOSIGNAL) != (ssize_t)sizeof m) {
            result = "Capture could not start.";
            goto out;
        }
    }
    set_conn(fd);

    uint8_t *in = malloc(CAP_HEADER_BYTES + CAP_MAX_PAYLOAD);
    if (!in) {
        result = "Capture could not start.";
        goto out;
    }
    size_t have = 0;
    int stopping = 0, was_recovering = 0, live = 0;
    uint64_t stop_at = 0, frames = 0;
    result = "Capture stopped.";
    for (;;) {
        if (!stopping && !p->keep_going()) {
            pthread_mutex_lock(&lock);
            send_msg(fd, CAP_STOP);
            pthread_mutex_unlock(&lock);
            stopping = 1;
            stop_at = mono_ms();
        }
        if (stopping && mono_ms() - stop_at > (uint64_t)stop_timeout_ms) {
            LOG("capture: the helper did not finish in %d ms; closing on it", stop_timeout_ms);
            break;
        }
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        if (poll(&pf, 1, 100) > 0) {
            ssize_t n = recv(fd, in + have, CAP_HEADER_BYTES + CAP_MAX_PAYLOAD - have, 0);
            if (n <= 0)
                break; // the helper is gone
            have += (size_t)n;
        }
        int bad = 0;
        for (;;) { // every whole message in what has arrived
            cap_header h;
            if (have < CAP_HEADER_BYTES)
                break;
            if (cap_parse_header(in, &h)) {
                bad = 1;
                break;
            }
            if (have < CAP_HEADER_BYTES + h.len)
                break;
            const uint8_t *body = in + CAP_HEADER_BYTES;
            switch (h.type) {
            case CAP_VIDEO:
                frames++;
                if (p->video)
                    p->video(body, h.len, h.flags & CAP_KEY);
                break;
            case CAP_HEADERS: // SPS and PPS as an access unit of their own: stream.c keeps them for the IDRs
                if (p->video)
                    p->video(body, h.len, 0);
                break;
            case CAP_PROFILE:
                if (h.len == 16) {
                    pthread_mutex_lock(&lock);
                    actual_w = (int)cap_get32(body), actual_h = (int)cap_get32(body + 4), actual_fps = (int)cap_get32(body + 8);
                    pthread_mutex_unlock(&lock);
                    LOG("capture: encoder runs %dx%d at %d fps (output mode 0x%x)", actual_w, actual_h, actual_fps,
                        cap_get32(body + 12));
                }
                break;
            case CAP_STATE:
                if (h.len >= 1 && body[0] == CAP_LIVE) {
                    if (!live && p->live)
                        p->live();
                    live = 1;
                    audio.live = 1;
                    if (was_recovering) { // the picture is back: a keyframe so viewers join it at once
                        LOG("capture: recovered after a gap (%llu frames so far)", (unsigned long long)frames);
                        was_recovering = 0;
                        capture_request_keyframe();
                    }
                } else if (h.len >= 1 && body[0] == CAP_RECOVERING) {
                    LOG("capture: the picture is recovering (a game or display switch)");
                    was_recovering = 1;
                } else if (h.len >= 1 && body[0] == CAP_ERROR) {
                    size_t t = h.len - 1 < sizeof why_text - 1 ? h.len - 1 : sizeof why_text - 1;
                    for (size_t i = 0; i < t; i++) // text from another process: printable ASCII only
                        why_text[i] = body[1 + i] >= 32 && body[1 + i] < 127 ? (char)body[1 + i] : '?';
                    why_text[t] = 0;
                }
                break;
            default:
                bad = 1; // not a message the helper sends
            }
            if (bad)
                break;
            memmove(in, in + CAP_HEADER_BYTES + h.len, have - CAP_HEADER_BYTES - h.len);
            have -= CAP_HEADER_BYTES + h.len;
        }
        if (bad) {
            LOG("capture: a bad message from the helper, closing");
            break;
        }
    }
    free(in);
    // Asked to stop: the helper's EOF (or our close) is the normal end. Otherwise the helper
    // told why, or just vanished.
    result = stopping || !p->keep_going() ? NULL : why_text[0] ? why_text : "Capture stopped.";
out:
    set_conn(-1);
    if (fd >= 0)
        close(fd); // either way the helper ends its capture session when this closes
    if (listener >= 0)
        close(listener);
    unlink(conn_file);
    audio_stop(&audio);
    pthread_mutex_lock(&lock);
    actual_w = actual_h = actual_fps = 0;
    pthread_mutex_unlock(&lock);
    return result;
}
