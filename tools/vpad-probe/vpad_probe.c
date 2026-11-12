// SPDX-License-Identifier: AGPL-3.0-or-later
/* /dev/hid virtual-pad probes and the supervised controller-audio worker.
 * Independently written from the documented ABI in FGGstore/FGG-XSense
 * (GPL-3.0), src/vpad.c:13-75,96-106,119-140,187-204 and src/vpad.h:41.
 * Credits also to sinfiltros/AnyPad-PS5-English (GPL-3.0-or-later),
 * src/ps5_vpad.c:253-319,534-613 and PRUEBAS.md:360-395 (user routing),
 * and Ghostpad (GPL-3.0-or-later), virtualDS5research.md:282-339
 * (button/trigger observations). Pinned upstream references: README.md.
 * No libScePad, Remote Play, or process injection. Only the opt-in USB build
 * takes over the physical controller through the existing DS5 USB runtime.
 */
#ifndef PROBE_USB_FORWARD
#define PROBE_USB_FORWARD 0
#endif
#ifndef PROBE_USB_TONE
#define PROBE_USB_TONE 0
#endif
#ifndef PROBE_USB_CALL
#define PROBE_USB_CALL 0
#endif
#ifndef PROBE_USB_SERVICE
#define PROBE_USB_SERVICE 0
#endif
#ifndef PROBE_RESUME_GAME
#define PROBE_RESUME_GAME 0
#endif
#if PROBE_USB_SERVICE && !PROBE_USB_CALL
#error Controller audio service requires authenticated core PCM
#endif
#if PROBE_USB_CALL && PROBE_USB_TONE
#error Call PCM and test tones are separate probe builds
#endif
#include <ctype.h>
#include <dlfcn.h>
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
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#if PROBE_USB_SERVICE
#include <sys/mman.h>
#endif
#if PROBE_USB_FORWARD
#include <pthread.h>
#include "usb_forward_report.h"
#endif

#define PROBE_DIR "/data/ps5-discord"
#if PROBE_USB_FORWARD
#if PROBE_USB_CALL
#if PROBE_USB_SERVICE
#define LOG_PATH PROBE_DIR "/controller-audio.log"
#define USB_LOG_PATH "/mnt/usb0/controller-audio.log"
#elif defined(DS5_USB_RESTORE_CONFIG)
#define LOG_PATH PROBE_DIR "/vpad-usb-call-restore-probe.log"
#define USB_LOG_PATH "/mnt/usb0/vpad-usb-call-restore-probe.log"
#else
#define LOG_PATH PROBE_DIR "/vpad-usb-call-probe.log"
#define USB_LOG_PATH "/mnt/usb0/vpad-usb-call-probe.log"
#endif
#elif PROBE_USB_TONE
#define LOG_PATH PROBE_DIR "/vpad-usb-audio-probe.log"
#define USB_LOG_PATH "/mnt/usb0/vpad-usb-audio-probe.log"
#elif defined(DS5_USB_RESTORE_CONFIG)
#define LOG_PATH PROBE_DIR "/vpad-usb-restore-probe.log"
#define USB_LOG_PATH "/mnt/usb0/vpad-usb-restore-probe.log"
#else
#define LOG_PATH PROBE_DIR "/vpad-usb-probe.log"
#define USB_LOG_PATH "/mnt/usb0/vpad-usb-probe.log"
#endif
#elif PROBE_RESUME_GAME
#define LOG_PATH PROBE_DIR "/vpad-game-probe.log"
#define USB_LOG_PATH "/mnt/usb0/vpad-game-probe.log"
#else
#define LOG_PATH PROBE_DIR "/vpad-probe.log"
#define USB_LOG_PATH "/mnt/usb0/vpad-probe.log"
#endif
#define USER_PATH PROBE_DIR "/vpad-probe-user"
#define LOCK_PATH PROBE_DIR "/vpad-probe.lock"
#define RUN_MS 23000u
#define FRAME_MS 16u

#define HID_ADD_DEVICE  0xC018482AUL
#define HID_INSERT_DATA 0x8018482CUL
#define HID_DEL_DEVICE  0x80104850UL
#define BUTTON_RIGHT 0x000020u
#define BUTTON_LEFT  0x000080u
#define BUTTON_L2    0x000100u
#define BUTTON_CROSS 0x004000u /* FGG-XSense src/vpad.c:54. */

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

/* Optional SDK helpers (ps5/kernel.h); provided by this SDK's crt1.o.
 * Weak references let an SDK without them skip the credential retry. */
uint64_t kernel_get_ucred_authid(pid_t pid) __attribute__((weak));
int32_t kernel_set_ucred_authid(pid_t pid, uint64_t authid) __attribute__((weak));
int32_t kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]) __attribute__((weak));
int32_t kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]) __attribute__((weak));

typedef int32_t (*mbus_bind_fn)(uint64_t device_id, int32_t user);

static FILE *g_log, *g_usb_log;
static int g_hid = -1, g_lock = -1, g_added, g_failed, g_cleaned;
static int32_t g_handle = -1;
static int g_bound;
static uint64_t g_start;
static volatile sig_atomic_t g_stop;
static int g_quiet; /* InsertData every 16 ms: log the first per step and failures only */
#if PROBE_USB_SERVICE
static _Atomic uint64_t *g_heartbeat;

static uint64_t guard_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
#endif
#if PROBE_USB_FORWARD
static void usb_forward_stop(void);
#endif
static struct {
    int pending;
    uint64_t authid;
    uint8_t caps[16];
} g_credentials;

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
    if (g_usb_log && (fputs(line, g_usb_log) == EOF || fflush(g_usb_log) != 0)) {
        int error = errno;
        g_failed = 1;
        klog_printf("[vpad-probe pid=%d t=%llums] USB log write failed errno=%d\n",
                    (int)getpid(), (unsigned long long)(now_ms() - g_start), error);
    }
    errno = saved;
}

static int restore_credentials(void)
{
    if (!g_credentials.pending)
        return 0;
    pid_t me = getpid();
    /* Attempt both restorations even if either setter fails. */
    int caps_rc = kernel_set_ucred_caps(me, g_credentials.caps);
    log_line("restore kernel_set_ucred_caps rc=0x%08x", (unsigned)caps_rc);
    int auth_rc = kernel_set_ucred_authid(me, g_credentials.authid);
    log_line("restore kernel_set_ucred_authid rc=0x%08x", (unsigned)auth_rc);
    uint64_t authid = kernel_get_ucred_authid(me);
    log_line("restore kernel_get_ucred_authid value=0x%016llx",
             (unsigned long long)authid);
    uint8_t caps[16];
    int read_rc = kernel_get_ucred_caps(me, caps);
    log_line("restore kernel_get_ucred_caps rc=0x%08x", (unsigned)read_rc);
    int verified = authid == g_credentials.authid && read_rc == 0 &&
                   memcmp(caps, g_credentials.caps, sizeof caps) == 0;
    log_line("restore credentials verified=%d", verified);
    if (caps_rc != 0 || auth_rc != 0 || !verified) {
        log_line("credential restoration failed; saved credentials remain pending for cleanup");
        g_failed = 1;
        return -1;
    }
    g_credentials.pending = 0;
    return 0;
}

static mbus_bind_fn prepare_mbus(void)
{
    const char *path = "/system/common/lib/libSceMbus.sprx";
    (void)dlerror();
    void *module = dlopen(path, RTLD_LAZY);
    const char *error = dlerror();
    log_line("dlopen %s handle=%p error=%s", path, module, error ? error : "none");
    if (!module) {
        log_line("MBus unavailable; skipping init and bind, continuing HID sequence");
        return NULL;
    }
    /* Use the payload SDK's resolver, as in its hello_dlfcn sample. Keep the
     * module open through process exit so the returned function stays valid. */
    (void)dlerror();
    void *init_address = dlsym(module, "sceMbusInit");
    error = dlerror();
    int init_ok = !error && init_address;
    log_line("dlsym sceMbusInit address=%p error=%s",
             init_address, error ? error : "none");
    (void)dlerror();
    void *bind_address = dlsym(module, "sceMbusBindDeviceWithUserId");
    error = dlerror();
    int bind_ok = !error && bind_address;
    log_line("dlsym sceMbusBindDeviceWithUserId address=%p error=%s",
             bind_address, error ? error : "none");
    if (init_ok) {
        int32_t (*init)(void) = (int32_t (*)(void))init_address;
        int32_t rc = init();
        log_line("sceMbusInit rc=0x%08x", (unsigned)rc);
    } else {
        log_line("sceMbusInit not present; skipping init");
    }
    if (!bind_ok) {
        log_line("MBus bind symbol unavailable; continuing HID sequence");
        return NULL;
    }
    return (mbus_bind_fn)bind_address;
}

static void bind_virtual_pad(mbus_bind_fn bind, uint32_t user)
{
    /* FW 13.00 syslog confirmed deviceId=0x13030d, ioctl handle=0x0013030d:
     * the MBus device ID equals the zero-extended ioctl handle, not uniqueId. */
    uint64_t device_id = (uint64_t)(uint32_t)g_handle;
    log_line("bind target: ioctl_handle=0x%08x MBus_deviceId=0x%016llx user=0x%08x",
             (unsigned)g_handle, (unsigned long long)device_id, (unsigned)user);
    if (!bind) {
        log_line("sceMbusBindDeviceWithUserId unavailable; skipping bind");
        return;
    }
    int32_t rc = bind(device_id, (int32_t)user);
    log_line("sceMbusBindDeviceWithUserId attempt=1 rc=0x%08x", (unsigned)rc);
    if (rc == 0) {
        g_bound = 1;
        return;
    }
    if (!kernel_get_ucred_authid || !kernel_set_ucred_authid ||
        !kernel_get_ucred_caps || !kernel_set_ucred_caps) {
        log_line("bind retry skipped: SDK credential helpers unavailable");
        return;
    }
    pid_t me = getpid();
    g_credentials.authid = kernel_get_ucred_authid(me);
    log_line("save kernel_get_ucred_authid value=0x%016llx",
             (unsigned long long)g_credentials.authid);
    int caps_rc = kernel_get_ucred_caps(me, g_credentials.caps);
    log_line("save kernel_get_ucred_caps rc=0x%08x", (unsigned)caps_rc);
    if (!g_credentials.authid || caps_rc != 0) {
        log_line("bind retry skipped: original credentials could not be saved");
        return;
    }
    uint8_t full_caps[16];
    memset(full_caps, 0xFF, sizeof full_caps); /* AnyPad's sixteen capability bytes. */
    const uint64_t authid = 0x3800000000010003ull;
    /* Journal before either write: even a failed setter may partially apply. */
    g_credentials.pending = 1;
    int auth_rc = kernel_set_ucred_authid(me, authid);
    log_line("elevate kernel_set_ucred_authid value=0x%016llx rc=0x%08x",
             (unsigned long long)authid, (unsigned)auth_rc);
    caps_rc = kernel_set_ucred_caps(me, full_caps);
    log_line("elevate kernel_set_ucred_caps bytes=16*FF rc=0x%08x", (unsigned)caps_rc);
    uint64_t current_authid = kernel_get_ucred_authid(me);
    log_line("elevate kernel_get_ucred_authid value=0x%016llx",
             (unsigned long long)current_authid);
    uint8_t current_caps[16];
    int read_rc = kernel_get_ucred_caps(me, current_caps);
    log_line("elevate kernel_get_ucred_caps rc=0x%08x", (unsigned)read_rc);
    int verified = current_authid == authid && read_rc == 0 &&
                   memcmp(current_caps, full_caps, sizeof full_caps) == 0;
    log_line("elevate credentials verified=%d", verified);
    if (auth_rc == 0 && caps_rc == 0 && verified) {
        rc = bind(device_id, (int32_t)user);
        log_line("sceMbusBindDeviceWithUserId attempt=2 elevated=1 rc=0x%08x", (unsigned)rc);
        g_bound = rc == 0;
    } else {
        log_line("bind retry skipped: credential elevation failed");
    }
    /* No stop/deadline/logging-error branch may bypass restoration. */
    (void)restore_credentials();
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
    (void)restore_credentials();
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
#if PROBE_USB_FORWARD
    usb_forward_stop();
#endif
    close_logged(&g_hid, "/dev/hid");
    /* Keep the inode: unlinking a flock file permits competing lock owners. */
    close_logged(&g_lock, LOCK_PATH);
    log_line("finish failed=%d signal=%d pad_remaining=%d", g_failed, (int)g_stop, g_added);
    if (g_usb_log) {
        FILE *file = g_usb_log;
        g_usb_log = NULL;
        if (fclose(file) != 0) {
            g_failed = 1;
            log_line("close USB log failed errno=%d", errno);
        }
    }
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
#if PROBE_USB_SERVICE
    uint64_t heartbeat = now_ms();
    if (g_failed)
        return 0;
    atomic_store(g_heartbeat, heartbeat);
#else
    if (now_ms() - g_start >= RUN_MS) {
        log_line("%u ms deadline reached (or monotonic clock failed)", RUN_MS);
        g_failed = 1;
        return 0;
    }
#endif
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

#if PROBE_USB_FORWARD
#include "usb_forward.inc"
#endif

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

#if !PROBE_USB_FORWARD
/* This is the kernel's 0xA0-byte input buffer, NOT a ScePadData struct. */
static int state_for(const char *step, uint32_t buttons, uint8_t lx, uint8_t l2, unsigned ms)
{
    uint8_t report[0xA0] = {0};
    float identity = 1.0f;
    if (buttons & ~(BUTTON_RIGHT | BUTTON_LEFT | BUTTON_L2 |
                    (PROBE_RESUME_GAME ? BUTTON_CROSS : 0u))) {
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

#endif

int main(void)
{
#if PROBE_USB_SERVICE
    /* Shared heartbeat lets the guard bound stalled calls, not call duration. */
    g_heartbeat = mmap(NULL, sizeof *g_heartbeat, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANON, -1, 0);
    if (g_heartbeat == MAP_FAILED)
        return 1;
    atomic_init(g_heartbeat, guard_ms());
#endif
    /* Start before service/file I/O or credential changes. Leave this passive
     * guard alive through SDK shutdown; it never inherits probe resources. */
    const pid_t parent = getpid();
    const pid_t watchdog = fork();
    if (watchdog < 0)
        return 1;
    if (watchdog == 0) {
#if PROBE_USB_SERVICE
        int terminated = 0;
        while (getppid() == parent) {
            uint64_t now = guard_ms();
            uint64_t last = atomic_load(g_heartbeat);
            uint64_t idle = now >= last ? now - last : 0;
            if (!terminated && (!now || idle >= 23000u)) {
                kill(parent, SIGTERM);
                terminated = 1;
            }
            if (!now || idle >= 25000u) {
                kill(parent, SIGKILL);
                break;
            }
            sleep(1);
        }
        _exit(0);
#else
        unsigned remaining = 23;
        while (remaining)
            remaining = sleep(remaining);
        if (getppid() != parent)
            _exit(0);
        kill(parent, SIGTERM);
        remaining = 2;
        while (remaining)
            remaining = sleep(remaining);
        if (getppid() == parent)
            kill(parent, SIGKILL);
        _exit(0);
#endif
    }

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
    errno = 0;
    g_usb_log = fopen(USB_LOG_PATH, "a");
    log_line("open optional USB append log %s present=%d errno=%d",
             USB_LOG_PATH, g_usb_log != NULL, errno);
    log_line("start: direct /dev/hid; watchdog=%ums; service=%d; no Remote Play; USB takeover=%d",
             RUN_MS, PROBE_USB_SERVICE, PROBE_USB_FORWARD);
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

    mbus_bind_fn bind = prepare_mbus();
    if (!running())
        goto done;
#if PROBE_USB_FORWARD
    if (!bind || usb_forward_start() != 0)
        goto done;
#endif
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
    bind_virtual_pad(bind, user);
#if PROBE_USB_FORWARD
    if (usb_forward_run() != 0)
        goto done;
#else
#if PROBE_RESUME_GAME
    if (!g_bound || !running()) {
        log_line("game resume refused: successful binding and healthy run required");
        goto done;
    }
#endif
    log_line("wait 3000ms after add, before first neutral sample");
    if (wait_until(now_ms() + 3000) != 0 ||
        state_for("initial neutral sample", 0, 128, 0, 0) != 0)
        goto done;
#if PROBE_RESUME_GAME
    /* Opt-in build: user requested selecting the highlighted running game.
     * One Cross pulse only; release and allow the title to regain focus. */
    if (state_for("select highlighted game (Cross once)", BUTTON_CROSS, 128, 0, 150) != 0 ||
        state_for("release Cross / wait for game focus", 0, 128, 0, 2000) != 0)
        goto done;
#endif
    if (dpad_burst("RIGHT", BUTTON_RIGHT) != 0 ||
        dpad_burst("LEFT", BUTTON_LEFT) != 0 ||
        state_for("left stick fully right", 0, 255, 0, 1000) != 0 ||
        state_for("left stick centre", 0, 128, 0, 0) != 0 ||
        state_for("L2 fully pressed", BUTTON_L2, 128, 255, 1000) != 0 ||
        state_for("L2 released", 0, 128, 0, 0) != 0 ||
        state_for("final neutral / wait before delete", 0, 128, 0, 2000) != 0)
        goto done;
#endif /* scripted builds */
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
