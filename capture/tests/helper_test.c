// SPDX-License-Identifier: GPL-3.0-only
// The helper's lifecycle and recovery, with a fake transport for the borrowed process (no
// ptrace, no console) and a real loopback socket to a fake core. Ported from the PoC's
// tests/record_lifecycle.c: same fake calls, the file writers replaced by the socket.
#define CAPTURE_NO_MAIN 1
#define CAPTURE_QUIET 1
#define CAP_DIR "."
#define CONN_FILE "./helper-test.conn"
#define LOCK_PATH "./helper-test.lock"
#define LOG_PATH "./helper-test.log"
#define NO_PICTURE_MS 1200u
#define MODE_WAIT_MS 700u
#define START_WAIT_MS 600u
#include "../src/helper.c"
#undef main
#include <assert.h>

enum scenario { OK, VIDEO_OPEN_FAIL, VIDEO_START_FAIL, BAD_HEADERS, VIDEO_OVERSIZE, TRANSPORT_FAIL, CALLBACK_FAIL,
                NO_IDR, MODE_120HZ, CLOSE_FAIL, MODE_TRANSITION, MODE_CHANGED, CORE_CLOSES, BAD_PROFILE,
                SCENARIO_COUNT,
                RECOVER_OK, RECOVER_ZERO_MODE, RECOVER_REOPEN_FAIL, RECOVER_SECOND_FAULT, RECOVER_BAD_CLEANUP,
                RECOVER_TRANSPORT, RECOVER_120HZ, RECOVER_LAST = RECOVER_120HZ };
static enum scenario scenario;
static uint8_t scratch[0x4000], video_mem[FRAME_CAP];
static const uint64_t video_handle = 0x1234567800000012ull;
static unsigned calls[FUNCTION_COUNT], vstop, vclose, detached, freed, attachments, mode_samples, reads_in_session;
static int saw_video_start, saw_bitrate, error_fired;
static struct start_req profile;

int sl_inject_begin(const char *name, sl_inject *ctx)
{
    assert(!strcmp(name, "SceRemotePlay"));
    memset(ctx, 0, sizeof *ctx);
    ctx->attached = 1; ctx->target_pid = 123;
    attachments++; reads_in_session = 0; saw_video_start = saw_bitrate = 0;
    ctx->scratch = (intptr_t)scratch; ctx->scratch_len = sizeof scratch;
    return 0;
}
int sl_inject_end(sl_inject *ctx) { assert(ctx->attached); ctx->attached = 0; detached++; return 0; }
intptr_t sl_resolve_in(pid_t pid, const char *module, const char *name)
{
    assert(pid == 123 && module);
    for (unsigned i = 0; i < FUNCTION_COUNT; i++)
        if (!strcmp(name, symbols[i])) return i + 1;
    assert(0); return 0;
}
int sl_inject_read_at(sl_inject *ctx, intptr_t address, void *out, size_t n)
{
    assert(ctx->attached && !ctx->running);
    if ((uintptr_t)address >= (uintptr_t)scratch && (uintptr_t)address < (uintptr_t)scratch + sizeof scratch)
        assert((uintptr_t)address + n <= (uintptr_t)scratch + sizeof scratch);
    else assert(address == (intptr_t)video_mem && n <= sizeof video_mem);
    memcpy(out, (void *)address, n); return 0;
}
int sl_inject_write_at(sl_inject *ctx, intptr_t address, const void *input, size_t n)
{
    assert(ctx->attached && !ctx->running);
    assert((uintptr_t)address >= (uintptr_t)scratch && (uintptr_t)address + n <= (uintptr_t)scratch + sizeof scratch);
    memcpy((void *)address, input, n); return 0;
}

static int recovery_scenario(void) { return scenario >= RECOVER_OK; }

int64_t inj_call_fast(sl_inject *ctx, intptr_t fn, const uint64_t a[6], int save_fp)
{
    assert(ctx->attached && !ctx->quarantined && save_fp == 1);
    unsigned id = (unsigned)fn - 1;
    assert(id < FUNCTION_COUNT);
    calls[id]++;
    switch (id) {
    case MODE:
        if (scenario == MODE_TRANSITION) *(uint32_t *)(uintptr_t)a[0] = 0;
        else if (scenario == MODE_120HZ) *(uint32_t *)(uintptr_t)a[0] = 0x40;
        else if (recovery_scenario() && error_fired && attachments == 1) {
            mode_samples++;
            *(uint32_t *)(uintptr_t)a[0] = scenario == RECOVER_ZERO_MODE ? 0 : scenario == RECOVER_120HZ ? 0x40 :
                                           mode_samples < 3 ? 0 : 0x13c;
        } else if (recovery_scenario() && attachments > 1) *(uint32_t *)(uintptr_t)a[0] = 0x13c;
        else *(uint32_t *)(uintptr_t)a[0] = scenario == MODE_CHANGED && calls[READ_VIDEO] > 0 ? 0x40 : 0x30;
        break;
    case SET_CALLBACK:
        assert(a[0] == FEATURE && a[1] == REMOTE_GETPID + 1);
        if (scenario == CALLBACK_FAIL) return -1;
        break;
    case CHANGE_STATE: assert(!strcmp((const char *)(uintptr_t)a[1], "RUNNING")); break;
    case OPEN_VIDEO:
        assert(a[0] == (uint64_t)(uintptr_t)scratch && a[1] == a[0] + 8 && a[2] == 0);
        assert(*(uint32_t *)(scratch + 8) == 0x2e0);
        assert(*(uint32_t *)(scratch + 0x20) == profile.selector);
        assert(*(uint32_t *)(scratch + 0x24) == profile.fps_flag);
        assert(*(uint32_t *)(scratch + 0x2c) == 0);
        assert(*(uint32_t *)(scratch + 0xd0) == profile.bitrate && *(uint32_t *)(scratch + 0xd4) == profile.bitrate);
        assert(*(uint32_t *)(scratch + 0x210) == profile.width && *(uint32_t *)(scratch + 0x214) == profile.height);
        assert(*(uint32_t *)(scratch + 0x2a8) == (scenario == MODE_120HZ ? 0x40u : recovery_scenario() && attachments > 1 ? 0x13cu : 0x30u));
        if (scenario == RECOVER_REOPEN_FAIL && attachments > 1) return -1;
        if (scenario == VIDEO_OPEN_FAIL) return -1;
        *(uint64_t *)(uintptr_t)a[0] = video_handle; break;
    case BITRATE_SET:
        assert(a[0] == video_handle && !saw_video_start);   // before Start, never after
        assert(*(uint32_t *)(scratch + 0x780 + 8) == profile.bitrate);
        saw_bitrate = 1; break;
    case START:
        assert(a[0] == video_handle && saw_bitrate);
        saw_video_start = 1;
        if (scenario == VIDEO_START_FAIL) return -1;
        break;
    case STOP: assert(a[0] == video_handle); vstop++; break;
    case CLOSE:
        assert(a[0] == video_handle); vclose++;
        if (scenario == CLOSE_FAIL || scenario == RECOVER_BAD_CLEANUP) return -1;
        break;
    case HEADERS: {
        assert(a[0] == video_handle && saw_video_start);
        uint8_t *p = (void *)(uintptr_t)a[1];
        put32(p, 0x10, scenario == BAD_HEADERS ? 129 : 5); put32(p, 0x98, 2);
        memcpy(p + 0x18, "\x67\x4d\x40\x28\x91", 5); p[0xa0] = 0x68; p[0xa1] = 0xee;
        break;
    }
    case KEYFRAME: assert(a[0] == video_handle); break;
    case READ_VIDEO:
        assert(a[0] == video_handle && a[1] == (uint64_t)(uintptr_t)video_mem && a[2] == FRAME_CAP && a[4] == 0);
        usleep(1000);
        reads_in_session++;
        if (scenario == TRANSPORT_FAIL) { ctx->quarantined = 1; return SL_CALL_ERROR; }
        if (recovery_scenario() && attachments == 1 && reads_in_session == 3) {
            error_fired = 1;
            if (scenario == RECOVER_TRANSPORT) { ctx->quarantined = 1; return SL_CALL_ERROR; }
            return (int64_t)(int32_t)0x81950009u;
        }
        if (scenario == RECOVER_SECOND_FAULT && attachments > 1 && reads_in_session == 3)
            return (int64_t)(int32_t)0x81950009u;
        if (scenario == VIDEO_OVERSIZE) return FRAME_CAP + 1;
        // an IDR, or (second read on) a P slice, as Annex-B access units
        if (scenario == NO_IDR || (reads_in_session % 2 == 0 && scenario != OK)) memcpy(video_mem, "\0\0\0\1\x41\x80", 6);
        else memcpy(video_mem, "\0\0\0\1\x65\x80", 6);
        return 6;
    case REMOTE_MALLOC: return (intptr_t)video_mem;
    case REMOTE_FREE: assert(a[0] == (uint64_t)(uintptr_t)video_mem); freed++; return (int64_t)0xffffffffu;
    default: break;
    }
    return 0;
}

// ---- the fake core -------------------------------------------------------------------------

static const uint8_t token[CAP_TOKEN_BYTES] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0xfe};
struct seen {
    unsigned videos, keys, headers, profiles, states[8];
    int last_state, hello;
    char error[128];
    uint8_t headers_bytes[64];
    size_t headers_len;
    uint32_t profile_w, profile_h, profile_fps, profile_mode;
    unsigned frames_after_recovering;
};

static int read_exact(int fd, void *p, size_t n, int timeout_ms)
{
    uint8_t *b = p;
    while (n) {
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        int r = poll(&pf, 1, timeout_ms);
        if (r == 0) return 0;
        if (r < 0) return -1;
        ssize_t got = recv(fd, b, n, 0);
        if (got <= 0) return -1;
        b += got; n -= (size_t)got;
    }
    return 1;
}

static void send_start(int fd, const struct start_req *r)
{
    uint8_t m[CAP_HEADER_BYTES + 24];
    cap_pack_header(m, CAP_START, 0, 24);
    cap_put32(m + 8, r->selector); cap_put32(m + 12, r->fps_flag); cap_put32(m + 16, r->bitrate);
    cap_put32(m + 20, r->width); cap_put32(m + 24, r->height); cap_put32(m + 28, r->fps);
    assert(send(fd, m, sizeof m, 0) == (ssize_t)sizeof m);
}

static void send_simple(int fd, uint8_t type)
{
    uint8_t m[CAP_HEADER_BYTES];
    cap_pack_header(m, type, 0, 0);
    assert(send(fd, m, sizeof m, 0) == (ssize_t)sizeof m);
}

static void *helper_thread(void *arg) { (void)arg; return (void *)(intptr_t)helper_run(); }

static int run_helper(const struct start_req *req, struct seen *s, int *rc_out, int write_conn, int send_start_msg)
{
    memset(s, 0, sizeof *s);
    int listener = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, (struct sockaddr *)&a, sizeof a) == 0 && listen(listener, 2) == 0);
    socklen_t al = sizeof a;
    assert(getsockname(listener, (struct sockaddr *)&a, &al) == 0);
    if (write_conn) {
        FILE *f = fopen(CONN_FILE, "w");
        fprintf(f, "%u ", ntohs(a.sin_port));
        for (unsigned i = 0; i < CAP_TOKEN_BYTES; i++) fprintf(f, "%02x", token[i]);
        fprintf(f, "\n");
        fclose(f);
    } else unlink(CONN_FILE);
    net_fd = -1; net_dead = stop_requested = key_requested = 0; inbox_len = 0; quit = 0;
    pthread_t t;
    pthread_create(&t, NULL, helper_thread, NULL);
    int fd = -1;
    struct pollfd pf = {.fd = listener, .events = POLLIN};
    if (poll(&pf, 1, 2000) > 0) fd = accept(listener, NULL, NULL);
    int stopped = 0, keyed = 0;
    if (fd >= 0) {
        uint8_t h[CAP_HEADER_BYTES];
        static uint8_t buf[0x10000];
        int r = read_exact(fd, h, sizeof h, 2000);
        cap_header hh;
        if (r == 1 && !cap_parse_header(h, &hh) && hh.type == CAP_HELLO && hh.len == 4 + CAP_TOKEN_BYTES) {
            assert(read_exact(fd, buf, hh.len, 1000) == 1);
            s->hello = cap_get32(buf) == CAP_VERSION && !memcmp(buf + 4, token, CAP_TOKEN_BYTES);
        }
        if (send_start_msg) send_start(fd, req);
        unsigned want_videos = scenario == RECOVER_OK ? 6 : 8;
        for (;;) {
            r = read_exact(fd, h, sizeof h, scenario == NO_IDR || scenario == MODE_TRANSITION ? 4000 : 3000);
            if (r <= 0) break;
            assert(!cap_parse_header(h, &hh));
            assert(hh.len < sizeof buf);
            if (hh.len) assert(read_exact(fd, buf, hh.len, 1000) == 1);
            switch (hh.type) {
            case CAP_VIDEO:
                s->videos++;
                if (hh.flags & CAP_KEY) s->keys++;
                assert(hh.len == 6 && !memcmp(buf, "\0\0\0\1", 4));
                if (s->last_state == CAP_RECOVERING || s->headers > 1) s->frames_after_recovering++;
                break;
            case CAP_HEADERS:
                s->headers++;
                s->headers_len = hh.len;
                memcpy(s->headers_bytes, buf, hh.len < sizeof s->headers_bytes ? hh.len : sizeof s->headers_bytes);
                break;
            case CAP_PROFILE:
                assert(hh.len == 16);
                s->profiles++;
                s->profile_w = cap_get32(buf); s->profile_h = cap_get32(buf + 4);
                s->profile_fps = cap_get32(buf + 8); s->profile_mode = cap_get32(buf + 12);
                break;
            case CAP_STATE:
                assert(hh.len >= 1 && buf[0] >= 1 && buf[0] <= 4);
                s->last_state = buf[0];
                s->states[buf[0]]++;
                if (buf[0] == CAP_ERROR) { memcpy(s->error, buf + 1, hh.len - 1); s->error[hh.len - 1] = 0; }
                break;
            default: assert(0);
            }
            if (s->videos >= 2 && !keyed) { send_simple(fd, CAP_KEYFRAME); keyed = 1; }
            if (!stopped && s->videos >= want_videos && (scenario != RECOVER_OK || s->frames_after_recovering >= 3) &&
                s->last_state == CAP_LIVE) {
                stopped = 1;
                if (scenario == CORE_CLOSES) { close(fd); fd = -1; break; }
                send_simple(fd, CAP_STOP);
            }
        }
        if (fd >= 0) close(fd);
    }
    close(listener);
    void *res = NULL;
    pthread_join(t, &res);
    *rc_out = (int)(intptr_t)res;
    return 0;
}

static void reset_fake(void)
{
    memset(scratch, 0xab, sizeof scratch);
    memset(calls, 0, sizeof calls);
    vstop = vclose = detached = freed = attachments = mode_samples = reads_in_session = 0;
    saw_video_start = saw_bitrate = error_fired = 0;
}

static const struct start_req profiles[] = {
    {3, 0, 10000000, 1280, 720, 30}, {3, 1, 10000000, 1280, 720, 60},
    {4, 0, 15000000, 1920, 1080, 30}, {4, 1, 15000000, 1920, 1080, 60},
};

int main(void)
{
    char dir[] = "/tmp/capture-helper-XXXXXX";
    assert(mkdtemp(dir) && !chdir(dir));
    log_file = fopen(LOG_PATH, "w");
    assert(watchdog_state(0) == 0 && watchdog_state(14999) == 0);
    assert(watchdog_state(15000) == 1 && watchdog_state(24999) == 1 && watchdog_state(25000) == 2);
    // no total duration: a run of any length with progress never trips
    assert(watchdog_state(100) == 0);
    int cases = 0;

    // Every profile: the selector/fps flag/bitrate/size/rate budget reach the encoder, the bitrate
    // is set once before Start, headers and profile arrive before the first IDR, keyframe request
    // round trip, clean stop by the core's CAP_STOP.
    for (unsigned p = 0; p < 4; p++) {
        scenario = OK; profile = profiles[p];
        reset_fake();
        struct seen s; int rc;
        run_helper(&profile, &s, &rc, 1, 1);
        assert(s.hello && rc == 0);
        assert(s.profiles == 1 && s.profile_w == profile.width && s.profile_h == profile.height &&
               s.profile_fps == profile.fps && s.profile_mode == 0x30);
        assert(s.headers == 1 && s.headers_len == 4 + 5 + 4 + 2 && !memcmp(s.headers_bytes, "\0\0\0\1\x67\x4d\x40\x28\x91\0\0\0\1\x68\xee", 15));
        assert(s.states[CAP_STARTING] == 1 && s.states[CAP_LIVE] == 1 && !s.states[CAP_ERROR] && !s.states[CAP_RECOVERING]);
        assert(s.videos >= 8 && s.keys == s.videos);
        assert(calls[KEYFRAME] >= 2); // once at setup, once for the core's request
        assert(calls[BITRATE_SET] == 1);
        assert(vstop == 1 && vclose == 1 && freed == 1 && calls[CLEAR_CALLBACK] == 1 && calls[TERMINATE] == 1);
        assert(attachments == 1 && detached == 1);
        for (size_t j = 0; j < sizeof scratch; j++) assert(scratch[j] == 0xab);
        cases++;
    }

    for (scenario = VIDEO_OPEN_FAIL; scenario < SCENARIO_COUNT; scenario++) {
        profile = profiles[0];
        if (scenario == BAD_PROFILE) profile.width = 1920; // selector 3 with 1080p dimensions
        reset_fake();
        struct seen s; int rc;
        run_helper(&profile, &s, &rc, 1, 1);
        const int quarantined = scenario == TRANSPORT_FAIL;
        const int ends_ok = scenario == MODE_CHANGED || scenario == CORE_CLOSES;
        const int refused_mode = scenario == MODE_TRANSITION || scenario == MODE_120HZ;
        if (scenario == BAD_PROFILE) {
            assert(rc == 1 && attachments == 0 && s.states[CAP_ERROR] == 1);
            cases++;
            continue;
        }
        assert(s.hello);
        assert((rc == 0) == ends_ok);
        assert(attachments == 1 && detached == 1);
        for (size_t j = 0; j < sizeof scratch; j++) assert(scratch[j] == 0xab);
        assert(calls[TERMINATE] == (unsigned)!quarantined);
        assert(calls[CLEAR_CALLBACK] == (unsigned)(scenario != CALLBACK_FAIL && !quarantined && scenario != MODE_TRANSITION &&
                                                    scenario != MODE_120HZ));
        if (!ends_ok) assert(s.states[CAP_ERROR] == 1 && s.error[0]); // the core is told why
        if (ends_ok) assert(s.states[CAP_LIVE] == 1 && !s.states[CAP_ERROR] && s.videos >= 8 && vstop == 1 && vclose == 1 && freed == 1);
        if (scenario == VIDEO_OPEN_FAIL) assert(vclose == 0);
        if (scenario == VIDEO_START_FAIL) assert(vstop == 1 && vclose == 1);
        if (refused_mode) assert(calls[OPEN_VIDEO] == 0 && s.videos == 0);
        if (scenario == MODE_120HZ) assert(strstr(s.error, "120 Hz"));
        if (scenario == NO_IDR) assert(s.videos == 0 && !s.states[CAP_LIVE] && calls[KEYFRAME] >= 2);
        if (scenario == BAD_HEADERS) assert(s.headers == 0 && s.videos == 0);
        if (scenario == VIDEO_OVERSIZE || scenario == TRANSPORT_FAIL) assert(attachments == 1 && s.videos == 0);
        if (quarantined) assert(calls[CLOSE] == 0 && calls[REMOTE_FREE] == 0);
        if (scenario == CLOSE_FAIL) assert(s.states[CAP_ERROR] == 1 || rc == 1);
        cases++;
    }

    // Recovery exactly as RECOVERY.md: only 0x81950009, three equal nonzero mode readings, a clean
    // teardown, one fresh session with its own SPS/PPS, then no more.
    for (scenario = RECOVER_OK; scenario <= RECOVER_LAST; scenario++) {
        profile = profiles[0];
        reset_fake();
        struct seen s; int rc;
        run_helper(&profile, &s, &rc, 1, 1);
        const int reopens = scenario == RECOVER_OK || scenario == RECOVER_REOPEN_FAIL || scenario == RECOVER_SECOND_FAULT;
        const unsigned expected = reopens ? 2 : 1;
        assert(s.hello);
        assert(attachments == expected && detached == expected && calls[OPEN_VIDEO] == expected);
        assert(calls[TERMINATE] == (scenario == RECOVER_TRANSPORT ? 0 : expected));
        for (size_t j = 0; j < sizeof scratch; j++) assert(scratch[j] == 0xab);
        if (scenario == RECOVER_OK) {
            assert(rc == 0 && s.states[CAP_RECOVERING] == 1 && s.states[CAP_LIVE] == 2 && !s.states[CAP_ERROR]);
            assert(s.headers == 2 && s.profiles == 2 && s.profile_mode == 0x13c); // re-read SPS/PPS, new mode
            assert(s.frames_after_recovering >= 3 && s.keys >= 1);
            assert(calls[KEYFRAME] >= 3); // an IDR requested in each session
            assert(vclose == 2 && vstop == 2);
        } else {
            assert(rc == 1 && s.states[CAP_ERROR] == 1 && s.error[0]);
            if (scenario == RECOVER_ZERO_MODE || scenario == RECOVER_BAD_CLEANUP || scenario == RECOVER_TRANSPORT ||
                scenario == RECOVER_120HZ) assert(s.states[CAP_LIVE] == 1);
            if (scenario == RECOVER_REOPEN_FAIL) assert(s.states[CAP_LIVE] == 1 && s.states[CAP_RECOVERING] == 1);
            if (scenario == RECOVER_SECOND_FAULT) assert(s.states[CAP_RECOVERING] == 1 && s.states[CAP_LIVE] == 2); // no third session
            if (scenario == RECOVER_120HZ) assert(strstr(s.error, "120 Hz"));
        }
        cases++;
    }

    // A missing connection file and a core that never sends a start are clean failures: nothing
    // attached, nothing touched.
    scenario = OK; profile = profiles[0]; reset_fake();
    {
        struct seen s; int rc;
        run_helper(&profile, &s, &rc, 0, 1);
        assert(rc == 1 && attachments == 0 && !s.hello);
        reset_fake();
        run_helper(&profile, &s, &rc, 1, 0);
        assert(rc == 1 && attachments == 0 && s.hello);
        cases += 2;
    }
    fclose(log_file);
    unlink(CONN_FILE); unlink(LOCK_PATH); unlink(LOG_PATH);
    assert(!chdir("/") && !rmdir(dir));
    printf("PASS: %d helper lifecycle/recovery cases (4 profiles, failures, recovery, mode guard); no hardware calls performed\n", cases);
    return 0;
}
