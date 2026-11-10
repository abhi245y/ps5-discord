// SPDX-License-Identifier: AGPL-3.0-or-later
// Go Live's wireless backend (capture_bc.h). The core listens on loopback, writes a fresh
// random token to a 0600 file, launches the capture helper through elfldr (as the overlay
// service is) and serves its socket. A crashed or wedged helper never takes the core down; a
// crashed core closes the socket, which makes the helper clean up and leave SceRemotePlay.
#include "capture_bc.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
#include "capture_io.h"
#include "game_audio.h"
#include "golive_opus.h"
#include "log.h"
#include "tls.h"

#define APP_ASSETS "/data/homebrew/PPSA99072/assets"
#define HELLO_SLOTS 8
#define HELLO_MS 500
#define COMMAND_MS 1000
#define STALE_MS 100

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; // conn_fd and the profile
static int conn_fd = -1;                                 // the authenticated helper, or -1
static int actual_w, actual_h, actual_fps;
static uint64_t last_key_ms;
static int key_pending;
static int first_media_ms = CAP_FIRST_MEDIA_MS, media_ms = CAP_MEDIA_MS, recovery_ms = CAP_RECOVERY_MS;
_Static_assert(CAP_FIRST_MEDIA_MS > CAP_STARTUP_MS + CAP_PICTURE_MS, "first-media budget too short");
_Static_assert(CAP_MEDIA_MS > CAP_PICTURE_MS, "media-progress budget too short");
_Static_assert(CAP_RECOVERY_MS > CAP_MODE_WAIT_MS + CAP_CLEANUP_BUDGET_MS + CAP_STARTUP_MS + CAP_PICTURE_MS,
               "recovery budget too short");
static capture_launcher launcher; // host tests replace the production loader
static const char *conn_file = CAP_CONN_FILE;
// quit waits 10 s for Go Live to end (main.c end_calls): the helper's cleanup is given 8 of them
static int connect_timeout_ms = CAP_CONNECT_MS, stop_timeout_ms = 8000;

static uint64_t mono_ms(void) { return cap_clock_ms(); }

void capture_set_launcher(capture_launcher l) { launcher = l; }
void capture_set_conn_file(const char *path) { conn_file = path; }
void capture_set_timeouts(int connect_ms, int stop_ms)
{
    connect_timeout_ms = connect_ms;
    stop_timeout_ms = stop_ms;
}

void capture_set_media_timeouts(int first_ms, int progress_ms, int recover_ms)
{
    first_media_ms = first_ms;
    media_ms = progress_ms;
    recovery_ms = recover_ms;
}

#ifdef __PROSPERO__
// The helper payload from the app's assets to elfldr (as overlay.c's send_elf).
static int send_helper(const capture_params *p, uint64_t deadline)
{
    const char *path = APP_ASSETS "/" CAP_ELF_NAME;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -2; // distinguish a missing asset from a loader/upload failure
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *elf = size > 0 && size < (8 << 20) ? malloc((size_t)size) : NULL;
    size_t n = elf ? fread(elf, 1, (size_t)size, f) : 0;
    fclose(f);
    int fd = socket(AF_INET, SOCK_STREAM, 0), ok = -1;
    struct sockaddr_in to = {.sin_len = sizeof to, .sin_family = AF_INET, .sin_port = htons(9021)}; // elfldr
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd >= 0 && size > 0 && n == (size_t)size &&
        !cap_connect(fd, (struct sockaddr *)&to, sizeof to, deadline, p->keep_going))
        ok = cap_upload(fd, (const uint8_t *)elf, n, deadline, p->keep_going);
    if (fd >= 0) {
        if (ok) {
            struct linger abort_upload = {.l_onoff = 1, .l_linger = 0};
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &abort_upload, sizeof abort_upload);
        }
        close(fd);
    }
    free(elf);
    LOG("capture: helper upload %s (%zu bytes, result %d)", path, n, ok);
    return ok;
}
#endif

void capture_request_keyframe(void)
{
    pthread_mutex_lock(&lock);
    const uint64_t now = mono_ms();
    if (conn_fd >= 0 && now - last_key_ms >= 200) { // viewers ask every second; a burst is one IDR
        last_key_ms = now;
        key_pending = 1; // the session worker is the only socket writer
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
        // Sound older than 100 ms when read is thrown away by the owner (this thread stalled: it
        // must not be encoded as current), and a drop, a discard or a reopened capture comes back
        // as `disc`: the half frame waiting is dropped, so two stretches that are not adjacent are
        // never joined inside one Opus frame.
        int disc = 0;
        const int n = ga_read2(a->id, pcm, sizeof pcm, STALE_MS, &disc);
        if (n < 0) { // the capture is down (it reopens itself): a gap, then a clean start
            down = 1;
            usleep(10000);
        } else if (n < 8) {
            usleep(4000);
        } else {
            if (down || disc)
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

// Fixed concurrent candidates: one slow or rejected peer never consumes a session budget.
struct hello {
    int fd;
    uint64_t deadline, order;
    size_t have;
    uint8_t bytes[CAP_HEADER_BYTES + 4 + CAP_TOKEN_BYTES];
};

static int hello_step(struct hello *c, const uint8_t token[CAP_TOKEN_BYTES])
{
    ssize_t n = recv(c->fd, c->bytes + c->have, sizeof c->bytes - c->have, MSG_DONTWAIT);
    if (!n) return -1;
    if (n < 0) return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    c->have += (size_t)n;
    if (c->have < sizeof c->bytes) return 0;
    cap_header h;
    if (cap_parse_header(c->bytes, &h) || h.type != CAP_HELLO || h.len != 4 + CAP_TOKEN_BYTES ||
        cap_get32(c->bytes + CAP_HEADER_BYTES) != CAP_VERSION) return -1;
    unsigned char diff = 0;
    for (unsigned i = 0; i < CAP_TOKEN_BYTES; i++)
        diff |= c->bytes[CAP_HEADER_BYTES + 4 + i] ^ token[i];
    return diff ? -1 : 1;
}

static void set_conn(int fd)
{
    pthread_mutex_lock(&lock);
    conn_fd = fd;
    last_key_ms = 0;
    key_pending = 0;
    pthread_mutex_unlock(&lock);
}

const char *capture_run(const capture_params *p)
{
    static char why_text[128];
    why_text[0] = 0;
    pthread_mutex_lock(&lock);
    actual_w = actual_h = actual_fps = 0;
    pthread_mutex_unlock(&lock);
    uint8_t token[CAP_TOKEN_BYTES];
    if (tls_random(NULL, token, sizeof token) != 0)
        return "Capture could not start.";
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // loopback only
    socklen_t al = sizeof a;
    if (listener < 0 || bind(listener, (struct sockaddr *)&a, sizeof a) != 0 || listen(listener, 16) != 0 || cap_nonblock(listener) != 0 ||
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
    int fd = -1, launches = 0, launched_ok = 0, helper_missing = 0;
    uint64_t begin = mono_ms(), next_launch = begin;
    uint64_t candidate_order = 0;
    struct hello candidates[HELLO_SLOTS];
    for (unsigned i = 0; i < HELLO_SLOTS; i++) candidates[i] = (struct hello){.fd = -1};
    // The helper: launched now and again if it has not called after 8 s (one still cleaning up
    // from the last Go Live holds its single-instance lock and exits; a second launch is harmless).
    while (fd < 0) {
        const uint64_t now = mono_ms();
        if (!p->keep_going()) {
            result = NULL;
            goto out;
        }
        if (now - begin >= (uint64_t)connect_timeout_ms) {
            if (!launched_ok && launches)
                result = helper_missing ? "The capture helper is missing." : "The capture helper could not be loaded.";
            goto out;
        }
        if (launches < CAP_LAUNCH_ATTEMPTS && now >= next_launch) {
            launches++;
            next_launch = now + CAP_LAUNCH_RETRY_MS;
            int launched = launcher ? launcher() : -1;
#ifdef __PROSPERO__
            if (!launcher) {
                uint64_t deadline = begin + (uint64_t)connect_timeout_ms;
                if (deadline > now + 3000) deadline = now + 3000;
                launched = send_helper(p, deadline);
            }
#endif
            if (launched == 0)
                launched_ok++;
            else {
                helper_missing |= launched == -2;
                LOG("capture: the helper could not be sent to the console");
            }
        }
        struct pollfd pf[1 + HELLO_SLOTS] = {{.fd = listener, .events = POLLIN}};
        for (unsigned i = 0; i < HELLO_SLOTS; i++)
            pf[i + 1] = (struct pollfd){.fd = candidates[i].fd, .events = POLLIN};
        int ready = poll(pf, 1 + HELLO_SLOTS, 25);
        if (ready < 0 && errno != EINTR) goto out;
        // Service established candidates before accepting another bounded batch.
        for (unsigned i = 0; i < HELLO_SLOTS; i++) {
            struct hello *c = &candidates[i];
            if (c->fd < 0) continue;
            int r = pf[i + 1].revents ? hello_step(c, token) : 0;
            // An inline loader upload may delay servicing; consume a complete buffered HELLO
            // before expiring an idle candidate.
            if (!r && mono_ms() >= c->deadline) r = -1;
            if (r > 0) { fd = c->fd; c->fd = -1; break; }
            if (r < 0) { close(c->fd); c->fd = -1; }
        }
        if (fd >= 0) break;
        if (pf[0].revents & POLLIN) for (unsigned accepted = 0; accepted < HELLO_SLOTS; accepted++) {
            int c = accept(listener, NULL, NULL);
            if (c < 0) break;
            if (cap_nonblock(c)) { close(c); continue; }
            int one = 1; // commands (keyframe, stop) go out at once too, as the helper's frames do
            (void)setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            unsigned slot = 0;
            for (unsigned i = 0; i < HELLO_SLOTS; i++) {
                if (candidates[i].fd < 0) { slot = i; break; }
                if (candidates[i].order < candidates[slot].order) slot = i;
            }
            if (candidates[slot].fd >= 0) {
                // A complete HELLO may have arrived after poll() and before this accept batch.
                if (hello_step(&candidates[slot], token) > 0) {
                    fd = candidates[slot].fd; candidates[slot].fd = -1;
                    close(c); break;
                }
                close(candidates[slot].fd);
            }
            candidates[slot] = (struct hello){.fd = c, .deadline = mono_ms() + HELLO_MS, .order = ++candidate_order};
        }
    }
    for (unsigned i = 0; i < HELLO_SLOTS; i++) {
        if (candidates[i].fd >= 0) close(candidates[i].fd);
        candidates[i].fd = -1;
    }
    close(listener); // no one else may connect now
    listener = -1;
    unlink(conn_file);
    LOG("capture: helper connected after %llu ms", (unsigned long long)(mono_ms() - begin));

    uint8_t tx[CAP_HEADER_BYTES + 24];
    size_t tx_len = sizeof tx, tx_offset = 0;
    uint64_t tx_at = mono_ms();
    {
        uint8_t *m = tx;
        cap_pack_header(m, CAP_START, 0, 24);
        const uint32_t selector = p->height >= 1080 ? 4 : 3, flag = p->fps >= 60;
        cap_put32(m + 8, selector), cap_put32(m + 12, flag), cap_put32(m + 16, (uint32_t)p->bitrate);
        cap_put32(m + 20, (uint32_t)p->width), cap_put32(m + 24, (uint32_t)p->height), cap_put32(m + 28, (uint32_t)p->fps);
    }
    set_conn(fd);

    uint8_t *in = malloc(CAP_HEADER_BYTES + CAP_MAX_PAYLOAD);
    if (!in) {
        result = "Capture could not start.";
        goto out;
    }
    size_t have = 0;
    int stopping = 0, was_recovering = 0, live = 0;
    uint64_t stop_at = 0, media_at = mono_ms();
    int profile_seen = 0, headers_seen = 0, idr_seen = 0;
    result = "Capture stopped.";
    int stop_sent = 0;
    for (;;) {
        if (!stopping && !p->keep_going()) {
            stopping = 1;
            stop_at = mono_ms();
        }
        if (stopping && mono_ms() - stop_at > (uint64_t)stop_timeout_ms) {
            LOG("capture: the helper did not finish in %d ms; closing on it", stop_timeout_ms);
            break;
        }
        if (!stopping && mono_ms() - media_at >= (uint64_t)(was_recovering ? recovery_ms :
                            profile_seen && headers_seen && idr_seen ? media_ms : first_media_ms)) {
            snprintf(why_text, sizeof why_text, "The capture helper stopped responding.");
            break;
        }
        // An entirely unsent command can be replaced by STOP. A partial header must finish
        // first, otherwise the receiver would parse STOP bytes as the remainder of that frame.
        if (stopping && tx_len && !tx_offset && tx[0] != CAP_STOP) tx_len = 0;
        if (!tx_len) {
            int type = 0;
            pthread_mutex_lock(&lock);
            if (stopping && !stop_sent) { type = CAP_STOP; stop_sent = 1; key_pending = 0; }
            else if (!stopping && key_pending) { type = CAP_KEYFRAME; key_pending = 0; }
            pthread_mutex_unlock(&lock);
            if (type) {
                cap_pack_header(tx, (uint8_t)type, 0, 0);
                tx_len = CAP_HEADER_BYTES; tx_offset = 0; tx_at = mono_ms();
            }
        }
        if (tx_len) {
            int r = cap_write_step(fd, tx, tx_len, &tx_offset);
            if (r < 0 || mono_ms() - tx_at >= COMMAND_MS) {
                snprintf(why_text, sizeof why_text, "The capture helper could not receive a command.");
                break;
            }
            if (r > 0) tx_len = tx_offset = 0;
        }
        struct pollfd pf = {.fd = fd, .events = POLLIN | (tx_len ? POLLOUT : 0)};
        int r = poll(&pf, 1, 25);
        if (r < 0 && errno != EINTR) break;
        if (r > 0 && pf.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            ssize_t n = recv(fd, in + have, CAP_HEADER_BYTES + CAP_MAX_PAYLOAD - have, MSG_DONTWAIT);
            if (!n || (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) break;
            if (n > 0) have += (size_t)n;
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
                if (!h.len || !profile_seen || !headers_seen) { bad = 1; break; }
                if (!idr_seen && !(h.flags & CAP_KEY)) break;
                if (h.flags & CAP_KEY) {
                    idr_seen = 1;
                    if (was_recovering) {
                        was_recovering = 0;
                        capture_request_keyframe();
                    }
                }
                media_at = mono_ms();
                if (!live) { live = 1; audio.live = 1; if (p->live) p->live(); }
                if (p->video)
                    p->video(body, h.len, h.flags & CAP_KEY);
                break;
            case CAP_HEADERS: // SPS and PPS as an access unit of their own: stream.c keeps them for the IDRs
                if (!h.len) { bad = 1; break; }
                headers_seen = 1;
                if (p->video)
                    p->video(body, h.len, 0);
                break;
            case CAP_PROFILE:
                if (h.len == 16) {
                    profile_seen = 1;
                    pthread_mutex_lock(&lock);
                    actual_w = (int)cap_get32(body), actual_h = (int)cap_get32(body + 4), actual_fps = (int)cap_get32(body + 8);
                    pthread_mutex_unlock(&lock);
                    LOG("capture: encoder runs %dx%d at %d fps (output mode 0x%x)", actual_w, actual_h, actual_fps,
                        cap_get32(body + 12));
                } else bad = 1;
                break;
            case CAP_STATE:
                if (h.len >= 1 && body[0] == CAP_LIVE) {
                    // Only a complete IDR, profile and headers prove media progress.
                } else if (h.len >= 1 && body[0] == CAP_RECOVERING && !was_recovering && live) {
                    was_recovering = 1;
                    profile_seen = headers_seen = idr_seen = 0;
                    media_at = mono_ms();
                } else if (h.len >= 1 && body[0] == CAP_ERROR) {
                    size_t t = h.len - 1 < sizeof why_text - 1 ? h.len - 1 : sizeof why_text - 1;
                    for (size_t i = 0; i < t; i++) // text from another process: printable ASCII only
                        why_text[i] = body[1 + i] >= 32 && body[1 + i] < 127 ? (char)body[1 + i] : '?';
                    why_text[t] = 0;
                    LOG("capture: helper error: %s", why_text);
                    bad = 2; // an explicit error is terminal; do not overwrite it with a later timeout
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
            if (bad == 1)
                LOG("capture: a bad message from the helper, closing");
            break;
        }
    }
    free(in);
    // An explicit helper error survives Stop (especially failed cleanup). Otherwise EOF
    // after an intentional stop is normal.
    result = why_text[0] ? why_text : stopping || !p->keep_going() ? NULL : "Capture stopped.";
out:
    for (unsigned i = 0; i < HELLO_SLOTS; i++)
        if (candidates[i].fd >= 0) close(candidates[i].fd);
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
