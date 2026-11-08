// SPDX-License-Identifier: AGPL-3.0-or-later
// The wireless Go Live backend (src/broadcast.c + src/capture_bc.c + src/golive_opus.c) against a
// fake capture helper that replays a real console recording (tests/data/capture-720p30.*: the
// first twelve access units of a 720p30 game capture and half a second of its sound) over the
// real loopback protocol, and a fake game-audio source. The rest of the core is stubbed here.
#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <opus.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../src/broadcast.h"
#include "../src/capture_bc.h"
#include "../src/capture_proto.h"

// ---- stubs for the rest of the core -----------------------------------------------------------

static _Atomic int call_on = 1, usb_on, rp_paired = -1;
static int usb_probe(void) { return usb_on; }
static int pref_quality, pref_backend, pref_bitrate;
static _Atomic unsigned stops_of_stream, alerts;
static char alert_text[160];

void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_printf(const char *fmt, ...)
{
    if (!getenv("VERBOSE"))
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
int voice_wanted(void) { return call_on; }
int prefs_get(const char *key)
{
    return !strcmp(key, "golive_quality")   ? pref_quality
           : !strcmp(key, "golive_backend") ? pref_backend
           : !strcmp(key, "golive_bitrate") ? pref_bitrate
                                            : 0;
}
int32_t rp_used(void) { return rp_paired; }
void stream_stop(void) { stops_of_stream++; }
void app_alert(const char *context, const char *text, const char *icon, int urgent)
{
    (void)icon, (void)urgent;
    snprintf(alert_text, sizeof alert_text, "%s: %s", context, text);
    alerts++;
}
int tls_random(void *unused, unsigned char *out, size_t len)
{
    (void)unused;
    FILE *f = fopen("/dev/urandom", "rb");
    assert(f && fread(out, 1, len, f) == len);
    fclose(f);
    return 0;
}

// The shared game sound: the recording's PCM, handed over in 1024-frame records once released.
static int16_t *pcm16;
static size_t pcm_frames, pcm_pos;
static _Atomic int ga_go, ga_acquired, ga_released;
int ga_acquire(void) { ga_acquired++; return 3; }
void ga_release(int id) { assert(id == 3); ga_released++; }
int ga_live(void) { return 1; }
static int ga_disc_at = -1; // flag a discontinuity on this 1024-frame record
static size_t ga_record;
static unsigned ga_stale_seen;
int ga_read2(int id, float *out, unsigned max, unsigned stale_ms, int *disc)
{
    assert(id == 3);
    ga_stale_seen = stale_ms;
    *disc = 0;
    if (!ga_go || pcm_pos >= pcm_frames)
        return 0;
    size_t n = pcm_frames - pcm_pos < 1024 ? pcm_frames - pcm_pos : 1024;
    assert(n * 8 <= max);
    for (size_t i = 0; i < n * 2; i++)
        out[i] = pcm16[pcm_pos * 2 + i] / 32768.0f;
    pcm_pos += n;
    *disc = (int)ga_record++ == ga_disc_at;
    return (int)(n * 8);
}
int ga_read(int id, float *out, unsigned max)
{
    int d;
    return ga_read2(id, out, max, 0, &d);
}

// ---- the sample and the sink -----------------------------------------------------------------------

static uint8_t *sample;
static size_t sizes[16], n_sizes, header_len; // sizes[0] is the header's, then each access unit
static size_t offsets[16];
static uint8_t is_key[16];

static pthread_mutex_t seen_mu = PTHREAD_MUTEX_INITIALIZER;
struct video_seen {
    size_t len;
    int key;
    uint8_t first[8];
    uint32_t checksum;
};
static struct video_seen videos[64];
static unsigned n_videos;
static uint8_t opus_frames[200][1300];
static size_t opus_len[200];
static unsigned n_opus;

static uint32_t checksum(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static void sink_video(const uint8_t *au, size_t n, int key)
{
    pthread_mutex_lock(&seen_mu);
    assert(n_videos < 64);
    videos[n_videos++] = (struct video_seen){n, key, {0}, checksum(au, n)};
    memcpy(videos[n_videos - 1].first, au, n < 8 ? n : 8);
    pthread_mutex_unlock(&seen_mu);
}
static void sink_audio(const uint8_t *opus, size_t n)
{
    pthread_mutex_lock(&seen_mu);
    assert(n_opus < 200 && n <= 1300);
    memcpy(opus_frames[n_opus], opus, n);
    opus_len[n_opus++] = n;
    pthread_mutex_unlock(&seen_mu);
}
static const bc_sink sink = {.video = sink_video, .audio = sink_audio};

static unsigned count_videos(void)
{
    pthread_mutex_lock(&seen_mu);
    unsigned n = n_videos;
    pthread_mutex_unlock(&seen_mu);
    return n;
}
static unsigned count_opus(void)
{
    pthread_mutex_lock(&seen_mu);
    unsigned n = n_opus;
    pthread_mutex_unlock(&seen_mu);
    return n;
}

static int has_idr(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1 && (p[i + 3] & 31) == 5)
            return 1;
    return 0;
}

static void load_sample(void)
{
    FILE *f = fopen("tests/data/capture-720p30.sizes", "r");
    assert(f);
    while (n_sizes < 16 && fscanf(f, "%zu", &sizes[n_sizes]) == 1)
        n_sizes++;
    fclose(f);
    header_len = sizes[0];
    f = fopen("tests/data/capture-720p30.h264", "rb");
    assert(f);
    size_t total = 0;
    for (size_t i = 0; i < n_sizes; i++)
        total += sizes[i];
    sample = malloc(total);
    assert(fread(sample, 1, total, f) == total);
    fclose(f);
    size_t at = 0;
    for (size_t i = 0; i < n_sizes; i++) {
        offsets[i] = at;
        at += sizes[i];
        is_key[i] = i > 0 && has_idr(sample + offsets[i], sizes[i]);
    }
    assert(is_key[1]); // the recording starts with its IDR
    f = fopen("tests/data/capture-720p30.pcm", "rb");
    assert(f);
    pcm16 = malloc(96000);
    assert(fread(pcm16, 1, 96000, f) == 96000);
    fclose(f);
    pcm_frames = 96000 / 4;
}

// ---- the fake helper -------------------------------------------------------------------------------

enum mode { H_NORMAL, H_BAD_TOKEN_FIRST, H_RECOVER, H_DIES, H_ERROR, H_GARBAGE, H_NONE, H_WRONG_PROFILE, H_DEAF, H_FRAGMENTED, H_COALESCED, H_FLOOD, H_SLOW_HELLO,
            H_SILENT, H_PARTIAL, H_NO_IDR, H_NO_PROGRESS, H_RECOVER_HANG,
            H_EOF_HELLO, H_EOF_PROFILE, H_EOF_HEADERS, H_SHORT_COMMANDS, H_COMMAND_STALL, H_STOP_ERROR, H_RECOVER_ERROR, H_BUSY_FIRST, H_LAUNCH_DELAY };
static enum mode mode;
static _Atomic int key_partly_sent, finish_key;
static unsigned command_writes;

// Linked only by the backend test object; this is the production session writer's send().
ssize_t capture_test_send(int fd, const void *buf, size_t n, int flags)
{
    if (mode == H_COMMAND_STALL) { errno = EAGAIN; return -1; }
    if (mode == H_SHORT_COMMANDS) {
        unsigned attempt = command_writes++;
        if (attempt == 0 || attempt == 2) { errno = attempt == 0 ? EINTR : EAGAIN; return -1; }
        if (key_partly_sent && !finish_key) { errno = EAGAIN; return -1; }
        if (n == CAP_HEADER_BYTES && ((const uint8_t *)buf)[0] == CAP_KEYFRAME) key_partly_sent = 1;
        if (n > 3) n = 3;
    }
    return send(fd, buf, n, flags);
}
static char conn_path[200];
static _Atomic unsigned launches;
static struct {
    _Atomic int rejected, started, saw_eof, conn_mode_ok;
    uint32_t start[6];
    _Atomic unsigned keyframes, keyframes_after_live2, stops;
    uint32_t port;
    char hex[40];
} hs;

static int send_all(int fd, const void *p, size_t n)
{
    const uint8_t *bytes = p;
    while (n) {
        size_t take = mode == H_FRAGMENTED && n > 3 ? 3 : n;
        ssize_t w = send(fd, bytes, take, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        bytes += w; n -= (size_t)w;
    }
    return 0;
}
static int send_msg(int fd, uint8_t type, uint8_t flags, const void *body, uint32_t len)
{
    uint8_t h[CAP_HEADER_BYTES];
    cap_pack_header(h, type, flags, len);
    return send_all(fd, h, sizeof h) || (len && send_all(fd, body, len)) ? -1 : 0;
}
static void send_state(int fd, int state, const char *text)
{
    uint8_t m[100];
    size_t t = text ? strlen(text) : 0;
    m[0] = (uint8_t)state;
    memcpy(m + 1, text ? text : "", t);
    send_msg(fd, CAP_STATE, 0, m, (uint32_t)(1 + t));
}
static void send_profile(int fd, uint32_t w, uint32_t h, uint32_t fps)
{
    uint8_t p[16];
    cap_put32(p, w), cap_put32(p + 4, h), cap_put32(p + 8, fps), cap_put32(p + 12, 0x30);
    send_msg(fd, CAP_PROFILE, 0, p, 16);
}
static void send_au(int fd, unsigned i)
{
    send_msg(fd, CAP_VIDEO, is_key[i] ? CAP_KEY : 0, sample + offsets[i], (uint32_t)sizes[i]);
}

static int connect_core(const uint8_t *token)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)hs.port)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    uint8_t hello[4 + CAP_TOKEN_BYTES];
    cap_put32(hello, CAP_VERSION);
    memcpy(hello + 4, token, CAP_TOKEN_BYTES);
    send_msg(fd, CAP_HELLO, 0, hello, sizeof hello);
    return fd;
}

// Non-blocking look at what the core sent: returns a message type or 0; EOF sets *eof.
static int poll_msg(int fd, int wait_ms, int *eof, uint8_t *body, size_t *len)
{
    struct pollfd pf = {.fd = fd, .events = POLLIN};
    if (poll(&pf, 1, wait_ms) <= 0)
        return 0;
    uint8_t h[CAP_HEADER_BYTES];
    ssize_t n = recv(fd, h, sizeof h, MSG_WAITALL);
    if (n != (ssize_t)sizeof h) {
        *eof = 1;
        return 0;
    }
    cap_header hh;
    assert(!cap_parse_header(h, &hh) && hh.len <= 64);
    if (hh.len)
        assert(recv(fd, body, hh.len, MSG_WAITALL) == (ssize_t)hh.len);
    *len = hh.len;
    return hh.type;
}

static void *helper_body(void *arg)
{
    (void)arg;
    char text[100] = {0};
    FILE *f = fopen(conn_path, "r");
    assert(f);
    assert(fread(text, 1, sizeof text - 1, f) > 0);
    fclose(f);
    unsigned port;
    char hex[40];
    assert(sscanf(text, "%u %32s", &port, hex) == 2 && strlen(hex) == 32);
    hs.port = port;
    strcpy(hs.hex, hex);
    uint8_t token[CAP_TOKEN_BYTES];
    for (int i = 0; i < CAP_TOKEN_BYTES; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        token[i] = (uint8_t)v;
    }
    if (mode == H_BAD_TOKEN_FIRST || mode == H_FLOOD) {
      for (unsigned attempt = 0; attempt < (mode == H_FLOOD ? 40u : 1u); attempt++) { // a stranger on loopback: refused, closed on, nothing served
        uint8_t wrong[CAP_TOKEN_BYTES];
        memcpy(wrong, token, sizeof wrong);
        wrong[CAP_TOKEN_BYTES - 1] ^= 1; // only the last byte differs
        int bad = connect_core(wrong);
        assert(bad >= 0);
        struct pollfd pf = {.fd = bad, .events = POLLIN};
        assert(poll(&pf, 1, 3000) > 0);
        uint8_t b;
        hs.rejected = recv(bad, &b, 1, 0) <= 0;
        close(bad);
      }
    }
    int slow[12];
    if (mode == H_SLOW_HELLO) for (unsigned i = 0; i < 12; i++) {
        slow[i] = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)hs.port)};
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(!connect(slow[i], (struct sockaddr *)&a, sizeof a));
        assert(send(slow[i], "C", 1, MSG_NOSIGNAL) == 1);
    }
    int fd = connect_core(token);
    if (mode == H_SLOW_HELLO) for (unsigned i = 0; i < 12; i++) close(slow[i]);
    assert(fd >= 0);
    if (mode == H_EOF_HELLO) { close(fd); return NULL; }
    uint8_t body[64];
    size_t len = 0;
    int eof = 0, type;
    while (!eof && (type = poll_msg(fd, 3000, &eof, body, &len)) != CAP_START)
        assert(type == 0);
    if (eof && mode == H_COMMAND_STALL) { hs.saw_eof = 1; close(fd); return NULL; }
    assert(!eof && len == 24);
    for (int i = 0; i < 6; i++)
        hs.start[i] = cap_get32(body + 4 * i);
    hs.started = 1;
    if (mode == H_SILENT || mode == H_PARTIAL) {
        if (mode == H_PARTIAL) {
            uint8_t partial[CAP_HEADER_BYTES + 3];
            cap_pack_header(partial, CAP_PROFILE, 0, 16);
            memset(partial + CAP_HEADER_BYTES, 0, 3);
            assert(!send_all(fd, partial, sizeof partial));
        }
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        assert(poll(&pf, 1, 3000) > 0);
        uint8_t b;
        hs.saw_eof = recv(fd, &b, 1, 0) <= 0;
        close(fd); return NULL;
    }
    const int wrong = mode == H_WRONG_PROFILE;
    if (mode == H_COALESCED) {
        size_t total = 3 * CAP_HEADER_BYTES + 16 + header_len + 1;
        uint8_t *messages = calloc(1, total), *m = messages;
        assert(messages);
        cap_pack_header(m, CAP_PROFILE, 0, 16); m += CAP_HEADER_BYTES;
        cap_put32(m, hs.start[3]); cap_put32(m + 4, hs.start[4]); cap_put32(m + 8, hs.start[5]); m += 16;
        cap_pack_header(m, CAP_HEADERS, 0, (uint32_t)header_len); m += CAP_HEADER_BYTES;
        memcpy(m, sample, header_len); m += header_len;
        cap_pack_header(m, CAP_STATE, 0, 1); m[CAP_HEADER_BYTES] = CAP_LIVE;
        assert(!send_all(fd, messages, total)); free(messages);
    } else {
        send_state(fd, CAP_STARTING, "");
        send_profile(fd, wrong ? 1920 : hs.start[3], wrong ? 1080 : hs.start[4], wrong ? 60 : hs.start[5]);
        if (mode == H_EOF_PROFILE) { close(fd); return NULL; }
        send_msg(fd, CAP_HEADERS, 0, sample, (uint32_t)header_len);
        if (mode == H_EOF_HEADERS) { close(fd); return NULL; }
        send_state(fd, CAP_LIVE, "");
    }
    unsigned next = 1, resend = 0, lives = 1;
    int quiet_after = 0;
    for (;;) {
        // each core message first: a keyframe request is answered with the IDR again
        for (int k = 0; k < 4 && !eof; k++) {
            type = poll_msg(fd, 0, &eof, body, &len);
            if (!type)
                break;
            if (type == CAP_KEYFRAME) {
                hs.keyframes++;
                if (lives >= 2)
                    hs.keyframes_after_live2++;
                resend++;
            } else if (type == CAP_STOP) {
                hs.stops++;
                if (mode == H_DEAF)
                    continue; // never answers: the core must give up on it
                if (mode == H_STOP_ERROR) send_state(fd, CAP_ERROR, "Capture cleanup failed. The console may need a restart.");
                usleep(200000); // cleanup takes a moment
                close(fd);
                return NULL;
            }
        }
        if (eof) {
            hs.saw_eof = 1;
            close(fd);
            return NULL;
        }
        if (mode == H_NO_IDR || (mode == H_NO_PROGRESS && next >= 4)) {
            usleep(1000); continue;
        }
        if (mode == H_RECOVER_ERROR && next >= 4) {
            send_state(fd, CAP_RECOVERING, "");
            send_state(fd, CAP_ERROR, "The encoder could not reopen.");
            usleep(20000); close(fd); return NULL;
        }
        if (mode == H_RECOVER_HANG && next >= 4) {
            send_state(fd, CAP_RECOVERING, "");
            usleep(20000); continue; // repeated state cannot extend the deadline
        }
        if (mode == H_DIES && next == 4) {
            close(fd);
            return NULL;
        }
        if (mode == H_ERROR && next == 4) {
            send_state(fd, CAP_ERROR, "The picture stopped.");
            usleep(50000);
            close(fd);
            return NULL;
        }
        if (mode == H_GARBAGE && next == 4) {
            send_msg(fd, 99, 0, NULL, 0);
            struct pollfd pf = {.fd = fd, .events = POLLIN};
            poll(&pf, 1, 3000);
            uint8_t b;
            hs.saw_eof = recv(fd, &b, 1, 0) <= 0;
            close(fd);
            return NULL;
        }
        if (mode == H_RECOVER && next == 6 && lives == 1) { // game switch: a gap, then a fresh session
            send_state(fd, CAP_RECOVERING, "");
            usleep(150000);
            send_profile(fd, hs.start[3], hs.start[4], hs.start[5]);
            send_msg(fd, CAP_HEADERS, 0, sample, (uint32_t)header_len);
            send_state(fd, CAP_LIVE, "");
            lives = 2;
            send_au(fd, 1); // the first IDR again
            continue;
        }
        if (resend) {
            resend--;
            send_au(fd, 1);
        } else if (next < n_sizes) {
            send_au(fd, next++);
        } else {
            quiet_after = 1;
        }
        usleep(quiet_after ? 5000 : 2000);
    }
}

static _Atomic int helpers_alive;
static void *helper_thread(void *arg)
{
    void *r = helper_body(arg);
    helpers_alive--;
    return r;
}

static int launch_fails(void)
{
    launches++;
    return -2;
}

static int launch_io_fails(void) { launches++; return -1; }

static int launch(void)
{
    launches++;
    if (mode == H_NONE || (mode == H_BUSY_FIRST && launches == 1))
        return 0;
    struct stat st;
    assert(stat(conn_path, &st) == 0);
    hs.conn_mode_ok = (st.st_mode & 0777) == 0600;
    pthread_t t;
    helpers_alive++;
    assert(pthread_create(&t, NULL, helper_thread, NULL) == 0);
    pthread_detach(t);
    if (mode == H_LAUNCH_DELAY) usleep(700000);
    return 0;
}

// ---- the test ---------------------------------------------------------------------------------------

#define WAIT(cond, ms)                                                   \
    do {                                                                 \
        int w_ = 0;                                                      \
        while (!(cond) && w_++ < (ms) / 5)                               \
            usleep(5000);                                                \
        if (!(cond)) {                                                   \
            fprintf(stderr, "timeout waiting for: %s (state %s, error '%s')\n", #cond, bc_state(), bc_error()); \
            assert(cond);                                                \
        }                                                                \
    } while (0)

static void reset(enum mode m)
{
    for (int i = 0; helpers_alive > 0 && i < 1000; i++) // the last scenario's helper has left
        usleep(5000);
    assert(helpers_alive == 0);
    mode = m;
    memset(&hs, 0, sizeof hs);
    launches = 0;
    key_partly_sent = finish_key = 0; command_writes = 0;
    pthread_mutex_lock(&seen_mu);
    n_videos = n_opus = 0;
    pthread_mutex_unlock(&seen_mu);
    pcm_pos = 0;
    ga_record = 0;
    ga_go = 0;
    ga_acquired = ga_released = 0;
    stops_of_stream = alerts = 0;
    alert_text[0] = 0;
    call_on = 1;
}

static int port_open(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)hs.port)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int r = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return r;
}

static void check_stopped_clean(void)
{
    WAIT(!strcmp(bc_state(), "off") || !strcmp(bc_state(), "error"), 6000);
    WAIT(ga_released == ga_acquired, 1000);
    assert(access(conn_path, F_OK) != 0); // the token file is gone
    assert(hs.port == 0 || !port_open()); // and nothing listens
    assert(bc_holds_controller() == 0 && !strcmp(bc_controller(), "ok"));
}

static void expect_start(unsigned selector, unsigned flag, unsigned bitrate, unsigned w, unsigned h, unsigned fps)
{
    assert(hs.start[0] == selector && hs.start[1] == flag && hs.start[2] == bitrate);
    assert(hs.start[3] == w && hs.start[4] == h && hs.start[5] == fps);
}

// One normal Go Live of a profile: the picture and sound as recorded, the keyframe round trip.
static void normal_run(int quality, unsigned selector, unsigned flag, unsigned bitrate, unsigned w, unsigned h, unsigned fps)
{
    reset(H_NORMAL);
    pref_quality = quality;
    assert(bc_start() == 0);
    assert(!strcmp(bc_state(), "starting") || !strcmp(bc_state(), "live"));
    WAIT(!strcmp(bc_state(), "live"), 5000);
    assert(launches == 1 && hs.conn_mode_ok && hs.started);
    expect_start(selector, flag, bitrate, w, h, fps);
    int pw, ph, pfps;
    bc_profile(&pw, &ph, &pfps);
    assert((unsigned)pw == w && (unsigned)ph == h && (unsigned)pfps == fps);
    ga_go = 1;
    WAIT(count_videos() >= 13 && count_opus() >= (ga_disc_at < 0 ? 50u : 49u), 5000);
    usleep(150000);
    pthread_mutex_lock(&seen_mu);
    // The headers first as an access unit of their own (stream.c keeps them for the IDRs), then
    // every access unit as recorded: boundaries, sizes, bytes and keyframe flags.
    assert(n_videos == 13);
    assert(videos[0].len == header_len && !videos[0].key && videos[0].checksum == checksum(sample, header_len));
    for (unsigned i = 1; i < n_sizes; i++) {
        assert(videos[i].len == sizes[i]);
        assert(videos[i].key == is_key[i]);
        assert(videos[i].checksum == checksum(sample + offsets[i], sizes[i]));
        assert(!memcmp(videos[i].first, "\0\0\0\1", 4));
    }
    // Sound: half a second is exactly 50 frames of 10 ms, each a stereo Opus frame.
    // 24000 frames are exactly 50 frames of 10 ms; a flagged cut drops the half frame waiting
    // (10 records of 1024 = 10240 frames: 21 whole frames, 160 left over, thrown away) so the rest
    // starts a clean frame: 13760 frames more = 28 whole, never 50 spliced across the cut.
    assert(n_opus == (ga_disc_at < 0 ? 50u : 49u));
    assert(ga_stale_seen == 100);
    int err;
    OpusDecoder *dec = opus_decoder_create(48000, 2, &err);
    double energy = 0;
    for (unsigned i = 0; i < n_opus; i++) {
        assert(opus_packet_get_nb_channels(opus_frames[i]) == 2);
        assert(opus_packet_get_samples_per_frame(opus_frames[i], 48000) == 480);
        int16_t out[960];
        assert(opus_decode(dec, opus_frames[i], (opus_int32)opus_len[i], out, 480, 0) == 480);
        for (int s = 0; s < 960; s++)
            energy += (double)out[s] * out[s];
    }
    opus_decoder_destroy(dec);
    assert(energy > 1e9); // not silence
    pthread_mutex_unlock(&seen_mu);

    // A viewer lost the picture: the helper is asked, answers with an IDR; a burst is one request.
    const unsigned before = count_videos();
    bc_request_keyframe();
    bc_request_keyframe();
    WAIT(count_videos() > before, 2000);
    usleep(300000);
    assert(hs.keyframes == 1 && count_videos() == before + 1);
    pthread_mutex_lock(&seen_mu);
    assert(videos[before].key && videos[before].checksum == checksum(sample + offsets[1], sizes[1]));
    pthread_mutex_unlock(&seen_mu);
    usleep(250000);
    bc_request_keyframe(); // after the spacing: a new request goes through
    WAIT(hs.keyframes == 2, 2000);

    bc_stop();
    check_stopped_clean();
    assert(!strcmp(bc_state(), "off") && !bc_error()[0] && hs.stops == 1);
    assert(stops_of_stream == 0 && alerts == 0);
    bc_profile(&pw, &ph, &pfps); // the requested one again
    assert(pw == (h >= 1080 ? 1920 : 1280));
}

struct racer {
    int result;
};
static void *racer_thread(void *arg)
{
    ((struct racer *)arg)->result = bc_start();
    return NULL;
}

int main(void)
{
    char dir[] = "/tmp/capture-backend-XXXXXX";
    load_sample();
    assert(mkdtemp(dir));
    snprintf(conn_path, sizeof conn_path, "%s/capture.conn", dir);
    capture_set_conn_file(conn_path);
    capture_set_launcher(launch);
    capture_set_timeouts(3000, 2500);
    bc_set_sink(&sink);
    int cases = 0;

    // Every profile the prefs offer reaches the encoder as selector/flag/size/fps/bitrate (the
    // recommended 4, 6, 6 and 8 Mbit/s), and each run delivers the recording exactly. Only 720p30
    // and 1080p60 are console-tested; 720p60 and 1080p30 pass through.
    normal_run(0, 3, 0, 4000000, 1280, 720, 30), cases++;
    normal_run(1, 3, 1, 6000000, 1280, 720, 60), cases++;
    normal_run(2, 4, 0, 6000000, 1920, 1080, 30), cases++;
    normal_run(3, 4, 1, 8000000, 1920, 1080, 60), cases++;
    // A custom bitrate (prefs level n: n + 1 Mbit/s), held to what the stream announces to
    // Discord: 10 Mbit/s at 720p, 20 at 1080p.
    pref_bitrate = 2;
    normal_run(0, 3, 0, 3000000, 1280, 720, 30), cases++;
    pref_bitrate = 19;
    normal_run(0, 3, 0, 10000000, 1280, 720, 30), cases++;
    normal_run(3, 4, 1, 20000000, 1920, 1080, 60), cases++;
    pref_bitrate = 0;
    pref_quality = 0;

    // The owner reports a cut in the sound (records dropped, a stale backlog, a reopened capture):
    // the Opus framing starts clean there.
    ga_disc_at = 10;
    normal_run(0, 3, 0, 4000000, 1280, 720, 30), cases++;
    ga_disc_at = -1;

    // Repeated and concurrent starts are one Go Live: one helper launch, one connection.
    reset(H_NORMAL);
    {
        pthread_t t[4];
        struct racer r[4] = {{-9}, {-9}, {-9}, {-9}};
        for (int i = 0; i < 4; i++)
            pthread_create(&t[i], NULL, racer_thread, &r[i]);
        for (int i = 0; i < 4; i++) {
            pthread_join(t[i], NULL);
            assert(r[i].result == 0);
        }
        assert(bc_start() == 0 && bc_start() == 0);
        WAIT(!strcmp(bc_state(), "live"), 5000);
        usleep(300000);
        assert(launches == 1);
        bc_stop();
        bc_stop(); // twice is harmless
        check_stopped_clean();
        assert(!strcmp(bc_state(), "off") && hs.stops == 1);
        cases++;
    }

    // A stranger who connects first, with a token one byte off, is refused and closed on; the real
    // helper then goes live.
    reset(H_BAD_TOKEN_FIRST);
    assert(bc_start() == 0);
    WAIT(!strcmp(bc_state(), "live"), 8000);
    assert(hs.rejected && hs.started);
    bc_stop();
    check_stopped_clean();
    cases++;

    // Wire fragmentation/coalescing and more than 32 wrong tokens cannot abort startup.
    const enum mode good_wire[] = {H_FRAGMENTED, H_COALESCED, H_FLOOD, H_SLOW_HELLO};
    for (unsigned i = 0; i < sizeof good_wire / sizeof *good_wire; i++) {
        reset(good_wire[i]);
        assert(!bc_start());
        WAIT(!strcmp(bc_state(), "live"), 5000);
        WAIT(count_videos() >= 13, 5000);
        assert(launches == 1);
        bc_stop(); check_stopped_clean();
        assert(!strcmp(bc_state(), "off") && hs.stops == 1);
        cases++;
    }
    const enum mode failed_wire[] = {H_SILENT, H_PARTIAL, H_NO_IDR, H_NO_PROGRESS, H_RECOVER_HANG,
                                     H_EOF_HELLO, H_EOF_PROFILE, H_EOF_HEADERS};
    capture_set_media_timeouts(500, 500, 500);
    for (unsigned i = 0; i < sizeof failed_wire / sizeof *failed_wire; i++) {
        reset(failed_wire[i]);
        assert(!bc_start());
        WAIT(!strcmp(bc_state(), "error"), 2500);
        check_stopped_clean();
        if (i < 5) assert(strstr(bc_error(), "stopped responding"));
        cases++;
    }
    capture_set_media_timeouts(CAP_FIRST_MEDIA_MS, CAP_MEDIA_MS, CAP_RECOVERY_MS);

    // Short START, then a partially written keyframe while STOP waits. No header is spliced.
    reset(H_SHORT_COMMANDS);
    assert(!bc_start());
    WAIT(!strcmp(bc_state(), "live"), 5000);
    bc_request_keyframe();
    WAIT(key_partly_sent, 1000);
    bc_stop(); finish_key = 1;
    check_stopped_clean();
    assert(!strcmp(bc_state(), "off") && hs.keyframes == 1 && hs.stops == 1);
    cases++;
    // A command that never makes progress ends the session independently of media deadlines.
    reset(H_COMMAND_STALL);
    assert(!bc_start());
    WAIT(!strcmp(bc_state(), "error"), 2500);
    check_stopped_clean();
    assert(strstr(bc_error(), "receive a command"));
    cases++;

    reset(H_STOP_ERROR);
    assert(!bc_start()); WAIT(!strcmp(bc_state(), "live"), 5000);
    bc_stop(); check_stopped_clean();
    assert(!strcmp(bc_state(), "error") && strstr(bc_error(), "cleanup failed")); cases++;

    reset(H_RECOVER_ERROR);
    assert(!bc_start()); WAIT(!strcmp(bc_state(), "error"), 5000);
    assert(!strcmp(bc_error(), "The encoder could not reopen.")); check_stopped_clean(); cases++;

    reset(H_LAUNCH_DELAY);
    assert(!bc_start()); WAIT(!strcmp(bc_state(), "live"), 5000);
    bc_stop(); check_stopped_clean(); cases++;

    // A prior helper still owns the lock: a later scheduled launch succeeds.
    reset(H_BUSY_FIRST); capture_set_timeouts(12000, 2500);
    assert(!bc_start()); WAIT(!strcmp(bc_state(), "live"), 11000);
    assert(launches == 2); bc_stop(); check_stopped_clean(); cases++;
    capture_set_timeouts(3000, 2500);

    // A game or display switch: the helper reports recovering, a gap, then a fresh session with its
    // own SPS/PPS and an IDR. The core stays "live" (the Discord stream holds) through it and asks
    // for a keyframe once the picture is back.
    reset(H_RECOVER);
    assert(bc_start() == 0);
    WAIT(!strcmp(bc_state(), "live"), 5000);
    {
        unsigned samples = 0;
        while (count_videos() < 12 + 1 + 1 + 2 && samples++ < 600) { // headers, 5 AUs, headers again, IDR...
            const char *s = bc_state();
            assert(!strcmp(s, "live")); // never "error" or "starting" in the gap
            usleep(5000);
        }
        WAIT(hs.keyframes_after_live2 >= 1, 3000);
        pthread_mutex_lock(&seen_mu);
        unsigned headers = 0, after_second_headers_key = 0, seen_headers = 0;
        for (unsigned i = 0; i < n_videos; i++) {
            if (videos[i].len == header_len && !videos[i].key) {
                headers++;
                seen_headers++;
            } else if (seen_headers == 2 && videos[i].key) {
                after_second_headers_key++;
            }
        }
        assert(headers == 2 && after_second_headers_key >= 1);
        pthread_mutex_unlock(&seen_mu);
        assert(!strcmp(bc_state(), "live"));
    }
    bc_stop();
    check_stopped_clean();
    assert(!strcmp(bc_state(), "off") && stops_of_stream == 0);
    cases++;

    // The helper dies mid-stream: an error, the Discord stream is taken down, the game is told;
    // and Go Live can be started again afterwards.
    reset(H_DIES);
    assert(bc_start() == 0);
    WAIT(!strcmp(bc_state(), "error"), 6000);
    assert(!strcmp(bc_error(), "Capture stopped."));
    WAIT(stops_of_stream == 1 && alerts == 1, 2000);
    assert(strstr(alert_text, "Go Live stopped"));
    check_stopped_clean();
    normal_run(0, 3, 0, 4000000, 1280, 720, 30);
    cases++;

    // The helper says why before it goes: that text is the error.
    reset(H_ERROR);
    assert(bc_start() == 0);
    WAIT(!strcmp(bc_state(), "error"), 6000);
    assert(!strcmp(bc_error(), "The picture stopped."));
    check_stopped_clean();
    cases++;

    // A message the helper never sends: the connection is closed on it.
    reset(H_GARBAGE);
    assert(bc_start() == 0);
    WAIT(!strcmp(bc_state(), "error"), 6000);
    WAIT(hs.saw_eof, 3000);
    check_stopped_clean();
    cases++;

    // No helper at all (elfldr did not run it): an error after the timeout, nothing left behind.
    reset(H_NONE);
    capture_set_timeouts(700, 2500);
    assert(bc_start() == 0);
    WAIT(!strcmp(bc_state(), "error"), 6000);
    assert(!strcmp(bc_error(), "Capture did not start.") && launches == 1);
    check_stopped_clean();
    capture_set_timeouts(3000, 2500);
    cases++;

    // The helper is not in the app's assets (the launcher fails every time): an error that says so.
    reset(H_NONE);
    capture_set_timeouts(700, 2500);
    capture_set_launcher(launch_fails);
    assert(bc_start() == 0);
    WAIT(!strcmp(bc_state(), "error"), 6000);
    assert(!strcmp(bc_error(), "The capture helper is missing."));
    check_stopped_clean();
    capture_set_launcher(launch);
    capture_set_timeouts(3000, 2500);
    cases++;

    reset(H_NONE); capture_set_timeouts(700, 2500); capture_set_launcher(launch_io_fails);
    assert(!bc_start()); WAIT(!strcmp(bc_state(), "error"), 3000);
    assert(!strcmp(bc_error(), "The capture helper could not be loaded.")); check_stopped_clean(); cases++;
    capture_set_launcher(launch); capture_set_timeouts(3000, 2500);

    // The call ends while live: Go Live ends with it, as asked, no error.
    reset(H_NORMAL);
    assert(bc_start() == 0);
    WAIT(!strcmp(bc_state(), "live"), 5000);
    call_on = 0;
    WAIT(!strcmp(bc_state(), "off"), 6000);
    assert(hs.stops == 1 && !bc_error()[0] && alerts == 0);
    check_stopped_clean();
    cases++;

    // The profile the encoder reports is the one bc_profile gives while live.
    reset(H_WRONG_PROFILE);
    pref_quality = 0;
    assert(bc_start() == 0);
    WAIT(!strcmp(bc_state(), "live"), 5000);
    {
        int w, h, fps;
        WAIT((bc_profile(&w, &h, &fps), w == 1920), 2000);
        assert(h == 1080 && fps == 60);
    }
    bc_stop();
    check_stopped_clean();
    {
        int w, h, fps;
        bc_profile(&w, &h, &fps);
        assert(w == 1280 && h == 720 && fps == 30);
    }
    cases++;

    // A helper that ignores the stop: the core closes on it after its timeout and is not stuck.
    reset(H_DEAF);
    assert(bc_start() == 0);
    WAIT(!strcmp(bc_state(), "live"), 5000);
    bc_stop();
    WAIT(hs.stops == 1, 2000);
    WAIT(!strcmp(bc_state(), "off"), 6000);
    check_stopped_clean();
    cases++;

    // Backend selection: Auto goes by a DualSense on USB (Remote Play takes it over, as before),
    // otherwise the wireless capture; the pref overrides either way. Remote Play on the host has
    // no console under it: it fails with its own message, which tells which backend was chosen.
    bc_set_usb_probe(usb_probe);
    pref_backend = 0;
    usb_on = 0;
    rp_paired = -1;
    bc_set_usb_probe(usb_probe);
    assert(!bc_needs_pairing() && !strcmp(bc_controller(), "ok")); // auto, no USB pad: wireless
    usb_on = 1;
    bc_set_usb_probe(usb_probe);
    assert(!bc_needs_pairing()); // auto, pad on USB but Remote Play not paired: it could not run, so wireless
    rp_paired = 5;
    assert(bc_needs_pairing()); // auto, pad on USB, paired: Remote Play
    reset(H_NORMAL);
    assert(bc_start() == 0);
    WAIT(!strcmp(bc_state(), "error"), 3000);
    assert(!strcmp(bc_error(), "Broadcasting needs the console.") && launches == 0); // Remote Play's run()
    WAIT(alerts == 1, 1000);
    rp_paired = -1;
    pref_backend = 1; // wireless forced: the pad on USB is left alone, no pairing asked
    assert(!bc_needs_pairing() && !strcmp(bc_controller(), "ok"));
    reset(H_NORMAL);
    assert(bc_start() == 0);
    WAIT(!strcmp(bc_state(), "live"), 5000);
    assert(launches == 1 && bc_holds_controller() == 0);
    bc_stop();
    check_stopped_clean();
    pref_backend = 2; // Remote Play forced without a pad on USB or a pairing: its own error
    usb_on = 0;
    bc_set_usb_probe(usb_probe);
    assert(bc_needs_pairing());
    reset(H_NORMAL);
    assert(bc_start() == -1 && strstr(bc_error(), "not paired") && launches == 0);
    pref_backend = 0; // auto again: a running wireless Go Live keeps its backend when a pad appears
    bc_set_usb_probe(usb_probe);
    reset(H_NORMAL);
    assert(bc_start() == 0);
    WAIT(!strcmp(bc_state(), "live"), 5000);
    usb_on = 1;
    rp_paired = 5; // now Auto would pick Remote Play, but this Go Live is running wireless
    bc_set_usb_probe(usb_probe);
    assert(!bc_needs_pairing() && !strcmp(bc_controller(), "ok"));
    assert(bc_start() == 0 && launches == 1); // a repeat while running is the same Go Live
    bc_stop();
    check_stopped_clean();
    usb_on = 0;
    rp_paired = -1;
    cases += 5;

    printf("PASS: wireless backend: %d scenarios against a replayed console recording\n", cases);
    return 0;
}
