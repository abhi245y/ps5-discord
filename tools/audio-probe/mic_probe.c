// SPDX-License-Identifier: GPL-3.0-or-later
// sceAudioIn (microphone) probe for ps5-discord. Payload only, one shot.
// Opens each candidate (user, type, freq, param), captures ~3 s from every
// successful open, logs return codes and per-100 ms RMS/peak levels.
// Hard limit 60 s. No fork, no watchdog, no daemon.
// Never calls update/PUP or network/auth functions.
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PROBE_DIR "/data/ps5-discord-probe"
#define PROBE_LOG PROBE_DIR "/mic.txt"
#define DEADLINE_MS 60000u
#define CAPTURE_MS 3000u
#define WINDOW_MS 100u
#define GRAIN_FRAMES 256u
#define GRAIN_BYTES (GRAIN_FRAMES * 2u) // S16 mono assumed
#define MAX_SUCCESS 32

// Sce user ids: SYSTEM 0xff and INVALID -1 from OpenOrbis _types/user.h.
#define USER_SYSTEM 0xffu

// Candidate port types. Only type 0 and 2 have any source (Vita SDK audioin.h:
// SCE_AUDIO_IN_PORT_TYPE_VOICE = 0, SCE_AUDIO_IN_PORT_TYPE_RAW = 2; seen via a
// search snippet, not PS5 verified). The rest are swept, not named.
static const int kTypes[] = {0, 1, 2, 3, 4, 5, 6, 7};
static const unsigned kFreqs[] = {16000, 48000};
static const unsigned kParams[] = {0, 1}; // format param; 0/1 guessed

int sceUserServiceInitialize(void *);
int sceUserServiceTerminate(void);
int sceUserServiceGetForegroundUser(int32_t *);

static int (*p_init)(void);
static int (*p_open)(int user, int type, int index, unsigned grain, unsigned freq, unsigned param);
static int (*p_input)(int handle, void *buf);
static int (*p_close)(int handle);

static FILE *g_log;
static uint64_t g_t0;
static char g_success[MAX_SUCCESS][160];
static int g_nsuccess;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int past_deadline(void)
{
    return now_ms() - g_t0 >= DEADLINE_MS;
}

// Mirrors every line to stdout, stderr (klog/elfldr capture) and the file.
static void log_line(const char *fmt, ...)
{
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stdout, "[mic-probe] %s\n", line);
    fprintf(stderr, "[mic-probe] %s\n", line);
    fflush(stdout);
    fflush(stderr);
    if (g_log) {
        fprintf(g_log, "t=%llums %s\n", (unsigned long long)(now_ms() - g_t0), line);
        fflush(g_log);
    }
}

static int load_audioin(void)
{
    void *lib = dlopen("libSceAudioIn.sprx", RTLD_LAZY);
    if (!lib) {
        log_line("dlopen libSceAudioIn.sprx failed: %s", dlerror());
        return -1;
    }
    *(void **)&p_open = dlsym(lib, "sceAudioInOpen");
    *(void **)&p_input = dlsym(lib, "sceAudioInInput");
    *(void **)&p_close = dlsym(lib, "sceAudioInClose");
    *(void **)&p_init = dlsym(lib, "sceAudioInInit"); // optional
    log_line("symbols: open=%p input=%p close=%p init=%p", (void *)p_open,
             (void *)p_input, (void *)p_close, (void *)p_init);
    return (p_open && p_input && p_close) ? 0 : -1;
}

// Captures ~3 s of samples from an open handle, emitting one line per 100 ms.
static void capture(int h, unsigned user, int type, unsigned freq, unsigned param)
{
    static uint8_t buf[GRAIN_BYTES];
    const uint64_t start = now_ms();
    uint64_t win_start = start;
    double sumsq = 0;
    int peak = 0, reads = 0, idx = 0;
    long n = 0;
    int first = 1;

    while (now_ms() - start < CAPTURE_MS && !past_deadline()) {
        const int rc = p_input(h, buf);
        reads++;
        if (rc < 0) {
            log_line("  input rc=0x%08x after %d reads, stopping capture", (unsigned)rc, reads);
            break;
        }
        if (first) {
            log_line("  first input rc=%d raw[0..15]=%02x %02x %02x %02x %02x %02x %02x %02x"
                     " %02x %02x %02x %02x %02x %02x %02x %02x",
                     rc, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
                     buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
            first = 0;
        }
        for (unsigned i = 0; i + 1 < GRAIN_BYTES; i += 2) {
            const int s = (int16_t)(buf[i] | (buf[i + 1] << 8));
            const int a = s < 0 ? -s : s;
            sumsq += (double)s * s;
            if (a > peak)
                peak = a;
            n++;
        }
        if (now_ms() - win_start >= WINDOW_MS && n > 0) {
            log_line("  t=%4u ms rms=%7.1f peak=%5d /32767", idx * WINDOW_MS,
                     __builtin_sqrt(sumsq / (double)n), peak);
            idx++;
            sumsq = 0;
            peak = 0;
            n = 0;
            win_start = now_ms();
        }
    }
    if (n > 0)
        log_line("  t=%4u ms rms=%7.1f peak=%5d /32767 (partial)", idx * WINDOW_MS,
                 __builtin_sqrt(sumsq / (double)n), peak);
    log_line("  capture done: reads=%d", reads);

    if (g_nsuccess < MAX_SUCCESS)
        snprintf(g_success[g_nsuccess++], sizeof g_success[0],
                 "user=0x%02x type=%d freq=%u param=%u handle=%d reads=%d",
                 user, type, freq, param, h, reads);
}

int main(void)
{
    g_t0 = now_ms();
    mkdir(PROBE_DIR, 0777); // may already exist
    g_log = fopen(PROBE_LOG, "a");
    log_line("start: sceAudioIn probe, 60 s hard limit, grain=%u frames", GRAIN_FRAMES);

    if (load_audioin() != 0) {
        log_line("abort: sceAudioIn symbols unavailable");
        goto finish;
    }
    if (p_init)
        log_line("sceAudioInInit rc=0x%08x", (unsigned)p_init());

    int rc = sceUserServiceInitialize(NULL);
    log_line("sceUserServiceInitialize rc=0x%08x", (unsigned)rc);
    int32_t fg = -1;
    const int fgrc = sceUserServiceGetForegroundUser(&fg);
    log_line("sceUserServiceGetForegroundUser rc=0x%08x user=0x%02x", (unsigned)fgrc,
             (unsigned)(uint32_t)fg);

    unsigned users[2];
    int nusers = 0;
    if (fgrc == 0 && fg >= 0 && (unsigned)fg != USER_SYSTEM)
        users[nusers++] = (unsigned)fg;
    users[nusers++] = USER_SYSTEM;

    for (int u = 0; u < nusers; u++) {
        for (unsigned t = 0; t < sizeof kTypes / sizeof kTypes[0]; t++) {
            for (unsigned f = 0; f < sizeof kFreqs / sizeof kFreqs[0]; f++) {
                for (unsigned p = 0; p < sizeof kParams / sizeof kParams[0]; p++) {
                    if (past_deadline()) {
                        log_line("deadline reached; skipping remaining opens");
                        goto summary;
                    }
                    const int h = p_open((int)users[u], kTypes[t], 0, GRAIN_FRAMES,
                                         kFreqs[f], kParams[p]);
                    log_line("open user=0x%02x type=%d freq=%u param=%u -> 0x%08x",
                             users[u], kTypes[t], kFreqs[f], kParams[p], (unsigned)h);
                    if (h < 0)
                        continue;
                    log_line("  OPEN OK: capturing ~3 s, speak or tap the controller now");
                    capture(h, users[u], kTypes[t], kFreqs[f], kParams[p]);
                    const int crc = p_close(h);
                    log_line("  close rc=0x%08x", (unsigned)crc);
                }
            }
        }
    }

summary:
    log_line("summary: %d successful open(s)", g_nsuccess);
    for (int i = 0; i < g_nsuccess; i++)
        log_line("  OK %s", g_success[i]);
    rc = sceUserServiceTerminate();
    log_line("sceUserServiceTerminate rc=0x%08x", (unsigned)rc);

finish:
    log_line("done in %llu ms", (unsigned long long)(now_ms() - g_t0));
    if (g_log)
        fclose(g_log);
    return 0;
}
