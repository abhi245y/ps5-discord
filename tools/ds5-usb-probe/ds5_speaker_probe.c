// SPDX-License-Identifier: AGPL-3.0-or-later
// USB speaker tests: eight-second tones or a bounded PCM file. Haptics stay zero.
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ds5_mic_format.h"
#include "ds5_speaker_tone.h"

#define FRAMES 16u
#define SAMPLES_PER_PACKET 48u
#define PACKET_BYTES (SAMPLES_PER_PACKET * 4u * 2u)
#define LOG_PATH "/data/ps5-discord-probe/ds5speaker.txt"
#define SONG_PATH "/data/ps5-discord-probe/song-mono.raw"

extern int32_t sceKernelSendNotificationRequest(int, void *, size_t, int);
static FILE *g_log, *g_usb_log;
static volatile sig_atomic_t g_stop;
static struct usb_fs_endpoint g_eps[3];
static uint8_t g_audio[2][FRAMES][PACKET_BYTES], g_report[63];
static void *g_pointers[3][FRAMES];
static uint32_t g_lengths[3][FRAMES];
static uint32_t g_queued;
static uint32_t g_total_samples = 48000u * 8u;
static uint32_t g_watchdog_ms = 25000u, g_playback_limit_ms = 12000u;
#ifdef DS5_PLAY_SONG
static FILE *g_source;
#endif

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static void log_line(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[ds5-speaker] %s\n", line);
    FILE *logs[] = {g_log, g_usb_log};
    for (unsigned i = 0; i < 2; i++)
        if (logs[i]) {
            fprintf(logs[i], "%s\n", line);
            fflush(logs[i]);
        }
}

static void notify(const char *message)
{
    struct { char reserved[45]; char message[3075]; } request = {0};
    snprintf(request.message, sizeof request.message, "%s", message);
    int rc = sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
    log_line("notification rc %d: %s", rc, message);
}

static void stop_signal(int sig)
{
    (void)sig;
    g_stop = 1;
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
            if (!ioctl(fd, USB_GET_DEVICE_DESC, &d) && UGETW(d.idVendor) == 0x054c &&
                (UGETW(d.idProduct) == 0x0ce6 || UGETW(d.idProduct) == 0x0df2)) {
                log_line("selected %s VID:PID %04x:%04x", path, UGETW(d.idVendor), UGETW(d.idProduct));
                return fd;
            }
            close(fd);
        }
    log_line("no USB DualSense found");
    return -1;
}

static int discover(int fd, struct ds5_mic_format *format, int *hid_index, uint8_t *hid_endpoint)
{
    int config = 0;
    // PS5 rejects USB_GET_DEVICEINFO with ENOTTY. This probe targets the same
    // DualSense USB layout whose 1 ms packet cadence was verified by mic capture.
    if (ioctl(fd, USB_GET_CONFIG, &config) || config < 0 || config >= 255) {
        log_line("USB_GET_CONFIG failed, errno %d", errno);
        return -1;
    }
    uint8_t data[4096];
    struct usb_gen_descriptor desc = {.ugd_data = data, .ugd_maxlen = sizeof data,
                                      .ugd_config_index = (uint8_t)config};
    if (ioctl(fd, USB_GET_FULL_DESC, &desc) ||
        ds5_audio_find(data, desc.ugd_actlen, format, 4, 0) != 1) {
        log_line("no supported UAC1 PCM S16_LE 4-channel 48000 Hz output, errno %d", errno);
        return -1;
    }
    int index = -1, previous = -1, hid = 0, interval = 0;
    *hid_index = -1;
    for (size_t off = 9; off < desc.ugd_actlen; off += data[off]) {
        const uint8_t *d = data + off;
        if (d[1] == 4) {
            if (d[2] != previous) {
                index++;
                previous = d[2];
            }
            hid = d[5] == 3 && d[3] == 0;
        } else if (d[1] == 5 && d[0] >= 7) {
            if (index == format->interface_index && d[2] == format->endpoint)
                interval = d[6];
            if (hid && !(d[2] & 0x80) && (d[3] & 3) == 3 && ds5_le16(d + 4) >= sizeof g_report) {
                *hid_index = index;
                *hid_endpoint = d[2];
            }
        }
    }
    if (interval != 4 || *hid_index < 0) {
        log_line("unsupported output interval %d or missing HID OUT endpoint", interval);
        return -1;
    }
    log_line("audio: interface %u index %u alt %u index %u endpoint %02x packet %u interval %d",
             format->interface_number, format->interface_index, format->alt_setting,
             format->alt_index, format->endpoint, format->packet_bytes, interval);
    log_line("HID: index %d endpoint %02x; audio control index %u",
             *hid_index, *hid_endpoint, format->control_index);
    return 0;
}

static int route_speaker(int fd)
{
    // Selective USB report 0x02 fields documented by dualsensectl. Leave trigger,
    // vibration, LEDs, microphone volume and power-save/mute flags untouched.
    // Output path 3 routes the right PCM channel to the built-in mono speaker.
    memset(g_report, 0, sizeof g_report);
    g_report[0] = 0x02;
    g_report[1] = 0xa0; // Speaker-volume and audio-routing fields are valid.
    g_report[2] = 0x80; // Audio-control2 (speaker pre-gain) is valid.
#ifdef DS5_PLAY_SONG
    g_report[6] = 0x64; // Full speaker volume, matching the requested max-volume test.
#else
    g_report[6] = 0x50; // Moderate speaker volume for generated beeps.
#endif
    g_report[8] = 0x30; // Built-in speaker only; headphone output disabled.
    g_report[38] = 0x02; // Speaker pre-gain 2; other audio-control2 bits zero.
    g_pointers[2][0] = g_report;
    g_lengths[2][0] = sizeof g_report;
    g_eps[2] = (struct usb_fs_endpoint){.ppBuffer = g_pointers[2], .pLength = g_lengths[2],
                                       .nFrames = 1, .timeout = 250};
    struct usb_fs_start start = {.ep_index = 2};
    if (ioctl(fd, USB_FS_START, &start))
        return -1;
    uint64_t deadline = now_ms() + 1000;
    while (!g_stop && now_ms() < deadline) {
        struct usb_fs_complete complete = {0};
        if (!ioctl(fd, USB_FS_COMPLETE, &complete)) {
            if (complete.ep_index != 2 || g_eps[2].status || g_eps[2].aFrames != 1 ||
                g_lengths[2][0] != sizeof g_report)
                return -1;
            log_line("speaker routing accepted: HID report 0x02, flags a0/80, volume %02x, route 30, pre-gain 02",
                     g_report[6]);
            return 0;
        }
        if (errno != EBUSY && errno != EINTR)
            return -1;
        usleep(1000);
    }
    return -1;
}

static int queue_audio(int fd, unsigned slot)
{
    for (unsigned f = 0; f < FRAMES; f++) {
#ifdef DS5_PLAY_SONG
        uint8_t mono[SAMPLES_PER_PACKET * 2u] = {0};
        size_t count = fread(mono, 1, sizeof mono, g_source);
        if (ferror(g_source) || count % 2) {
            log_line("song PCM read failed");
            return -1;
        }
        ds5_speaker_mono_packet(g_audio[slot][f], mono);
#else
        ds5_speaker_packet(g_audio[slot][f], g_queued);
#endif
        g_queued += SAMPLES_PER_PACKET;
        g_pointers[slot][f] = g_audio[slot][f];
        g_lengths[slot][f] = PACKET_BYTES;
    }
    g_eps[slot] = (struct usb_fs_endpoint){.ppBuffer = g_pointers[slot], .pLength = g_lengths[slot],
                                          .nFrames = FRAMES, .timeout = 250};
    struct usb_fs_start start = {.ep_index = (uint8_t)slot};
    return ioctl(fd, USB_FS_START, &start);
}

static int playback(void)
{
    int result = 1, fd = -1, initialized = 0, detached = 0, changed = 0, hid = -1;
    unsigned opened = 0, pending = 0, transfers = 0;
    uint8_t hid_endpoint = 0;
    struct ds5_mic_format format = {0};
    struct usb_alt_interface old_alt = {0};
    mkdir("/data/ps5-discord-probe", 0755);
    g_log = fopen(LOG_PATH, "w");
    g_usb_log = fopen("/mnt/usb0/ds5-speaker-probe.log", "a");
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
#ifdef DS5_PLAY_SONG
    log_line("start: song playback from %s; %u PCM frames, %.3f seconds; haptic channels zero",
             SONG_PATH, g_total_samples, g_total_samples / 48000.0);
#else
    log_line("start: 8-second speaker playback; four beeps, amplitude %d; haptic PCM channels zero",
             DS5_SPEAKER_AMPLITUDE);
#endif
    fd = scan();
    if (fd < 0 || discover(fd, &format, &hid, &hid_endpoint))
        goto cleanup;
    old_alt.uai_interface_index = format.interface_index;
    if (ioctl(fd, USB_GET_ALTINTERFACE, &old_alt))
        goto cleanup;
    if (format.control_index != 255) {
        int parent = format.control_index;
        int active = ioctl(fd, USB_IFACE_DRIVER_ACTIVE, &parent);
        if (active && errno != ENXIO)
            goto cleanup;
        if (!active) {
            if (ioctl(fd, USB_IFACE_DRIVER_DETACH, &parent))
                goto cleanup;
            detached = 1;
        }
    }
    int iface = format.interface_index;
    if (ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface))
        goto cleanup;
    detached = 1;
    // Own HID temporarily so system output reports cannot overwrite speaker routing.
    if (ioctl(fd, USB_IFACE_DRIVER_DETACH, &hid))
        goto cleanup;
    log_line("audio and HID detached; controller input pauses until handback");
    struct usb_alt_interface alt = {.uai_interface_index = format.interface_index,
                                    .uai_alt_index = format.alt_index};
    if (ioctl(fd, USB_SET_ALTINTERFACE, &alt))
        goto cleanup;
    changed = 1;
    struct usb_fs_init init = {.pEndpoints = g_eps, .ep_index_max = 3};
    if (ioctl(fd, USB_FS_INIT, &init))
        goto cleanup;
    initialized = 1;
    for (unsigned slot = 0; slot < 3; slot++) {
        struct usb_fs_open op = {.ep_index = (uint8_t)slot,
            .ep_no = slot == 2 ? hid_endpoint : format.endpoint,
            .max_frames = slot == 2 ? 1 : FRAMES,
            .max_bufsize = slot == 2 ? sizeof g_report : FRAMES * PACKET_BYTES};
        if (ioctl(fd, USB_FS_OPEN, &op)) {
            log_line("USB_FS_OPEN slot %u errno %d", slot, errno);
            goto cleanup;
        }
        opened++;
        log_line("slot %u opened: packet %u frames %u buffer %u", slot,
                 op.max_packet_length, (unsigned)op.max_frames, (unsigned)op.max_bufsize);
        if (slot < 2 && (op.max_frames < FRAMES || op.max_packet_length < PACKET_BYTES ||
                         op.max_bufsize < FRAMES * PACKET_BYTES))
            goto cleanup;
    }
    if (route_speaker(fd)) {
        log_line("speaker routing failed, errno %d", errno);
        goto cleanup;
    }
#ifdef DS5_PLAY_SONG
    notify("DualSense music started - Auld Lang Syne, sung by Frank C. Stanley (1910).");
#else
    notify("DualSense speaker test started - listen for four louder beeps.");
#endif
    uint64_t start = now_ms(), last = start, last_log = start;
    for (unsigned slot = 0; slot < 2; slot++) {
        if (queue_audio(fd, slot))
            goto cleanup;
        pending++;
    }
    while (pending && !g_stop && now_ms() - start < g_playback_limit_ms) {
        struct usb_fs_complete complete = {0};
        if (ioctl(fd, USB_FS_COMPLETE, &complete)) {
            if ((errno != EBUSY && errno != EINTR) || now_ms() - last > 1000) {
                log_line("audio completion failed/stalled, errno %d", errno);
                goto cleanup;
            }
            usleep(1000);
            continue;
        }
        unsigned slot = complete.ep_index;
        if (slot >= 2 || g_eps[slot].status || g_eps[slot].aFrames != FRAMES) {
            log_line("bad completion slot %u status %d", slot, slot < 3 ? g_eps[slot].status : -1);
            goto cleanup;
        }
        for (unsigned f = 0; f < FRAMES; f++)
            if (g_lengths[slot][f] != PACKET_BYTES) {
                log_line("short output frame %u", g_lengths[slot][f]);
                goto cleanup;
            }
        pending--;
        transfers++;
        last = now_ms();
        if (g_queued < g_total_samples) {
            if (queue_audio(fd, slot))
                goto cleanup;
            pending++;
        }
        if (last - last_log >= 1000) {
            log_line("t=%us completed %u queued %u PCM frames", (unsigned)((last - start) / 1000), transfers, g_queued);
            last_log = last;
        }
    }
    if (!pending && !g_stop && g_queued == g_total_samples)
        result = 0;
    log_line("playback ended: %u transfers, %u PCM frames, wall %llu ms", transfers, g_queued,
             (unsigned long long)(now_ms() - start));

cleanup:
    if (result)
        log_line("cleanup after failure/interruption, errno %d", errno);
    for (unsigned slot = 0; slot < opened; slot++) {
        struct usb_fs_stop stop = {.ep_index = (uint8_t)slot};
        struct usb_fs_close close_ep = {.ep_index = (uint8_t)slot};
        ioctl(fd, USB_FS_STOP, &stop);
        if (ioctl(fd, USB_FS_CLOSE, &close_ep))
            result = 1;
    }
    if (initialized) {
        struct usb_fs_uninit uninit = {0};
        if (ioctl(fd, USB_FS_UNINIT, &uninit))
            result = 1;
    }
    if (changed && ioctl(fd, USB_SET_ALTINTERFACE, &old_alt))
        result = 1;
    if (detached) {
        int zero = 0;
        int rc = ioctl(fd, USB_DEVICEENUMERATE, &zero);
        log_line("re-enumerate rc %d errno %d", rc, rc ? errno : 0);
        if (rc)
            result = 1;
    }
    if (fd >= 0)
        close(fd);
#ifdef DS5_PLAY_SONG
    if (g_source)
        fclose(g_source);
    notify(result ? "DualSense music stopped with an error. Check ds5speaker.txt."
                  : "DualSense music finished. Press PS to reconnect if needed.");
#else
    notify(result ? "DualSense speaker test failed. Check ds5speaker.txt."
                  : "DualSense speaker test finished. Press PS to reconnect if needed.");
#endif
    log_line("done: result %d", result);
    if (g_log)
        fclose(g_log);
    if (g_usb_log)
        fclose(g_usb_log);
    return result;
}

int main(void)
{
#ifdef DS5_PLAY_SONG
    // Validate and size the bounded source before taking any USB interface.
    g_source = fopen(SONG_PATH, "rb");
    struct stat st;
    if (!g_source || fstat(fileno(g_source), &st) || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || st.st_size % 2 || st.st_size > 48000 * 2 * 300) {
        if (g_source)
            fclose(g_source);
        notify("DualSense music: missing or invalid song-mono.raw (48 kHz mono S16_LE, max 5 minutes).");
        return 1;
    }
    uint32_t samples = (uint32_t)(st.st_size / 2);
    uint32_t block = FRAMES * SAMPLES_PER_PACKET;
    g_total_samples = ((samples + block - 1) / block) * block;
    uint32_t duration_ms = (g_total_samples + 47u) / 48u;
    g_playback_limit_ms = duration_ms + 4000u;
    g_watchdog_ms = duration_ms + 15000u;
#endif
    pid_t child = fork();
    if (child < 0) {
#ifdef DS5_PLAY_SONG
        fclose(g_source);
#endif
        return 1;
    }
    if (!child)
        _exit(playback());
#ifdef DS5_PLAY_SONG
    fclose(g_source); // Worker owns its inherited descriptor; parent only supervises.
    g_source = NULL;
#endif
    uint64_t start = now_ms();
    int status = 0, terminated = 0;
    for (;;) {
        pid_t rc = waitpid(child, &status, WNOHANG);
        if (rc == child)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        if (rc < 0 && errno != EINTR)
            return 1;
        if (!terminated && now_ms() - start >= g_watchdog_ms - 2000u) {
            kill(child, SIGTERM);
            terminated = 1;
        }
        if (now_ms() - start >= g_watchdog_ms) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            notify("DualSense speaker test timed out. Reconnect controller.");
            return 1;
        }
        usleep(10000);
    }
}
