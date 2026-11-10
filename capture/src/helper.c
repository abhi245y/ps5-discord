// SPDX-License-Identifier: GPL-3.0-only
// discord-capture.elf: the wireless Go Live backend's helper. It borrows the console's own
// hardware H.264 encoder (Avcap2) through calls made inside SceRemotePlay, with the sequence
// the PoC (ps5-encoder-poc, tools/encoder-probe/capture_record.c) proved on the console, and
// sends the video over a loopback socket to the core (src/capture_bc.c, protocol in
// src/capture_proto.h). It starts no Remote Play client session, takes no controller and
// changes no display setting. Video only: the core captures the game's sound itself.
//
// Process model (as the PoC): the main process is a small supervisor; the work runs in a forked
// child on a 2 MiB thread stack. The supervisor's watchdog asks the child to clean up after 15 s
// without progress and kills only the child after 25 s. There is no total duration: Go Live is
// open-ended, the core ends it by a CAP_STOP or by closing the socket.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <time.h>
#include <unistd.h>

#include "capture_proto.h"
#include "inject.h"
#include "inject_broker.h"
#include "h264_timing.h"
#ifdef SL_PS5
int sceKernelSetProcessName(const char *name); // the SDK's kernel header does not declare it
#endif

#ifndef CAP_DIR
#define CAP_DIR "/data/ps5-discord"
#endif
#ifndef CONN_FILE
#define CONN_FILE CAP_CONN_FILE
#endif
#ifndef LOCK_PATH
#define LOCK_PATH CAP_LOCK_FILE
#endif
#ifndef LOG_PATH
#define LOG_PATH CAP_DIR "/capture.log"
#endif
// Mode 0x40 is the output mode Glint treats as 120 Hz. The PoC refused it (its baseline) and
// only experiments allowed it; no capture was ever shown to work in it, so production refuses.
#ifndef ALLOW_MODE_40
#define ALLOW_MODE_40 0
#endif
#define FRAME_CAP 0x100000u
#define FEATURE 12u
#ifndef START_WAIT_MS
#define START_WAIT_MS CAP_STARTUP_MS
#endif
#define SEND_STALL_MS 5000u
#ifndef MODE_POLL_MS
#define MODE_POLL_MS 1000u
#endif
#ifndef RECOVERY_MAX
#define RECOVERY_MAX 4
#define RECOVERY_WINDOW_MS 60000
#endif
#ifndef NO_PICTURE_MS
#define NO_PICTURE_MS CAP_PICTURE_MS
#endif
#ifndef MODE_WAIT_MS
#define MODE_WAIT_MS CAP_MODE_WAIT_MS // a transitional or changing output mode is waited out this long
#endif

enum function {
    INITIALIZE, TERMINATE, MODE, ENABLE_SCAN, OPEN_VIDEO, START, STOP, CLOSE, BITRATE_SET, HEADERS,
    KEYFRAME, READ_VIDEO, SET_CALLBACK, CHANGE_STATE, CLEAR_CALLBACK, REMOTE_GETPID, REMOTE_MALLOC,
    REMOTE_FREE, FUNCTION_COUNT
};
static const char *const symbols[FUNCTION_COUNT] = {
    "sceAvcap2Initialize", "sceAvcap2Terminate", "sceAvcap2GetVideoOutMode",
    "sceAvcap2EnableScanInEncode", "sceAvcap2OpenVideo", "sceAvcap2Start", "sceAvcap2Stop",
    "sceAvcap2Close", "sceAvcap2SetVideoBitrate", "sceAvcap2GetNonVclNalUnitStream",
    "sceAvcap2RequestKeyFrame", "sceAvcap2ReadVideo", "sceResourceArbitratorSetCallback",
    "sceResourceArbitratorChangeState", "sceResourceArbitratorClearCallback", "getpid", "malloc", "free"
};

struct progress { _Atomic int function, target; _Atomic uint64_t heartbeat_ms; };
static struct progress *progress;
static _Atomic int quit;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal cancellation must be lock-free");
static FILE *log_file;

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void beat(void)
{
    if (progress) atomic_store(&progress->heartbeat_ms, now_ms());
}

static void log_line(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
#ifdef CAPTURE_QUIET
    FILE *files[] = {log_file};
#else
    FILE *files[] = {stderr, log_file};
#endif
    for (unsigned i = 0; i < sizeof files / sizeof *files; i++)
        if (files[i]) {
            fprintf(files[i], "[capture pid=%d ms=%llu] %s\n", getpid(), (unsigned long long)now_ms(), line);
            fflush(files[i]);
        }
}

// ---- the socket to the core ---------------------------------------------------------------

static int net_fd = -1;
static int net_dead;          // the core closed, or the socket failed: end the session
static int stop_requested;    // CAP_STOP
static int key_requested;     // CAP_KEYFRAME, not yet passed to the encoder
static uint8_t inbox[64];
static size_t inbox_len;

// Closing with unread data from the core would send a reset and could discard the last state
// message we just wrote: finish our side, drain theirs briefly, then close.
static void net_close(void)
{
    if (net_fd >= 0) {
        uint8_t sink[256];
        shutdown(net_fd, SHUT_WR);
        for (unsigned i = 0; i < 4; i++) {
            struct pollfd pf = {.fd = net_fd, .events = POLLIN};
            if (poll(&pf, 1, 50) <= 0 || recv(net_fd, sink, sizeof sink, MSG_DONTWAIT) <= 0) break;
        }
        close(net_fd);
    }
    net_fd = -1;
}

// Everything is sent whole or the connection counts as dead. Never blocks for ever: the
// supervisor's watchdog would kill the worker mid-session otherwise.
static int net_send_raw(const uint8_t *p, size_t n)
{
    uint64_t begin = now_ms();
    while (n && !net_dead) {
        ssize_t w = send(net_fd, p, n, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (w > 0) { p += w; n -= (size_t)w; beat(); continue; }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { net_dead = 1; break; }
        if (now_ms() - begin > SEND_STALL_MS) { log_line("socket send stalled"); net_dead = 1; break; }
        struct pollfd pf = {.fd = net_fd, .events = POLLOUT};
        poll(&pf, 1, 50);
        beat();
    }
    return net_dead ? -1 : 0;
}

// Header and payload; payload may be NULL when len is 0.
static int net_send(uint8_t type, uint8_t flags, const uint8_t *payload, uint32_t len)
{
    uint8_t h[CAP_HEADER_BYTES];
    cap_pack_header(h, type, flags, len);
    if (net_fd < 0 || net_dead) return -1;
    if (net_send_raw(h, sizeof h) || (len && net_send_raw(payload, len))) return -1;
    return 0;
}

// A video frame read straight behind its own header, one send.
static int net_send_inplace(uint8_t *buf, uint8_t type, uint8_t flags, uint32_t len)
{
    cap_pack_header(buf, type, flags, len);
    return net_fd < 0 || net_dead ? -1 : net_send_raw(buf, CAP_HEADER_BYTES + len);
}

static int net_state(enum cap_state s, const char *text)
{
    uint8_t m[1 + 120];
    size_t t = text ? strlen(text) : 0;
    if (t > 120) t = 120;
    m[0] = (uint8_t)s;
    if (t) memcpy(m + 1, text, t);
    return net_send(CAP_STATE, 0, m, (uint32_t)(1 + t));
}

struct start_req { uint32_t selector, fps_flag, bitrate, width, height, fps; };

// What the core sent, if anything, without waiting. Returns when the socket has nothing more.
// Only three messages exist and all are tiny; anything else ends the connection.
static void net_poll(struct start_req *start, int *got_start, int wait_ms)
{
    while (net_fd >= 0 && !net_dead) {
        struct pollfd pf = {.fd = net_fd, .events = POLLIN};
        int r = poll(&pf, 1, wait_ms);
        wait_ms = 0;
        if (r <= 0) return;
        ssize_t n = recv(net_fd, inbox + inbox_len, sizeof inbox - inbox_len, MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { net_dead = 1; return; }
        if (n < 0) return;
        inbox_len += (size_t)n;
        for (;;) {
            cap_header h;
            if (inbox_len < CAP_HEADER_BYTES) break;
            if (cap_parse_header(inbox, &h) || h.len > sizeof inbox - CAP_HEADER_BYTES) { net_dead = 1; return; }
            if (inbox_len < CAP_HEADER_BYTES + h.len) break;
            const uint8_t *p = inbox + CAP_HEADER_BYTES;
            if (h.type == CAP_STOP) stop_requested = 1;
            else if (h.type == CAP_KEYFRAME) key_requested = 1;
            else if (h.type == CAP_START && h.len == 24 && start && !*got_start) {
                start->selector = cap_get32(p), start->fps_flag = cap_get32(p + 4);
                start->bitrate = cap_get32(p + 8), start->width = cap_get32(p + 12);
                start->height = cap_get32(p + 16), start->fps = cap_get32(p + 20);
                *got_start = 1;
            } else { net_dead = 1; return; }
            memmove(inbox, inbox + CAP_HEADER_BYTES + h.len, inbox_len - CAP_HEADER_BYTES - h.len);
            inbox_len -= CAP_HEADER_BYTES + h.len;
        }
    }
}

// "<port> <token hex>\n", written by the core before it launched us.
static int net_connect(void)
{
    char text[80] = {0};
    FILE *f = fopen(CONN_FILE, "r");
    if (!f) { log_line("no %s", CONN_FILE); return -1; }
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    unsigned port = 0;
    char hex[2 * CAP_TOKEN_BYTES + 1] = {0};
    if (sscanf(text, "%u %32s", &port, hex) != 2 || !port || port > 65535 || strlen(hex) != 2 * CAP_TOKEN_BYTES) {
        log_line("bad connection file");
        return -1;
    }
    uint8_t hello[4 + CAP_TOKEN_BYTES];
    cap_put32(hello, CAP_VERSION);
    for (unsigned i = 0; i < CAP_TOKEN_BYTES; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return -1;
        hello[4 + i] = (uint8_t)v;
    }
    net_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (net_fd < 0 || connect(net_fd, (struct sockaddr *)&a, sizeof a) != 0) {
        log_line("connect to the core failed (errno %d)", errno);
        net_close();
        return -1;
    }
    // Console 2026-11-09: frames left the encoder every 33 ms but reached the core in pairs
    // (Nagle holding each frame's tail until the previous one was acknowledged). Send at once.
    int one = 1;
    (void)setsockopt(net_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return net_send(CAP_HELLO, 0, hello, sizeof hello);
}

// ---- Annex-B checks (derived from the PoC's record_file.c) ---------------------------------

static uint32_t le32(const uint8_t *p) { return cap_get32(p); }

static int has_nal(const uint8_t *p, size_t n, unsigned type)
{
    for (size_t i = 0; i + 3 < n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1 && (p[i + 3] & 31) == type) return 1;
    return 0;
}

static int annexb_ok(const uint8_t *p, size_t n)
{
    return n >= 4 && p[0] == 0 && p[1] == 0 && (p[2] == 1 || (n > 4 && p[2] == 0 && p[3] == 1));
}

// The real SPS/PPS from the encoder's header blob (SPS length at 0x10, bytes at 0x18; PPS
// length at 0x98, bytes at 0xa0) as Annex-B, or -1. Nothing is guessed or hardcoded.
static int headers_annexb(const uint8_t *h, size_t n, uint8_t *out, size_t cap, size_t *len)
{
    if (n < 0xa4) return -1;
    uint32_t sps = le32(h + 0x10), pps = le32(h + 0x98);
    if (sps < 5 || sps > 128 || pps < 2 || pps > 128 || 0x18u + sps > n || 0xa0u + pps > n ||
        (h[0x18] & 31) != 7 || (h[0xa0] & 31) != 8 || cap < 8u + sps + pps) return -1;
    static const uint8_t prefix[] = {0, 0, 0, 1};
    size_t m = 0;
    memcpy(out + m, prefix, 4); m += 4;
    memcpy(out + m, h + 0x18, sps); m += sps;
    memcpy(out + m, prefix, 4); m += 4;
    memcpy(out + m, h + 0xa0, pps); m += pps;
    *len = m;
    return 0;
}

// ---- the capture session in SceRemotePlay --------------------------------------------------

struct capture {
    sl_inject ctx;
    intptr_t fn[FUNCTION_COUNT];
    uint64_t video, remote_frame, startup_deadline;
    int startup_done, video_unknown;
    int initialized, callback, video_open, video_start_attempted, cleanup, failed, cleanup_failed;
    uint32_t output_mode, final_mode;
};

// One result per session; the supervisor never conflates capture and cleanup.
struct session_result {
    uint64_t begin_ms, last_video_ms;
    unsigned frames;
    uint32_t api_error, initial_mode, final_mode, stable_mode;
    int cleanup_ok, transport_failed, refused_mode;
    char why[100];
};
static struct session_result segment;

static int64_t remote(struct capture *c, enum function id, const uint64_t args[6])
{
    if (!c->cleanup) {
        net_poll(NULL, &(int){1}, 0);
        if (quit || net_dead || stop_requested || (!c->startup_done && now_ms() >= c->startup_deadline))
            return SL_CALL_ERROR;
    }
    if (c->ctx.quarantined && !(c->cleanup && c->ctx.aborted_restored)) { c->failed = 1; return SL_CALL_ERROR; }
    if (progress) progress->function = id + 1;
    if (id != READ_VIDEO && (id != MODE || !c->startup_done)) log_line("calling %s", symbols[id]);
    // Preserve the pinned borrowed LWP's general and extended CPU state across media calls.
    int64_t rc = c->cleanup ? sl_inject_cleanup_call(&c->ctx, c->fn[id], args) :
                            inj_call_fast(&c->ctx, c->fn[id], args, 1);
    if (progress) progress->function = 0;
    beat();
    if (id != READ_VIDEO && (id != MODE || !c->startup_done)) log_line("%s returned 0x%llx", symbols[id], (unsigned long long)rc);
    if (rc == SL_CALL_ERROR) {
        log_line("transport failure in %s; stage=%s quarantined=%d", symbols[id],
                 c->ctx.stage ? c->ctx.stage : "unknown", c->ctx.quarantined);
        c->failed = 1;
    }
    return rc;
}
#define REMOTE(c, id, ...) remote(c, id, (const uint64_t[6]){__VA_ARGS__})
static int good(int64_t rc) { return rc != SL_CALL_ERROR && (int32_t)rc >= 0; }
static void put32(uint8_t *p, size_t at, uint32_t n) { memcpy(p + at, &n, 4); }
static void put64(uint8_t *p, size_t at, uint64_t n) { memcpy(p + at, &n, 8); }

// Offsets in Glint's scratch block include its leading output-session qword. Selector 3 is
// 720p and 4 is 1080p (native firmware construction at 0x6f8fe/0x6f964); the fps flag is 0 for
// 30 and 1 for 60. Width, height, rate budget and the actual output mode are written
// independently: changing the dimensions alone does not suffice.
static void video_params(uint8_t p[0x2e8], uint32_t mode, const struct start_req *r)
{
    memset(p, 0, 0x2e8);
    put32(p, 8, 0x2e0); put32(p, 0x10, 0x28);
    put32(p, 0x1c, 2);
    put32(p, 0x20, r->selector);
    put32(p, 0x24, r->fps_flag);
    put32(p, 0x2c, 0); // H.264
    put32(p, 0x30, FEATURE);
    put64(p, 0xc0, 0xe0); put64(p, 0xc8, 0x20);
    put32(p, 0xd0, r->bitrate); put32(p, 0xd4, r->bitrate);
    put32(p, 0xd8, (uint32_t)((r->bitrate * 1001ull + r->fps * 1000u - 1) / (r->fps * 1000u)));
    put32(p, 0xdc, 18); put32(p, 0xe0, 0x33);
    put64(p, 0x1f0, 0xf8); put64(p, 0x1f8, 0x28);
    put32(p, 0x210, r->width); put32(p, 0x214, r->height);
    put64(p, 0x220, 0x88); memset(p + 0x228, 0xff, 0x80);
    put32(p, 0x2a8, mode); put32(p, 0x2ac, 2);
    put64(p, 0x2b0, 0x10); put32(p, 0x2b4, 1); p[0x2bc] = 1;
}

static int bind_functions(struct capture *c)
{
    for (unsigned i = 0; i < FUNCTION_COUNT; i++) {
        const char *module = i >= REMOTE_MALLOC ? "libSceLibcInternal.sprx" :
                             i == REMOTE_GETPID ? "libkernel_sys.sprx" :
                             i >= SET_CALLBACK ? "libSceResourceArbitrator.sprx" : "libSceAvcap2.sprx";
        c->fn[i] = sl_resolve_in(c->ctx.target_pid, module, symbols[i]);
        if (c->fn[i] <= 0) { log_line("missing %s in %s", symbols[i], module); return -1; }
    }
    return 0;
}

static int read_mode(struct capture *c, uint32_t *mode)
{
    intptr_t at = c->ctx.scratch + 0x700;
    *mode = 0;
    if (sl_inject_write_at(&c->ctx, at, mode, sizeof *mode) || !good(REMOTE(c, MODE, at)) ||
        sl_inject_read_at(&c->ctx, at, mode, sizeof *mode)) return -1;
    return 0;
}

// Poll the existing output-mode query every 100 ms for at most budget_ms; three equal nonzero
// readings are stable. The values are opaque and identify neither HDR nor refresh rate.
static int wait_stable_mode(struct capture *c, uint32_t *stable, unsigned budget_ms)
{
    uint64_t deadline = now_ms() + budget_ms;
    uint32_t previous = 0;
    unsigned equal = 0;
    log_line("WAIT_STABLE (%u ms, three nonzero equal samples)", budget_ms);
    while (!quit && !net_dead && !stop_requested && !c->ctx.quarantined && now_ms() < deadline) {
        uint32_t mode;
        if (read_mode(c, &mode)) return -1;
        log_line("output mode sample=0x%x", mode);
        equal = mode && mode == previous ? equal + 1 : mode ? 1 : 0;
        previous = mode;
        if (equal >= 3) { *stable = mode; return 0; }
        usleep(100000);
        beat();
        net_poll(NULL, &(int){1}, 0);
    }
    log_line("stable mode unavailable within the deadline");
    return -1;
}

static void cleanup(struct capture *c)
{
    c->cleanup = 1;
    if (!c->ctx.quarantined || c->ctx.aborted_restored) {
        if (c->video_open) {
            if (c->video_start_attempted && !good(REMOTE(c, STOP, c->video))) c->failed = c->cleanup_failed = 1;
            if (!good(REMOTE(c, CLOSE, c->video))) c->failed = c->cleanup_failed = 1;
        }
        // free() has no return value; only transport completion is meaningful.
        if (c->remote_frame && REMOTE(c, REMOTE_FREE, c->remote_frame) == SL_CALL_ERROR) c->failed = c->cleanup_failed = 1;
        if (c->output_mode && !c->ctx.quarantined) {
            uint32_t after = 0;
            if (read_mode(c, &after)) c->failed = c->cleanup_failed = 1;
            else {
                c->final_mode = after;
                log_line("HDMI mode before=0x%x after=0x%x", c->output_mode, after);
            }
        }
        if (c->callback && !good(REMOTE(c, CLEAR_CALLBACK, FEATURE))) c->failed = c->cleanup_failed = 1;
        if (c->initialized && !good(REMOTE(c, TERMINATE))) c->failed = c->cleanup_failed = 1;
    } else {
        c->failed = c->cleanup_failed = 1;
        log_line("remote calls quarantined; service cleanup incomplete, reboot may be needed");
    }
    if (c->video_unknown) {
        c->failed = c->cleanup_failed = 1;
        log_line("OpenVideo succeeded but ownership is uncertain; Terminate is not a verified Close");
    }
    if (sl_inject_end(&c->ctx)) { log_line("detach failed"); c->failed = c->cleanup_failed = 1; }
}

static void why(const char *text) { if (!segment.why[0]) snprintf(segment.why, sizeof segment.why, "%s", text); }

enum session_end { SESS_STOPPED, SESS_RECOVERABLE, SESS_FAILED };

// One capture session: attach, open the encoder, stream until the core stops it or something
// fails, then tear everything down. `may_recover` lets the 0x81950009 path wait for a stable
// output mode (RECOVERY.md); the caller decides whether to open another session.
static enum session_end capture_session(const struct start_req *req, int may_recover)
{
    memset(&segment, 0, sizeof segment);
    struct capture c = {.startup_deadline = now_ms() + START_WAIT_MS};
    uint8_t *frame = NULL;
    int first_idr = 0;
    int64_t window_gap = 0, last_read_us = 0;
    size_t window_max = 0, window_idr = 0;
    unsigned gaps[CAP_GAP_BUCKETS] = {0};
    enum session_end end = SESS_FAILED;
    uint8_t headers[0x200], annexb[512];
    size_t annexb_len = 0;

    if (sl_inject_begin("SceRemotePlay", &c.ctx)) {
        log_line("attach failed: stage=%s errno=%d rip=0x%llx lwp=%d", c.ctx.stage, c.ctx.last_errno,
                 (unsigned long long)c.ctx.last_rip, (int)c.ctx.thread_pid);
        why("Capture could not start.");
        goto done;
    }
    if (progress) progress->target = c.ctx.target_pid;
    log_line("attached: pid=%d lwp=%d rip=0x%llx scratch=0x%llx", (int)c.ctx.target_pid, (int)c.ctx.thread_pid,
             (unsigned long long)c.ctx.last_rip, (unsigned long long)c.ctx.scratch);
    if (c.ctx.scratch <= 0 || c.ctx.scratch_len < 0x4000 || bind_functions(&c)) {
        log_line("setup failed: scratch=0x%llx len=0x%zx stage=%s", (unsigned long long)c.ctx.scratch,
                 (size_t)c.ctx.scratch_len, c.ctx.stage);
        why("Capture could not start."); goto done;
    }
    if (!good(REMOTE(&c, INITIALIZE))) { log_line("Initialize failed: stage=%s", c.ctx.stage); why("Capture could not start."); goto done; }
    c.initialized = 1;
    uint32_t mode = 0;
    intptr_t base = c.ctx.scratch;
    // A transitional mode zero (a game starting or ending) is waited out, not captured in.
    if (wait_stable_mode(&c, &mode, MODE_WAIT_MS)) { why("The display is changing. Try again."); goto done; }
    log_line("HDMI output mode=0x%x; display settings will not be changed", mode);
    c.output_mode = segment.initial_mode = mode;
    if (mode == 0x40 && !ALLOW_MODE_40) {
        log_line("refusing the known 120 Hz output mode");
        segment.refused_mode = 1;
        why("Go Live can't capture while the console outputs 120 Hz.");
        goto done;
    }
    if (!good(REMOTE(&c, SET_CALLBACK, FEATURE, c.fn[REMOTE_GETPID]))) goto done;
    c.callback = 1;
    static const char running[] = "RUNNING";
    if (sl_inject_write_at(&c.ctx, base + 0x600, running, sizeof running) ||
        !good(REMOTE(&c, CHANGE_STATE, FEATURE, base + 0x600)) ||
        !good(REMOTE(&c, ENABLE_SCAN))) goto done;
    uint8_t video[0x2e8];
    video_params(video, mode, req);
    if (sl_inject_write_at(&c.ctx, base, video, sizeof video) ||
        !good(REMOTE(&c, OPEN_VIDEO, base, base + 8, 0))) { why("The console's encoder would not open."); goto done; }
    // Read failures after a successful open still need cleanup; use the returned handle only
    // after a complete eight-byte transfer.
    c.video_unknown = 1;
    if (sl_inject_read_at(&c.ctx, base, &c.video, 8) || !c.video) {
        // Retry once while the target is stopped; never use a partial or zero handle.
        c.video = 0;
        if (sl_inject_read_at(&c.ctx, base, &c.video, 8) || !c.video) {
            c.cleanup_failed = 1;
            why("The encoder opened but its handle could not be read.");
            goto done;
        }
    }
    c.video_unknown = 0;
    c.video_open = 1;
    // The bitrate is set once, before Start: a change mid-session wedges the encoder until
    // the console restarts.
    uint8_t rate[16] = {0};
    put64(rate, 0, 16); put32(rate, 8, req->bitrate); put32(rate, 12, req->bitrate);
    if (sl_inject_write_at(&c.ctx, base + 0x780, rate, sizeof rate) ||
        !good(REMOTE(&c, BITRATE_SET, c.video, base + 0x780))) goto done;
    c.video_start_attempted = 1;
    if (!good(REMOTE(&c, START, c.video))) { why("The console's encoder would not start."); goto done; }

    int headers_ready = 0;
    for (unsigned i = 0; i < 20 && !quit && !net_dead && !stop_requested && now_ms() < c.startup_deadline; i++) {
        net_poll(NULL, &(int){1}, 0);
        if (quit || net_dead || stop_requested) goto done;
        memset(headers, 0, sizeof headers);
        if (sl_inject_write_at(&c.ctx, base + 0x2000, headers, sizeof headers) ||
            !good(REMOTE(&c, HEADERS, c.video, base + 0x2000)) ||
            sl_inject_read_at(&c.ctx, base + 0x2000, headers, sizeof headers)) goto done;
        if (!headers_annexb(headers, sizeof headers, annexb, sizeof annexb, &annexb_len)) { headers_ready = 1; break; }
        usleep(50000);
        beat();
    }
    if (!headers_ready || quit || net_dead || stop_requested || now_ms() >= c.startup_deadline) { why("The encoder gave no picture headers."); goto done; }
    if (capture_h264_timing(annexb, &annexb_len, sizeof annexb, req->fps)) {
        why("The encoder gave unsupported picture timing.");
        goto done;
    }
    uint8_t profile[16];
    cap_put32(profile, req->width); cap_put32(profile + 4, req->height);
    cap_put32(profile + 8, req->fps); cap_put32(profile + 12, mode);
    if (net_send(CAP_PROFILE, 0, profile, sizeof profile) ||
        net_send(CAP_HEADERS, 0, annexb, (uint32_t)annexb_len)) goto done;
    int64_t allocated = REMOTE(&c, REMOTE_MALLOC, FRAME_CAP);
    if (allocated == SL_CALL_ERROR || allocated <= 0) goto done;
    c.remote_frame = (uint64_t)allocated;
    frame = malloc(CAP_HEADER_BYTES + FRAME_CAP);
    if (!frame || !good(REMOTE(&c, KEYFRAME, c.video))) goto done;
    net_poll(NULL, &(int){1}, 0);
    if (quit || net_dead || stop_requested || now_ms() >= c.startup_deadline) goto done;
    segment.begin_ms = now_ms();
    uint64_t last_key = segment.begin_ms, next_mode = segment.begin_ms;
    c.startup_done = 1;
    uint64_t last_picture = segment.begin_ms;
    uint32_t observed_mode = c.output_mode;
    unsigned mode_misses = 0;
    while (!quit && !net_dead && !stop_requested) {
        net_poll(NULL, &(int){1}, 0);
        if (net_dead || stop_requested) break;
        if (now_ms() >= next_mode) {
            uint32_t current = 0;
            int mode_ok = read_mode(&c, &current) == 0;
            next_mode = now_ms() + MODE_POLL_MS;
            if (!mode_ok) {
                if (c.failed || c.ctx.quarantined || ++mode_misses >= 3) {
                    why("The display mode could not be checked."); goto done;
                }
                log_line("output mode query missed (%u of 3); picture reads continue", mode_misses);
            } else {
                mode_misses = 0;
                if (current == 0x40 && !ALLOW_MODE_40) {
                    segment.refused_mode = 1;
                    why("Go Live can't capture while the console outputs 120 Hz.");
                    goto done;
                }
                // Transitions may precede the validated ReadVideo error. Keep reading so the
                // existing one-recovery path can run; only 0x40 is refused immediately.
                if (current != observed_mode) {
                    log_line("output mode changed 0x%x -> 0x%x; waiting for picture progress", observed_mode, current);
                    observed_mode = current;
                }
            }
        }
        if (key_requested) {
            key_requested = 0;
            if (!good(REMOTE(&c, KEYFRAME, c.video))) goto done;
        }
        // No IDR yet after a second: ask again. No picture in ten: the encoder is not working.
        if (!first_idr && now_ms() - last_key >= 1000) {
            if (now_ms() - segment.begin_ms > NO_PICTURE_MS) { why("The encoder produced no picture."); goto done; }
            last_key = now_ms();
            if (!good(REMOTE(&c, KEYFRAME, c.video))) goto done;
        }
        if (now_ms() - last_picture >= NO_PICTURE_MS) { why("The picture stopped."); goto done; }
        int64_t n = REMOTE(&c, READ_VIDEO, c.video, c.remote_frame, FRAME_CAP, base + 0x300, 0);
        if (n == SL_CALL_ERROR || n < 0 || n > FRAME_CAP) {
            log_line("ReadVideo failed: 0x%llx", (unsigned long long)n);
            if (n != SL_CALL_ERROR && (int32_t)n < 0) segment.api_error = (uint32_t)n;
            // Only the observed API error is a recovery candidate. Oversize frames, unknown
            // errors and quarantined transport never are.
            if (may_recover && segment.frames && segment.api_error == 0x81950009u && !c.ctx.quarantined) {
                net_state(CAP_RECOVERING, "");
                (void)wait_stable_mode(&c, &segment.stable_mode, MODE_WAIT_MS);
            }
            why("The picture stopped.");
            goto done;
        }
        if (n < 5) {
            usleep(1000);
            continue;
        }
        if (sl_inject_read_at(&c.ctx, (intptr_t)c.remote_frame, frame + CAP_HEADER_BYTES, (size_t)n)) goto done;
        uint8_t *au = frame + CAP_HEADER_BYTES;
        if (!annexb_ok(au, (size_t)n)) { log_line("invalid frame (not Annex-B)"); why("The encoder sent a bad picture."); goto done; }
        const int idr = has_nal(au, (size_t)n, 5);
        if (!first_idr && !idr) continue; // wait for the first IDR; nothing earlier is decodable
        if (!first_idr) {
            first_idr = 1;
            if (net_state(CAP_LIVE, "")) goto done;
        }
        size_t au_len = (size_t)n;
        if (has_nal(au, au_len, 7) && capture_h264_timing(au, &au_len, FRAME_CAP, req->fps)) {
            why("The encoder gave unsupported picture timing."); goto done;
        }
        {   // when each frame came out of the encoder, against the frame period
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            const int64_t us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
            if (last_read_us) gaps[cap_gap_bucket(us - last_read_us, 1000000 / (long long)req->fps)]++;
            last_read_us = us;
        }
        if (net_send_inplace(frame, CAP_VIDEO, idr ? CAP_KEY : 0, (uint32_t)au_len)) break;
        const uint64_t got = (uint64_t)now_ms();
        if (segment.frames && (int64_t)(got - last_picture) > window_gap) window_gap = (int64_t)(got - last_picture);
        if (au_len > window_max) window_max = au_len;
        if (idr && au_len > window_idr) window_idr = au_len;
        last_picture = got;
        segment.frames++;
        segment.last_video_ms = got - segment.begin_ms;
        if (segment.frames <= 3 || segment.frames % (req->fps * 10) == 0) {
            // Viewer-side hiccups (2026-11-09): the largest gap and frames of the last 10 s.
            log_line("frames=%u last=%zu bytes; 10 s: max gap %lld ms, largest %zu, largest IDR %zu; "
                     "gaps vs period <.5 %u <.9 %u ~1 %u <1.5 %u <2.5 %u more %u", segment.frames,
                     (size_t)n, (long long)window_gap, window_max, window_idr, gaps[0], gaps[1], gaps[2], gaps[3],
                     gaps[4], gaps[5]);
            window_gap = 0; window_max = window_idr = 0;
            memset(gaps, 0, sizeof gaps);
        }
    }
    end = SESS_STOPPED;
done:
    free(frame);
    cleanup(&c);
    segment.final_mode = c.final_mode;
    segment.cleanup_ok = !c.cleanup_failed && (!c.ctx.quarantined || c.ctx.aborted_restored);
    if (!segment.cleanup_ok)
        snprintf(segment.why, sizeof segment.why, "Capture cleanup failed. The console may need a restart.");
    segment.transport_failed = c.ctx.quarantined && !c.ctx.aborted_restored;
    // A stop (the core's, or its disappearance, or ours) is the normal end whatever stopped
    // the setup; only a failed cleanup makes it a failure.
    if (end == SESS_STOPPED || quit || net_dead || stop_requested)
        return segment.cleanup_ok ? SESS_STOPPED : SESS_FAILED;
    if (segment.api_error == 0x81950009u && may_recover && segment.cleanup_ok && !segment.transport_failed &&
        segment.stable_mode && (segment.stable_mode != 0x40 || ALLOW_MODE_40) && segment.frames)
        return SESS_RECOVERABLE;
    if (segment.stable_mode == 0x40 && !ALLOW_MODE_40)
        snprintf(segment.why, sizeof segment.why, "Go Live can't capture while the console outputs 120 Hz.");
    return SESS_FAILED;
}

// The worker: connect, wait for the core's start, run the session, recover at most once.
static int helper_run(void)
{
    if (net_connect()) return 1;
    struct start_req req = {0};
    int got = 0;
    for (uint64_t begin = now_ms(); !got && !net_dead && !quit && !stop_requested && now_ms() - begin < START_WAIT_MS;) {
        net_poll(&req, &got, 100);
        beat();
    }
    if (!got || stop_requested) { net_close(); return stop_requested ? 0 : 1; }
    const int sel_ok = (req.selector == 3 && req.width == 1280 && req.height == 720) ||
                       (req.selector == 4 && req.width == 1920 && req.height == 1080);
    if (!sel_ok || req.fps_flag > 1 || req.fps != (req.fps_flag ? 60u : 30u) || req.bitrate < 500000 || req.bitrate > 30000000) {
        log_line("refusing an unsupported profile");
        net_state(CAP_ERROR, "Unsupported capture profile.");
        net_close();
        return 1;
    }
    log_line("start: %ux%u@%u bitrate=%u", req.width, req.height, req.fps, req.bitrate);
    if (ALLOW_MODE_40)
        log_line("test build: the 120 Hz output mode (0x40) is allowed");
    net_state(CAP_STARTING, "");
    // Console 2026-11-09: launching a game changes the HDMI output twice about 3 s apart; with
    // one recovery per Go Live the second change ended the stream. Each reopened session must
    // deliver frames before it may recover again (capture_session), and at most RECOVERY_MAX
    // reopens fall within any RECOVERY_WINDOW_MS, so a broken encoder still ends with an error.
    int rc = 1;
    int64_t reopened[RECOVERY_MAX] = {0}; // when each recent recovery started, oldest replaced first
    unsigned next_slot = 0;
    for (;;) {
        int recent = 0;
        for (unsigned i = 0; i < RECOVERY_MAX; i++)
            recent += reopened[i] && now_ms() - reopened[i] < RECOVERY_WINDOW_MS;
        enum session_end e = capture_session(&req, recent < RECOVERY_MAX);
        if (e == SESS_STOPPED) { rc = 0; break; }
        if (e == SESS_RECOVERABLE && !net_dead && !quit && !stop_requested) {
            reopened[next_slot] = now_ms(); next_slot = (next_slot + 1) % RECOVERY_MAX;
            log_line("recovery %d of %d in a minute: fresh session (mode 0x%x)", recent + 1, RECOVERY_MAX,
                     segment.stable_mode);
            continue;
        }
        net_state(CAP_ERROR, segment.why[0] ? segment.why : "Capture failed.");
        break;
    }
    net_close();
    log_line("helper done rc=%d", rc);
    return rc;
}

// ---- process: lock, supervisor, worker ---------------------------------------------------


// Idle time, not total time: Go Live has no duration. After 15 s without progress the worker
// is asked to clean up; after 25 s only it is killed.
#ifndef WATCHDOG_SOFT_MS
#define WATCHDOG_SOFT_MS 15000u
#define WATCHDOG_HARD_MS 25000u
#define TERM_GRACE_MS 10000u
#define RECOVERY_POLL_US 1000000u
#endif
static int watchdog_state(uint64_t idle)
{
    return idle >= WATCHDOG_HARD_MS ? 2 : idle >= WATCHDOG_SOFT_MS ? 1 : 0;
}

static sl_broker supervisor;
struct supervision { pid_t child; int lock_fd; };
static void *supervise(void *arg)
{
    const struct supervision *s = arg;
    pid_t child = s->child;
    int lock_fd = s->lock_fd;
    int status = 0, term_sent = 0, worker_failed = 0;
    uint64_t term_deadline = 0;
    for (;;) {
        pid_t rc = waitpid(child, &status, WNOHANG);
        if (rc == child) break;
        if (rc < 0 && errno != EINTR) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            worker_failed = 1; break;
        }
        int watchdog = watchdog_state(now_ms() - atomic_load(&progress->heartbeat_ms));
        if (!term_sent && (quit || watchdog >= 1)) {
            log_line("watchdog requesting graceful cleanup");
            kill(child, SIGTERM); term_sent = 1; term_deadline = now_ms() + TERM_GRACE_MS;
        }
        if (watchdog == 2 || (term_sent && now_ms() >= term_deadline)) {
            int id = progress->function;
            log_line("watchdog killing ONLY the owned worker; target=%d stage=%s; reboot may be needed",
                     progress->target, id > 0 && id <= FUNCTION_COUNT ? symbols[id - 1] : "local work");
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        int broker_rc = sl_broker_poll(&supervisor, 25);
        if (broker_rc < 0) {
            // The worker may have died mid-request, or violated framing. Reap it before
            // recovering the private context; never run worker cleanup concurrently.
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            worker_failed = broker_rc == -2; break;
        }
    }
    log_line("worker status=0x%x", status);
    if (supervisor.context.attached) {
        worker_failed = 1;
        log_line("supervisor recovering owned target after worker exit; encoder cleanup unconfirmed, reboot may be needed");
    }
    uint64_t last_hold_log = 0;
    for (;;) {
        int recovered = sl_broker_recover(&supervisor);
        if (recovered == 0 || recovered == SL_END_GONE) break;
        worker_failed = 1;
        if (!last_hold_log || now_ms() - last_hold_log >= 30000) {
            log_line("restoration incomplete at %s; supervisor retaining tracer and retrying; reboot may be needed",
                     supervisor.context.stage ? supervisor.context.stage : "unknown");
            last_hold_log = now_ms();
        }
        usleep(RECOVERY_POLL_US);
    }
    while (sl_broker_close(&supervisor)) {
        worker_failed = 1;
        log_line("supervisor close refused; retaining recovery ownership");
        (void)sl_broker_recover(&supervisor);
        usleep(RECOVERY_POLL_US);
    }
    munmap(progress, sizeof *progress); progress = NULL;
    close(lock_fd);
    if (log_file) fclose(log_file);
    return (void *)(intptr_t)(!worker_failed && WIFEXITED(status) ? WEXITSTATUS(status) : 1);
}

#ifndef CAPTURE_NO_MAIN
static void on_signal(int sig) { (void)sig; quit = 1; }
static void *worker(void *unused) { (void)unused; return (void *)(intptr_t)helper_run(); }
int main(void)
{
#ifdef SL_PS5
    sceKernelSetProcessName(CAP_PROCESS_NAME);
#endif
    if (mkdir(CAP_DIR, 0700) && errno != EEXIST) return 1;
    int lock_fd = open(LOCK_PATH, O_CREAT | O_RDWR, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB)) return 1; // one helper at a time
    (void)rename(LOG_PATH, LOG_PATH ".prev"); // the session before, kept for diagnosis
    log_file = fopen(LOG_PATH, "w");
    progress = mmap(NULL, sizeof *progress, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (progress == MAP_FAILED) return 1;
    memset(progress, 0, sizeof *progress);
    signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGPIPE, SIG_IGN);
    atomic_init(&progress->function, 0);
    atomic_init(&progress->target, 0);
    atomic_init(&progress->heartbeat_ms, now_ms());
    if (sl_broker_open(&supervisor)) return 1;
    pid_t child = fork();
    if (child < 0) return 1;
    if (!child) {
#ifdef SL_PS5
        sceKernelSetProcessName(CAP_WORKER_NAME); // the parent stays the supervisor
#endif
        sl_broker_worker(&supervisor);
        pthread_attr_t attr;
        pthread_t thread;
        if (pthread_attr_init(&attr)) _exit(1);
        int rc = pthread_attr_setstacksize(&attr, 2u * 1024u * 1024u);
        if (!rc) rc = pthread_create(&thread, &attr, worker, NULL);
        pthread_attr_destroy(&attr);
        if (rc) _exit(1);
        void *result = NULL;
        if (pthread_join(thread, &result)) _exit(1);
        _exit((int)(intptr_t)result);
    }
    sl_broker_parent(&supervisor);
    // Native injector/kernel helpers run on the same generous stack as the capture worker.
    // Threads share tracing ownership; main never exits before this supervisor thread joins.
    struct supervision s = {child, lock_fd};
    pthread_attr_t attr;
    pthread_t thread;
    if (pthread_attr_init(&attr)) {
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        sl_broker_close(&supervisor); close(lock_fd); return 1;
    }
    int rc = pthread_attr_setstacksize(&attr, 2u * 1024u * 1024u);
    if (!rc) rc = pthread_create(&thread, &attr, supervise, &s);
    pthread_attr_destroy(&attr);
    if (rc) {
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        sl_broker_close(&supervisor); close(lock_fd); return 1;
    }
    void *result = NULL;
    // A valid, joinable supervisor thread cannot be discarded while it may own the tracee.
    if (pthread_join(thread, &result)) for (;;) pause();
    return (int)(intptr_t)result;
}
#endif
