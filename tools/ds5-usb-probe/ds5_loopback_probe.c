// SPDX-License-Identifier: AGPL-3.0-or-later
// Sixty-second full-duplex DualSense mic/output test with headphone hotplug.
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
#include "ds5_loopback_audio.h"

#define FRAMES 16u
#define MIC_PACKET 196u
#define OUTPUT_PACKET 384u
#define SLOTS 6u
#define HID_IN 4u
#define HID_OUT 5u
#define RUN_MS 60000u
#define WATCHDOG_MS (RUN_MS + 15000u)

extern int32_t sceKernelSendNotificationRequest(int, void *, size_t, int);
static FILE *g_log, *g_usb_log;
static volatile sig_atomic_t g_stop;
static struct usb_fs_endpoint g_eps[SLOTS];
static uint8_t g_data[SLOTS][FRAMES][OUTPUT_PACKET];
static void *g_pointers[SLOTS][FRAMES];
static uint32_t g_lengths[SLOTS][FRAMES];
static struct ds5_loop_ring g_ring;
static int g_busy[SLOTS];
static int g_headphones, g_observed, g_muted, g_route_dirty, g_sent_headphones, g_sent_muted;
static int g_applied = -1, g_initial = 1;
static uint8_t g_previous_buttons;
static uint64_t g_jack_changed;
static uint64_t g_last[SLOTS];
static unsigned g_mic_transfers, g_output_transfers, g_hid_reports, g_handoffs;
static uint64_t g_input_frames, g_output_frames, g_nonzero, g_headphone_frames, g_speaker_frames;
static uint64_t g_clipped_input;
static unsigned g_input_peak, g_output_peak;
static double g_input_squares;

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
    fprintf(stderr, "[ds5-loopback] %s\n", line);
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
    log_line("notification rc %d: %s", sceKernelSendNotificationRequest(0, &request, sizeof request, 0), message);
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
    return -1;
}

static int discover(int fd, struct ds5_mic_format formats[2], int *hid, uint8_t endpoints[2])
{
    int config = 0;
    uint8_t data[4096];
    if (ioctl(fd, USB_GET_CONFIG, &config) || config < 0 || config >= 255)
        return -1;
    struct usb_gen_descriptor d = {.ugd_data = data, .ugd_maxlen = sizeof data,
                                   .ugd_config_index = (uint8_t)config};
    if (ioctl(fd, USB_GET_FULL_DESC, &d) || ds5_audio_find(data, d.ugd_actlen, &formats[0], 2, 1) != 1 ||
        ds5_audio_find(data, d.ugd_actlen, &formats[1], 4, 0) != 1 ||
        formats[0].packet_bytes != MIC_PACKET || formats[0].control_index != formats[1].control_index)
        return -1;
    int index = -1, previous = -1, is_hid = 0, intervals[2] = {0};
    *hid = -1;
    for (size_t off = 9; off < d.ugd_actlen; off += data[off]) {
        const uint8_t *p = data + off;
        if (p[1] == 4) {
            if (p[2] != previous) {
                index++;
                previous = p[2];
            }
            is_hid = p[5] == 3 && p[3] == 0;
        } else if (p[1] == 5 && p[0] >= 7) {
            for (unsigned n = 0; n < 2; n++)
                if (index == formats[n].interface_index && p[2] == formats[n].endpoint)
                    intervals[n] = p[6];
            if (is_hid && (p[3] & 3) == 3 && ds5_le16(p + 4) >= 64) {
                *hid = index;
                endpoints[(p[2] & 0x80) ? 0 : 1] = p[2];
            }
        }
    }
    if (*hid < 0 || !endpoints[0] || !endpoints[1] || intervals[0] != 4 || intervals[1] != 4)
        return -1;
    for (unsigned n = 0; n < 2; n++)
        log_line("%s: interface %u alt-index %u endpoint %02x packet %u", n ? "output" : "mic",
                 formats[n].interface_index, formats[n].alt_index, formats[n].endpoint, formats[n].packet_bytes);
    log_line("HID index %d IN %02x OUT %02x", *hid, endpoints[0], endpoints[1]);
    return 0;
}

static int submit(int fd, unsigned slot)
{
    unsigned frames = slot < HID_IN ? FRAMES : 1;
    uint32_t bytes = slot < 2 ? MIC_PACKET : slot < 4 ? OUTPUT_PACKET : slot == HID_IN ? 64 : 63;
    for (unsigned f = 0; f < frames; f++) {
        g_pointers[slot][f] = g_data[slot][f];
        g_lengths[slot][f] = bytes;
    }
    g_eps[slot] = (struct usb_fs_endpoint){.ppBuffer = g_pointers[slot], .pLength = g_lengths[slot],
        .nFrames = frames, .timeout = 250,
        .flags = slot < 2 || slot == HID_IN ? USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK : 0};
    struct usb_fs_start start = {.ep_index = (uint8_t)slot};
    if (ioctl(fd, USB_FS_START, &start)) {
        log_line("submit slot %u errno %d", slot, errno);
        return -1;
    }
    g_busy[slot] = 1;
    g_last[slot] = now_ms();
    return 0;
}

static int submit_route(int fd)
{
    if (g_busy[HID_OUT] || !g_route_dirty)
        return 0;
    ds5_loop_route(g_data[HID_OUT][0], g_headphones, g_muted);
    g_sent_headphones = g_headphones;
    g_sent_muted = g_muted;
    g_route_dirty = 0;
    log_line("route requested: %s mic %s (built-in mic forced)",
             g_headphones ? "headphones" : "speaker", g_muted ? "muted" : "live");
    return submit(fd, HID_OUT);
}

static void process_hid(void)
{
    int hp, muted;
    uint8_t buttons;
    if (!ds5_loop_status(g_data[HID_IN][0], g_lengths[HID_IN][0], &hp, &muted, &buttons))
        return;
    g_hid_reports++;
    if (g_initial) {
        g_headphones = g_observed = hp;
        g_muted = muted;
        g_previous_buttons = buttons;
        g_jack_changed = now_ms();
        g_initial = 0;
        g_route_dirty = 1;
        log_line("initial HID status byte54=%02x headphones %d mic-muted %d", g_data[HID_IN][0][54], hp, muted);
        return;
    }
    if ((buttons & 4) && !(g_previous_buttons & 4)) {
        g_muted = !g_muted;
        ds5_loop_reset(&g_ring);
        g_route_dirty = 1;
        log_line("mute button: mic %s", g_muted ? "muted" : "live");
    }
    if ((buttons & 1) && !(g_previous_buttons & 1)) {
        log_line("PS pressed: stopping loopback");
        g_stop = 1;
    }
    g_previous_buttons = buttons;
    if (hp != g_observed) {
        g_observed = hp;
        g_jack_changed = now_ms();
        log_line("jack observed: status %02x headphones %d", g_data[HID_IN][0][54], hp);
    }
    if (g_observed != g_headphones && now_ms() - g_jack_changed >= 100) {
        g_headphones = g_observed;
        ds5_loop_reset(&g_ring);
        g_route_dirty = 1;
    }
}

static int process_mic(unsigned slot)
{
    if (g_eps[slot].aFrames > FRAMES)
        return -1;
    g_mic_transfers++;
    for (unsigned f = 0; f < g_eps[slot].aFrames; f++) {
        uint32_t bytes = g_lengths[slot][f];
        if (bytes > MIC_PACKET || bytes % 4)
            return -1;
        for (unsigned off = 0; off < bytes; off += 4) {
            int left = ds5_signed_sample(g_data[slot][f] + off);
            int right = ds5_signed_sample(g_data[slot][f] + off + 2);
            g_clipped_input += left == 32767 || left == -32768 || right == 32767 || right == -32768;
            int value = (left + right) / 2;
            unsigned magnitude = (unsigned)(value < 0 ? -value : value);
            if (magnitude > g_input_peak)
                g_input_peak = magnitude;
            g_input_squares += (double)value * value;
            g_nonzero += value != 0;
            g_input_frames++;
            ds5_loop_push(&g_ring, g_muted ? 0 : value);
        }
    }
    return 0;
}

static int submit_output(int fd, unsigned slot)
{
    int headphones = g_applied >= 0 ? g_applied : g_headphones;
    for (unsigned f = 0; f < FRAMES; f++) {
        ds5_loop_packet(&g_ring, g_data[slot][f], headphones, g_muted);
        for (unsigned s = 0; s < 48; s++) {
            int value = ds5_signed_sample(g_data[slot][f] + s * 8 + 2);
            unsigned magnitude = (unsigned)(value < 0 ? -value : value);
            if (magnitude > g_output_peak)
                g_output_peak = magnitude;
        }
    }
    if (headphones)
        g_headphone_frames += FRAMES * 48;
    else
        g_speaker_frames += FRAMES * 48;
    return submit(fd, slot);
}

static int complete_one(int fd, unsigned *slot)
{
    struct usb_fs_complete complete = {0};
    if (ioctl(fd, USB_FS_COMPLETE, &complete))
        return errno == EBUSY || errno == EINTR ? 0 : -1;
    *slot = complete.ep_index;
    if (*slot >= SLOTS || !g_busy[*slot] || g_eps[*slot].status) {
        log_line("bad completion slot %u status %d", *slot, *slot < SLOTS ? g_eps[*slot].status : -1);
        return -1;
    }
    g_busy[*slot] = 0;
    g_last[*slot] = now_ms();
    return 1;
}

static int loopback(void)
{
    int fd = -1, result = 1, detached = 0, initialized = 0, hid = -1;
    unsigned changed = 0, opened = 0;
    struct ds5_mic_format formats[2];
    struct usb_alt_interface old[2];
    uint8_t hid_ep[2] = {0};
    mkdir("/data/ps5-discord-probe", 0755);
    g_log = fopen("/data/ps5-discord-probe/ds5loopback.txt", "w");
    g_usb_log = fopen("/mnt/usb0/ds5-loopback-probe.log", "a");
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    log_line("start: 60-second full-duplex loopback; built-in mic, automatic headphone output; haptics zero");
    log_line("gain: mic 20, headphone volume 7f loopback 1/2 limit 16384, speaker volume 64 loopback 1/1");
    fd = scan();
    if (fd < 0 || discover(fd, formats, &hid, hid_ep))
        goto cleanup;
    for (unsigned n = 0; n < 2; n++) {
        old[n] = (struct usb_alt_interface){.uai_interface_index = formats[n].interface_index};
        if (ioctl(fd, USB_GET_ALTINTERFACE, &old[n]))
            goto cleanup;
    }
    if (formats[0].control_index != 255) {
        int parent = formats[0].control_index;
        int active = ioctl(fd, USB_IFACE_DRIVER_ACTIVE, &parent);
        if (active && errno != ENXIO)
            goto cleanup;
        if (!active) {
            if (ioctl(fd, USB_IFACE_DRIVER_DETACH, &parent))
                goto cleanup;
            detached = 1;
        }
    }
    for (unsigned n = 0; n < 2; n++) {
        int iface = formats[n].interface_index;
        if (ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface))
            goto cleanup;
        detached = 1;
    }
    if (ioctl(fd, USB_IFACE_DRIVER_DETACH, &hid))
        goto cleanup;
    for (unsigned n = 0; n < 2; n++) {
        struct usb_alt_interface alt = {.uai_interface_index = formats[n].interface_index,
                                        .uai_alt_index = formats[n].alt_index};
        if (ioctl(fd, USB_SET_ALTINTERFACE, &alt))
            goto cleanup;
        changed++;
    }
    struct usb_fs_init init = {.pEndpoints = g_eps, .ep_index_max = SLOTS};
    if (ioctl(fd, USB_FS_INIT, &init))
        goto cleanup;
    initialized = 1;
    for (unsigned slot = 0; slot < SLOTS; slot++) {
        struct usb_fs_open op = {.ep_index = (uint8_t)slot,
            .ep_no = slot < 2 ? formats[0].endpoint : slot < 4 ? formats[1].endpoint : hid_ep[slot - 4],
            .max_frames = slot < HID_IN ? FRAMES : 1,
            .max_bufsize = slot < 2 ? FRAMES * MIC_PACKET : slot < 4 ? FRAMES * OUTPUT_PACKET : 64};
        if (ioctl(fd, USB_FS_OPEN, &op)) {
            log_line("open slot %u failed errno %d", slot, errno);
            goto cleanup;
        }
        opened++;
        uint32_t minimum = slot < 2 ? MIC_PACKET : slot < 4 ? OUTPUT_PACKET : 64;
        if (op.max_frames < (slot < HID_IN ? FRAMES : 1) || op.max_packet_length < minimum ||
            op.max_bufsize < (slot < HID_IN ? FRAMES * minimum : minimum))
            goto cleanup;
        log_line("slot %u endpoint %02x opened: packet %u frames %u", slot, op.ep_no,
                 op.max_packet_length, (unsigned)op.max_frames);
    }
    if (submit(fd, HID_IN))
        goto cleanup;
    uint64_t deadline = now_ms() + 1000;
    while (g_initial && !g_stop && now_ms() < deadline) {
        unsigned slot;
        int rc = complete_one(fd, &slot);
        if (rc < 0 || (rc && slot != HID_IN))
            goto cleanup;
        if (rc)
            process_hid();
        else
            usleep(1000);
    }
    if (g_initial || g_stop || submit_route(fd))
        goto cleanup;
    deadline = now_ms() + 1000;
    while (g_busy[HID_OUT] && !g_stop && now_ms() < deadline) {
        unsigned slot;
        int rc = complete_one(fd, &slot);
        if (rc < 0 || (rc && slot != HID_OUT))
            goto cleanup;
        if (rc) {
            if (g_eps[HID_OUT].aFrames != 1 || g_lengths[HID_OUT][0] != 63)
                goto cleanup;
            g_applied = g_sent_headphones;
            log_line("initial route applied: %s mic %s", g_applied ? "headphones" : "speaker",
                     g_sent_muted ? "muted" : "live");
        } else {
            usleep(1000);
        }
    }
    if (g_busy[HID_OUT] || g_stop)
        goto cleanup;
    notify(g_muted ? "Mic loopback ready but muted. Press mute to enable; PS stops test."
                   : "Mic loopback started for 60 seconds. Speak normally. Watch prompts for headphone test. PS stops test.");
    ds5_loop_reset(&g_ring);
    uint64_t start = now_ms(), last_log = start;
    int asked_unplug = 0, asked_replug = 0;
    for (unsigned slot = 0; slot < SLOTS; slot++)
        g_last[slot] = start;
    if (submit(fd, HID_IN) || submit(fd, 0) || submit(fd, 1) ||
        submit_output(fd, 2) || submit_output(fd, 3))
        goto cleanup;
    while (!g_stop && now_ms() - start < RUN_MS) {
        unsigned slot = 0;
        int rc = complete_one(fd, &slot);
        if (rc < 0)
            goto cleanup;
        if (!rc) {
            usleep(1000);
        } else if (slot < 2) {
            if (process_mic(slot) || submit(fd, slot))
                goto cleanup;
        } else if (slot < 4) {
            if (g_eps[slot].aFrames != FRAMES)
                goto cleanup;
            for (unsigned f = 0; f < FRAMES; f++)
                if (g_lengths[slot][f] != OUTPUT_PACKET)
                    goto cleanup;
            g_output_transfers++;
            g_output_frames += FRAMES * 48;
            if (submit_output(fd, slot))
                goto cleanup;
        } else if (slot == HID_IN) {
            process_hid();
            if (submit(fd, HID_IN))
                goto cleanup;
        } else {
            if (g_eps[HID_OUT].aFrames != 1 || g_lengths[HID_OUT][0] != 63)
                goto cleanup;
            if (g_applied != g_sent_headphones) {
                g_handoffs++;
                g_applied = g_sent_headphones;
                notify(g_applied ? "Mic loopback: headphones connected - audio moved to headphones."
                                 : "Mic loopback: headphones removed - audio moved to controller speaker.");
            }
            log_line("route applied: %s mic %s", g_sent_headphones ? "headphones" : "speaker",
                     g_sent_muted ? "muted" : "live");
        }
        if (submit_route(fd))
            goto cleanup;
        if (!asked_unplug && now_ms() - start >= 10000) {
            asked_unplug = 1;
            if (g_headphones)
                notify("Loopback test: unplug headphones now. Keep speaking into controller mic.");
        }
        if (!asked_replug && now_ms() - start >= 30000) {
            asked_replug = 1;
            if (!g_headphones)
                notify("Loopback test: plug headphones back in now. Keep speaking.");
        }
        for (unsigned n = 0; n < SLOTS; n++)
            if (g_busy[n] && now_ms() - g_last[n] > 1000) {
                log_line("stalled slot %u", n);
                goto cleanup;
            }
        if (now_ms() - last_log >= 1000) {
            log_line("t=%us mic %u OUT %u HID %u route %s mute %d ring %u underruns %llu overruns %llu peak IN/OUT %u/%u",
                (unsigned)((now_ms() - start) / 1000), g_mic_transfers, g_output_transfers, g_hid_reports,
                g_applied ? "headphones" : "speaker", g_muted, g_ring.count,
                (unsigned long long)g_ring.underruns, (unsigned long long)g_ring.overruns, g_input_peak, g_output_peak);
            last_log = now_ms();
        }
    }
    if (g_input_frames && g_output_frames)
        result = 0;
    log_line("summary: IN %llu frames OUT %llu frames, mic transfers %u OUT transfers %u HID %u handoffs %u wall %llu ms",
             (unsigned long long)g_input_frames, (unsigned long long)g_output_frames,
             g_mic_transfers, g_output_transfers, g_hid_reports, g_handoffs,
             (unsigned long long)(now_ms() - start));
    log_line("levels: input RMS %.1f peak %u nonzero %llu output peak %u; routed headphone/speaker %llu/%llu frames",
        g_input_frames ? sqrt(g_input_squares / g_input_frames) : 0, g_input_peak,
        (unsigned long long)g_nonzero, g_output_peak, (unsigned long long)g_headphone_frames,
        (unsigned long long)g_speaker_frames);
    log_line("input full-scale stereo frames %llu", (unsigned long long)g_clipped_input);

cleanup:
    if (result)
        log_line("failure/interruption errno %d", errno);
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
    while (changed)
        if (ioctl(fd, USB_SET_ALTINTERFACE, &old[--changed]))
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
    notify(result ? "Mic loopback failed. Check ds5loopback.txt; press PS to reconnect."
                  : "Mic loopback finished. Press PS to reconnect controller.");
    log_line("done: result %d", result);
    if (g_log)
        fclose(g_log);
    if (g_usb_log)
        fclose(g_usb_log);
    return result;
}

int main(void)
{
    pid_t child = fork();
    if (child < 0)
        return 1;
    if (!child)
        _exit(loopback());
    uint64_t start = now_ms();
    int status = 0, terminated = 0;
    for (;;) {
        pid_t rc = waitpid(child, &status, WNOHANG);
        if (rc == child)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        if (rc < 0 && errno != EINTR)
            return 1;
        if (!terminated && now_ms() - start >= WATCHDOG_MS - 2000u) {
            kill(child, SIGTERM);
            terminated = 1;
        }
        if (now_ms() - start >= WATCHDOG_MS) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            notify("Mic loopback timed out. Reconnect controller.");
            return 1;
        }
        usleep(10000);
    }
}
