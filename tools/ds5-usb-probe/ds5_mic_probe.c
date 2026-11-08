// SPDX-License-Identifier: AGPL-3.0-or-later
// One-shot DualSense USB audio probe. Explicitly records 30 seconds on launch.
// Uses the same ugen/USB_FS API as ds5_usb_probe.c, with isochronous transfers.
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ds5_mic_format.h"

#define PROBE_DIR "/data/ps5-discord-probe"
#define WAV_PATH PROBE_DIR "/ds5mic.wav"
#define CAPTURE_SECONDS 30u
#define CAPTURE_MS (CAPTURE_SECONDS * 1000u)
#define PCM_LIMIT (48000u * 4u * CAPTURE_SECONDS)
#define WATCHDOG_MS (CAPTURE_MS + 15000u)
#define SLOTS 2u
#define FRAMES 16u
#define STRIDE 1024u

static FILE *g_log, *g_usb_log;
static volatile sig_atomic_t g_stop;
static struct usb_fs_endpoint g_eps[SLOTS];
static uint8_t g_buffers[SLOTS][FRAMES][STRIDE];
static void *g_pointers[SLOTS][FRAMES];
static uint32_t g_lengths[SLOTS][FRAMES];
static uint32_t g_frames[SLOTS], g_packet[SLOTS];

// Notification ABI also used by Ghostcontrol's payload/gc_main.c.
extern int32_t sceKernelSendNotificationRequest(int, void *, size_t, int);

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void log_line(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[ds5-mic] %s\n", line);
    FILE *logs[] = {g_log, g_usb_log};
    for (unsigned i = 0; i < 2; i++)
        if (logs[i]) {
            fprintf(logs[i], "%s\n", line);
            fflush(logs[i]);
        }
}

static void stop_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void notify(const char *message)
{
    struct { char reserved[45]; char message[3075]; } request = {0};
    snprintf(request.message, sizeof request.message, "%s", message);
    int rc = sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
    log_line("notification rc %d: %s", rc, message);
}

static int scan(void)
{
    for (int bus = 0; bus < 8 && !g_stop; bus++)
        for (int unit = 1; unit < 16 && !g_stop; unit++) {
            char path[32];
            snprintf(path, sizeof path, "/dev/ugen%d.%d", bus, unit);
            int fd = open(path, O_RDWR | O_NONBLOCK);
            if (fd < 0)
                continue;
            struct usb_device_descriptor d = {0};
            int rc = ioctl(fd, USB_GET_DEVICE_DESC, &d);
            unsigned vid = UGETW(d.idVendor), pid = UGETW(d.idProduct);
            log_line("%s: descriptor %d VID:PID %04x:%04x", path, rc, vid, pid);
            if (!rc && vid == 0x054c && (pid == 0x0ce6 || pid == 0x0df2)) {
                log_line("selected %s", path);
                return fd;
            }
            close(fd);
        }
    log_line("no USB DualSense found");
    return -1;
}

static int find_format(int fd, struct ds5_mic_format *format)
{
    int config = 0;
    if (ioctl(fd, USB_GET_CONFIG, &config) || config < 0 || config >= 255) {
        log_line("USB_GET_CONFIG: errno %d", errno);
        return -1;
    }
    uint8_t data[4096];
    struct usb_gen_descriptor desc = {.ugd_data = data, .ugd_maxlen = sizeof data,
                                      .ugd_config_index = (uint8_t)config};
    if (ioctl(fd, USB_GET_FULL_DESC, &desc)) {
        log_line("USB_GET_FULL_DESC: errno %d", errno);
        return -1;
    }
    // Validate before logging individual descriptors or modifying any interface.
    int rc = ds5_mic_find(data, desc.ugd_actlen, format);
    if (rc < 0) {
        log_line("malformed or truncated configuration (%u bytes)", desc.ugd_actlen);
        return -1;
    }
    for (size_t off = 9; off < desc.ugd_actlen; off += data[off]) {
        const uint8_t *d = data + off;
        if (d[1] == 4)
            log_line("interface %u alt %u class %u subclass %u protocol %u endpoints %u",
                     d[2], d[3], d[5], d[6], d[7], d[4]);
        else if (d[1] == 5 && d[0] >= 7)
            log_line("endpoint %02x attributes %02x max-packet %u interval %u",
                     d[2], d[3], ds5_le16(d + 4), d[6]);
    }
    if (!rc) {
        log_line("no supported UAC1 PCM S16_LE stereo 48000 Hz mic endpoint");
        return -1;
    }
    log_line("mic: interface %u (index %u), alt %u (index %u), endpoint %02x, packet %u, control index %u",
             format->interface_number, format->interface_index, format->alt_setting,
             format->alt_index, format->endpoint, format->packet_bytes, format->control_index);
    return 0;
}

static int start_transfer(int fd, unsigned slot)
{
    for (unsigned f = 0; f < g_frames[slot]; f++) {
        // USB_FS_COMPLETE copies each frame separately, at its requested offset.
        // Returned short lengths must be reset before every resubmission.
        g_pointers[slot][f] = g_buffers[slot][f];
        g_lengths[slot][f] = g_packet[slot];
    }
    g_eps[slot] = (struct usb_fs_endpoint){.ppBuffer = g_pointers[slot],
        .pLength = g_lengths[slot], .nFrames = g_frames[slot], .timeout = 250,
        .flags = USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK};
    struct usb_fs_start start = {.ep_index = (uint8_t)slot};
    if (ioctl(fd, USB_FS_START, &start)) {
        log_line("USB_FS_START slot %u: errno %d", slot, errno);
        return -1;
    }
    return 0;
}

static void levels(const uint8_t *pcm, size_t bytes, const char *label)
{
    double sum[2] = {0, 0};
    unsigned peak[2] = {0, 0};
    size_t frames = bytes / 4;
    for (size_t f = 0; f < frames; f++)
        for (unsigned ch = 0; ch < 2; ch++) {
            int value = ds5_le16(pcm + 4 * f + 2 * ch);
            if (value >= 32768)
                value -= 65536;
            unsigned magnitude = (unsigned)(value < 0 ? -value : value);
            sum[ch] += (double)value * value;
            if (magnitude > peak[ch])
                peak[ch] = magnitude;
        }
    log_line("%s: %zu PCM frames, L rms %.1f peak %u, R rms %.1f peak %u",
             label, frames, frames ? sqrt(sum[0] / frames) : 0, peak[0],
             frames ? sqrt(sum[1] / frames) : 0, peak[1]);
}

static int save_wav(const uint8_t *pcm, uint32_t bytes)
{
    uint8_t header[44];
    ds5_wav_header(header, bytes);
    FILE *out = fopen(WAV_PATH, "wb");
    if (!out) {
        log_line("open %s: errno %d", WAV_PATH, errno);
        return -1;
    }
    int failed = fwrite(header, 1, sizeof header, out) != sizeof header;
    if (!failed)
        failed = fwrite(pcm, 1, bytes, out) != bytes;
    if (fclose(out))
        failed = 1;
    log_line("WAV %s: %s (%u bytes, %.3f seconds of received PCM)", WAV_PATH,
             failed ? "write failed" : "saved", bytes, bytes / 192000.0);
    return failed ? -1 : 0;
}

static int capture(void)
{
    int result = 1, fd = -1, initialized = 0, detached = 0, alt_changed = 0;
    unsigned opened = 0, transfers = 0, empty = 0;
    uint32_t bytes = 0;
    uint8_t *pcm = NULL;
    struct ds5_mic_format format = {0};
    struct usb_alt_interface old_alt = {0};
    mkdir(PROBE_DIR, 0755);
    g_log = fopen(PROBE_DIR "/ds5mic.txt", "w");
    g_usb_log = fopen("/mnt/usb0/ds5-mic-probe.log", "a");
    log_line("start: explicit %u-second mic recording; USB audio only; no HID output commands",
             CAPTURE_SECONDS);
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    if (unlink(WAV_PATH) && errno != ENOENT) {
        log_line("cannot remove previous WAV: errno %d", errno);
        goto cleanup;
    }
    fd = scan();
    if (fd < 0 || find_format(fd, &format))
        goto cleanup;
    pcm = malloc(PCM_LIMIT);
    if (!pcm) {
        log_line("PCM allocation failed");
        goto cleanup;
    }
    old_alt.uai_interface_index = format.interface_index;
    if (ioctl(fd, USB_GET_ALTINTERFACE, &old_alt)) {
        log_line("get original alternate setting: errno %d", errno);
        goto cleanup;
    }
    // UAC drivers may own the entire audio collection at its control interface.
    // Detach that audio parent only when it actually has a driver attached.
    if (format.control_index != 255) {
        int parent = format.control_index;
        int active = ioctl(fd, USB_IFACE_DRIVER_ACTIVE, &parent);
        if (active && errno != ENXIO) {
            log_line("audio parent driver query: errno %d", errno);
            goto cleanup;
        }
        if (!active) {
            log_line("detaching audio collection at index %d (system USB audio pauses)", parent);
            if (ioctl(fd, USB_IFACE_DRIVER_DETACH, &parent)) {
                log_line("detach audio parent: errno %d", errno);
                goto cleanup;
            }
            detached = 1;
        }
    }
    int iface = format.interface_index;
    if (ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface)) {
        log_line("detach mic interface: errno %d", errno);
        goto cleanup;
    }
    detached = 1;
    struct usb_alt_interface alt = {.uai_interface_index = format.interface_index,
                                    .uai_alt_index = format.alt_index};
    if (ioctl(fd, USB_SET_ALTINTERFACE, &alt)) {
        log_line("select mic alternate setting: errno %d", errno);
        goto cleanup;
    }
    alt_changed = 1;
    struct usb_fs_init init = {.pEndpoints = g_eps, .ep_index_max = SLOTS};
    if (ioctl(fd, USB_FS_INIT, &init)) {
        log_line("USB_FS_INIT: errno %d", errno);
        goto cleanup;
    }
    initialized = 1;
    for (unsigned slot = 0; slot < SLOTS; slot++) {
        struct usb_fs_open op = {.ep_index = (uint8_t)slot, .ep_no = format.endpoint,
            .max_frames = FRAMES, .max_bufsize = FRAMES * STRIDE};
        if (ioctl(fd, USB_FS_OPEN, &op)) {
            log_line("USB_FS_OPEN slot %u: errno %d", slot, errno);
            goto cleanup;
        }
        opened++;
        log_line("slot %u opened: frames %u packet %u buffer %u", slot,
                 (unsigned)op.max_frames, op.max_packet_length, (unsigned)op.max_bufsize);
        if (!op.max_frames || op.max_frames > FRAMES || !op.max_packet_length ||
            op.max_packet_length > STRIDE || op.max_bufsize < op.max_frames * op.max_packet_length) {
            log_line("unsupported kernel transfer geometry");
            goto cleanup;
        }
        g_frames[slot] = op.max_frames;
        g_packet[slot] = op.max_packet_length;
    }
    if (g_stop)
        goto cleanup;
    log_line("recording now; speak/tap near controller mic; physical mute state is unchanged");
    notify("DualSense mic capture started - recording for 30 seconds. Speak now.");
    for (unsigned slot = 0; slot < SLOTS; slot++)
        if (start_transfer(fd, slot))
            goto cleanup;
    uint64_t start = now_ms(), last_completion = start, last_log = start;
    uint32_t last_bytes = 0;
    while (!g_stop && now_ms() - start < CAPTURE_MS && bytes < PCM_LIMIT) {
        struct usb_fs_complete complete = {0};
        if (ioctl(fd, USB_FS_COMPLETE, &complete)) {
            if (errno == EINTR)
                continue;
            if (errno != EBUSY || now_ms() - last_completion > 1000) {
                log_line("completion failed/stalled: errno %d", errno);
                goto cleanup;
            }
            usleep(1000);
            continue;
        }
        unsigned slot = complete.ep_index;
        if (slot >= SLOTS || g_eps[slot].status || g_eps[slot].aFrames > g_frames[slot]) {
            log_line("invalid completion: slot %u status %d", slot,
                     slot < SLOTS ? g_eps[slot].status : -1);
            goto cleanup;
        }
        last_completion = now_ms();
        transfers++;
        for (unsigned f = 0; f < g_eps[slot].aFrames; f++) {
            uint32_t n = g_lengths[slot][f];
            if (n > g_packet[slot] || n % 4) {
                log_line("invalid PCM frame length %u", n);
                goto cleanup;
            }
            empty += n == 0;
            if (n > PCM_LIMIT - bytes)
                n = PCM_LIMIT - bytes;
            memcpy(pcm + bytes, g_buffers[slot][f], n);
            bytes += n;
        }
        if (start_transfer(fd, slot))
            goto cleanup;
        if (now_ms() - last_log >= 1000) {
            char label[96];
            snprintf(label, sizeof label, "t=%us transfers %u empty USB frames %u",
                     (unsigned)((now_ms() - start) / 1000), transfers, empty);
            levels(pcm + last_bytes, bytes - last_bytes, label);
            last_log = now_ms();
            last_bytes = bytes;
        }
    }
    if (!g_stop && bytes) {
        result = 0;
        levels(pcm, bytes, "total");
        log_line("capture finished: wall %llu ms, transfers %u, empty frames %u",
                 (unsigned long long)(now_ms() - start), transfers, empty);
        if (bytes < PCM_LIMIT * 95u / 100u)
            log_line("short recording: missing USB audio is omitted, WAV duration is received PCM only");
    } else {
        log_line("capture interrupted or no PCM received");
    }

cleanup:
    for (unsigned slot = 0; slot < opened; slot++) {
        struct usb_fs_stop stop = {.ep_index = (uint8_t)slot};
        struct usb_fs_close close_ep = {.ep_index = (uint8_t)slot};
        if (ioctl(fd, USB_FS_STOP, &stop))
            log_line("stop slot %u: errno %d", slot, errno);
        if (ioctl(fd, USB_FS_CLOSE, &close_ep)) {
            log_line("close slot %u: errno %d", slot, errno);
            result = 1;
        }
    }
    if (initialized) {
        struct usb_fs_uninit uninit = {0};
        if (ioctl(fd, USB_FS_UNINIT, &uninit)) {
            log_line("USB_FS_UNINIT: errno %d", errno);
            result = 1;
        }
    }
    if (alt_changed && ioctl(fd, USB_SET_ALTINTERFACE, &old_alt)) {
        log_line("restore alternate setting: errno %d", errno);
        result = 1;
    }
    if (detached) {
        int zero = 0;
        int rc = ioctl(fd, USB_DEVICEENUMERATE, &zero);
        log_line("re-enumerate: rc %d errno %d; press PS/replug if controller stays disconnected",
                 rc, rc ? errno : 0);
        if (rc)
            result = 1;
    }
    if (fd >= 0)
        close(fd);
    if (bytes && save_wav(pcm, bytes))
        result = 1;
    free(pcm);
    log_line("done: result %d, captured %u bytes%s", result, bytes,
             bytes && result ? " (partial recording)" : "");
    notify(result ? "DualSense mic capture stopped with an error. Check ds5mic.txt."
                  : "DualSense mic capture finished - recording saved. Press PS to reconnect if needed.");
    if (g_log)
        fclose(g_log);
    if (g_usb_log)
        fclose(g_usb_log);
    return result;
}

int main(void)
{
    // Allow the requested 30-second recording plus setup/cleanup headroom.
    // TERM at 43 s and KILL at 45 s bound a stalled USB ioctl.
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "[ds5-mic] watchdog fork failed: errno %d; not recording\n", errno);
        return 1;
    }
    if (!child)
        _exit(capture());
    uint64_t start = now_ms();
    int status = 0, terminated = 0;
    for (;;) {
        pid_t rc = waitpid(child, &status, WNOHANG);
        if (rc == child)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        if (rc < 0 && errno != EINTR)
            return 1;
        uint64_t elapsed = now_ms() - start;
        if (!terminated && elapsed >= WATCHDOG_MS - 2000u) {
            kill(child, SIGTERM);
            terminated = 1;
        }
        if (elapsed >= WATCHDOG_MS) {
            fprintf(stderr, "[ds5-mic] watchdog: killing stalled worker; reconnect controller\n");
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            notify("DualSense mic capture timed out. Reconnect the controller.");
            return 1;
        }
        usleep(10000);
    }
}
