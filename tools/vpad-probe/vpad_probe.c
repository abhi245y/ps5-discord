// SPDX-License-Identifier: AGPL-3.0-or-later
/* Standalone, bounded /dev/hid virtual-pad research probe for ps5-discord.
 * Independently written from the documented ABI in FGGstore/FGG-XSense
 * (GPL-3.0), src/vpad.c:13-75,96-106,119-140,187-204 and src/vpad.h:41.
 * Credits also to sinfiltros/AnyPad-PS5-English (GPL-3.0-or-later),
 * src/ps5_vpad.c:253-319,534-613 and PRUEBAS.md:360-395 (user routing),
 * and Ghostpad (GPL-3.0-or-later), virtualDS5research.md:282-339
 * (button/trigger observations). Reference snapshots are in tmp/research/.
 * No libScePad, Remote Play, process injection, or physical-pad operations.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ps5/klog.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PROBE_DIR "/data/ps5-discord"
#define LOG_PATH PROBE_DIR "/vpad-probe.log"
#define USER_PATH PROBE_DIR "/vpad-probe-user"
#define LOCK_PATH PROBE_DIR "/vpad-probe.lock"
#define RUN_MS 30000u
#define FRAME_MS 16u

#define HID_ADD_DEVICE  0xC018482AUL
#define HID_INSERT_DATA 0x8018482CUL
#define HID_DEL_DEVICE  0x80104850UL
#define BUTTON_RIGHT 0x000020u
#define BUTTON_LEFT  0x000080u
#define BUTTON_L2    0x000100u

/* Native PS5 x86-64 layouts, including the reserved bytes and pointer slots. */
struct hid_request {
    uint32_t command;
    uint32_t reserved;
    void *data;
    void *out;
};

struct hid_delete {
    uint8_t command;
    uint8_t reserved[7];
    uint32_t handle;
    uint32_t reserved2;
};

_Static_assert(sizeof(void *) == 8, "PS5 requires 64-bit pointers");
_Static_assert(sizeof(struct hid_request) == 24, "add/insert request size");
_Static_assert(offsetof(struct hid_request, data) == 8, "data pointer offset");
_Static_assert(offsetof(struct hid_request, out) == 16, "output pointer offset");
_Static_assert(sizeof(struct hid_delete) == 16, "delete request size");
_Static_assert(offsetof(struct hid_delete, handle) == 8, "delete handle offset");

int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int32_t *user);
int sceUserServiceGetInitialUser(int32_t *user);

static FILE *g_log;
static int g_hid = -1, g_lock = -1, g_added, g_failed, g_cleaned;
static int32_t g_handle = -1;
static uint64_t g_start;
static volatile sig_atomic_t g_stop;
static int g_quiet; /* InsertData every 16 ms: log the first per step and failures only */

static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        g_failed = 1;
        return g_start + RUN_MS; /* Fail closed if the clock stops working. */
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Every line carries PID and monotonic elapsed time, including each ioctl. */
static void log_line(const char *fmt, ...)
{
    int saved = errno;
    char message[512], line[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "[vpad-probe pid=%d t=%llums] %s\n",
             (int)getpid(), (unsigned long long)(now_ms() - g_start), message);
    if (klog_printf("%s", line) < 0)
        g_failed = 1;
    if (g_log && (fputs(line, g_log) == EOF || fflush(g_log) != 0)) {
        int error = errno;
        g_failed = 1;
        klog_printf("[vpad-probe pid=%d t=%llums] log write failed errno=%d\n",
                    (int)getpid(), (unsigned long long)(now_ms() - g_start), error);
    }
    errno = saved;
}

static void stop_signal(int signo)
{
    g_stop = signo; /* Cleanup and logging run in main, never in signal context. */
}

static int checked_ioctl(const char *name, unsigned long op, void *request)
{
    errno = 0;
    int rc = ioctl(g_hid, op, request);
    int error = errno;
    if (!g_quiet || rc != 0)
        log_line("%s ioctl=0x%08lx rc=%d errno=%d (%s) handle=0x%08x",
                 name, op, rc, error, strerror(error), (unsigned)g_handle);
    if (rc != 0)
        g_failed = 1;
    return rc;
}

static void close_logged(int *fd, const char *name)
{
    if (*fd < 0)
        return;
    errno = 0;
    int rc = close(*fd);
    int error = errno;
    *fd = -1;
    log_line("close %s rc=%d errno=%d", name, rc, error);
    if (rc != 0)
        g_failed = 1;
}

static void cleanup(void)
{
    if (g_cleaned)
        return;
    g_cleaned = 1;
    if (g_added) {
        struct hid_delete request = { .command = 5, .handle = (uint32_t)g_handle };
        /* One bounded retry also covers an interrupted delete. Never add again. */
        for (int attempt = 1; attempt <= 2; ++attempt) {
            log_line("delete virtual pad attempt=%d", attempt);
            if (checked_ioctl("DeleteDevice", HID_DEL_DEVICE, &request) == 0) {
                g_added = 0;
                break;
            }
        }
        if (g_added)
            log_line("DELETE FAILED: virtual pad may remain; handle=0x%08x",
                     (unsigned)g_handle);
    }
    close_logged(&g_hid, "/dev/hid");
    /* Keep the inode: unlinking a flock file permits competing lock owners. */
    close_logged(&g_lock, LOCK_PATH);
    log_line("finish failed=%d signal=%d pad_remaining=%d", g_failed, (int)g_stop, g_added);
    if (g_log) {
        FILE *file = g_log;
        g_log = NULL;
        if (fclose(file) != 0) {
            g_failed = 1;
            log_line("close log failed errno=%d", errno);
        }
    }
}

static int running(void)
{
    if (g_stop || g_failed)
        return 0;
    if (now_ms() - g_start >= RUN_MS) {
        log_line("30 s deadline reached (or monotonic clock failed)");
        g_failed = 1;
        return 0;
    }
    return 1;
}

/* Short interruptible sleeps keep stop/deadline handling within one frame. */
static int wait_until(uint64_t end)
{
    while (running()) {
        uint64_t now = now_ms();
        if (now >= end)
            return 0;
        uint64_t ms = end - now;
        if (ms > FRAME_MS)
            ms = FRAME_MS;
        struct timespec delay = { .tv_sec = 0, .tv_nsec = (long)ms * 1000000L };
        if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
            log_line("nanosleep failed errno=%d", errno);
            g_failed = 1;
            return -1;
        }
    }
    return -1;
}

static int choose_user(uint32_t *user)
{
    int rc = sceUserServiceInitialize(NULL);
    log_line("sceUserServiceInitialize rc=0x%08x (may already be initialized)", (unsigned)rc);
    int32_t foreground = -1, initial = -1;
    int fg_rc = sceUserServiceGetForegroundUser(&foreground);
    int init_rc = sceUserServiceGetInitialUser(&initial);
    log_line("foreground rc=0x%08x user=0x%08x; initial rc=0x%08x user=0x%08x",
             (unsigned)fg_rc, (unsigned)foreground, (unsigned)init_rc, (unsigned)initial);

    errno = 0;
    FILE *file = fopen(USER_PATH, "r");
    int error = errno;
    log_line("open user override present=%d errno=%d", file != NULL, error);
    if (file) {
        char text[64];
        size_t n = fread(text, 1, sizeof text - 1, file);
        int invalid = ferror(file) || memchr(text, '\0', n) != NULL;
        if (fgetc(file) != EOF || ferror(file))
            invalid = 1;
        if (fclose(file) != 0)
            invalid = 1;
        text[n] = '\0';
        char *start = text, *end;
        while (isspace((unsigned char)*start))
            ++start;
        errno = 0;
        /* Decimal is decimal even with leading zeroes; hex requires 0x. */
        int base = start[0] == '0' && (start[1] == 'x' || start[1] == 'X') ? 16 : 10;
        unsigned long value = strtoul(start, &end, base);
        if (!isdigit((unsigned char)*start) || start == end || errno == ERANGE ||
            value >= UINT32_MAX)
            invalid = 1;
        while (isspace((unsigned char)*end))
            ++end;
        if (*end != '\0' || invalid) {
            log_line("invalid %s: require one decimal or 0x hex uint32 user ID, excluding -1", USER_PATH);
            return -1;
        }
        *user = (uint32_t)value;
        log_line("selected override user=%u (0x%08x)", (unsigned)*user, (unsigned)*user);
        return 0;
    }
    if (error != ENOENT) {
        log_line("cannot read user override errno=%d; refusing fallback", error);
        return -1;
    }
    if (fg_rc == 0 && foreground != -1)
        *user = (uint32_t)foreground;
    else if (init_rc == 0 && initial != -1)
        *user = (uint32_t)initial;
    else {
        log_line("no foreground or initial user available");
        return -1;
    }
    log_line("selected %s user=%u (0x%08x)",
             fg_rc == 0 && foreground != -1 ? "foreground" : "initial",
             (unsigned)*user, (unsigned)*user);
    return 0;
}

/* This is the kernel's 0xA0-byte input buffer, NOT a ScePadData struct. */
static int state_for(const char *step, uint32_t buttons, uint8_t lx, uint8_t l2, unsigned ms)
{
    uint8_t report[0xA0] = {0};
    float identity = 1.0f;
    if (buttons & ~(BUTTON_RIGHT | BUTTON_LEFT | BUTTON_L2)) {
        g_failed = 1;
        log_line("refused button mask=0x%08x", (unsigned)buttons);
        return -1;
    }
    memcpy(report + 0x0C, &buttons, sizeof buttons);
    memset(report + 0x10, 128, 4);
    report[0x10] = lx;
    report[0x14] = l2;
    report[0x15] = 0;
    report[0x17] = 1;
    memcpy(report + 0x78 + 12, &identity, sizeof identity);
    struct hid_request request = { .command = (uint32_t)g_handle, .data = report };
    log_line("step=%s buttons=0x%08x LX=%u L2=%u duration=%ums cadence=%ums",
             step, (unsigned)buttons, (unsigned)lx, (unsigned)l2, ms, FRAME_MS);
    uint64_t end = now_ms() + ms;
    g_quiet = 0;
    do {
        if (!running() || checked_ioctl("InsertData", HID_INSERT_DATA, &request) != 0)
            return g_quiet = 0, -1;
        g_quiet = 1;
        uint64_t next = now_ms() + FRAME_MS;
        if (wait_until(next < end ? next : end) != 0)
            return -1;
    } while (now_ms() < end);
    g_quiet = 0;
    return 0;
}

static int dpad_burst(const char *direction, uint32_t button)
{
    for (int i = 1; i <= 5; ++i) {
        log_line("D-pad %s pulse=%d/5", direction, i);
        if (state_for(direction, button, 128, 0, 150) != 0 ||
            state_for("D-pad release/gap", 0, 128, 0, 850) != 0)
            return -1;
    }
    return state_for("pause after D-pad burst", 0, 128, 0, 1000);
}

int main(void)
{
    int status = 1;
    uint32_t user;
    g_start = now_ms();
    if (atexit(cleanup) != 0) {
        log_line("atexit registration failed; no device opened");
        return 1;
    }
    if (mkdir(PROBE_DIR, 0755) != 0 && errno != EEXIST) {
        log_line("mkdir %s failed errno=%d", PROBE_DIR, errno);
        goto done;
    }
    g_log = fopen(LOG_PATH, "a");
    if (!g_log) {
        log_line("open append log %s failed errno=%d", LOG_PATH, errno);
        goto done;
    }
    log_line("start: direct /dev/hid probe; deadline=%ums; no Remote Play or physical-pad calls", RUN_MS);
    struct sigaction action = { .sa_handler = stop_signal };
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        log_line("install signal handlers failed errno=%d", errno);
        goto done;
    }
    g_lock = open(LOCK_PATH, O_CREAT | O_RDWR, 0600);
    if (g_lock < 0 || flock(g_lock, LOCK_EX | LOCK_NB) != 0) {
        log_line("single-instance lock unavailable errno=%d; no pad created", errno);
        goto done;
    }
    log_line("single-instance lock acquired: %s", LOCK_PATH);
    if (choose_user(&user) != 0 || !running())
        goto done;
    errno = 0;
    g_hid = open("/dev/hid", O_RDWR);
    int error = errno;
    log_line("open /dev/hid fd=%d errno=%d", g_hid, error);
    if (g_hid < 0 || !running())
        goto done;

    uint8_t descriptor[168] = {0};
    descriptor[0x00] = 3;
    memcpy(descriptor + 0x1C, &user, sizeof user);
    descriptor[0x20] = 1;
    struct hid_request request = { .command = 3, .data = descriptor, .out = &g_handle };
    log_line("add one pad: descriptor=168 protocol=3 class=1 user=0x%08x", (unsigned)user);
    int rc = checked_ioctl("AddDevice", HID_ADD_DEVICE, &request);
    /* Also clean up if a failed add nevertheless returned an output handle. */
    g_added = g_handle != -1;
    if (rc != 0 || !g_added) {
        log_line("add failed or returned no handle; stopping");
        goto done;
    }
    log_line("wait 3000ms after add, before first neutral sample");
    if (wait_until(now_ms() + 3000) != 0 ||
        state_for("initial neutral sample", 0, 128, 0, 0) != 0 ||
        dpad_burst("RIGHT", BUTTON_RIGHT) != 0 ||
        dpad_burst("LEFT", BUTTON_LEFT) != 0 ||
        state_for("left stick fully right", 0, 255, 0, 1000) != 0 ||
        state_for("left stick centre", 0, 128, 0, 0) != 0 ||
        state_for("L2 fully pressed", BUTTON_L2, 128, 255, 1000) != 0 ||
        state_for("L2 released", 0, 128, 0, 0) != 0 ||
        state_for("final neutral / wait before delete", 0, 128, 0, 2000) != 0)
        goto done;
    log_line("sequence complete; delete and close next");
    status = 0;
done:
    if (g_stop)
        log_line("stop requested signal=%d; cleaning up", (int)g_stop);
    if (status != 0)
        g_failed = 1;
    cleanup();
    return g_stop ? 128 + (int)g_stop : (g_failed ? 1 : 0);
}
