// SPDX-License-Identifier: AGPL-3.0-or-later
// Reusable USB runtime derived from hardware-verified DS5 duplex/hotplug probes.
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "ds5_usb_pcm.h"

#define FRAMES 16u
#define MIC_PACKET 196u
#define OUTPUT_PACKET 384u
#define SLOTS 6u
#define HID_IN 4u
#define HID_OUT 5u

extern int32_t sceKernelSendNotificationRequest(int, void *, size_t, int);
static atomic_int g_stop;
static atomic_flag running = ATOMIC_FLAG_INIT;
static struct {
    ds5_usb_config config;
    ds5_usb_callbacks callbacks;
    void *user;
    struct usb_fs_endpoint eps[SLOTS];
    uint8_t data[SLOTS][FRAMES][OUTPUT_PACKET];
    void *pointers[SLOTS][FRAMES];
    uint32_t lengths[SLOTS][FRAMES];
    int busy[SLOTS];
    int headphones, observed, muted, route_dirty, sent_headphones, sent_muted, applied, initial;
    uint8_t previous_buttons;
    uint64_t jack_changed, last[SLOTS];
    unsigned mic_transfers, output_transfers, hid_reports, handoffs;
    uint64_t input_frames, output_frames, nonzero, headphone_frames, speaker_frames, clipped_input, silence_frames;
    unsigned input_peak, output_peak;
    double input_squares;
} g;

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
    if (g.callbacks.log)
        g.callbacks.log(g.user, line);
    else
        fprintf(stderr, "[ds5-usb] %s\n", line);
}

static void notify(const char *message)
{
    if (!g.config.notifications)
        return;
    struct { char reserved[45]; char message[3075]; } request = {0};
    snprintf(request.message, sizeof request.message, "%s", message);
    log_line("notification rc %d: %s", sceKernelSendNotificationRequest(0, &request, sizeof request, 0), message);
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
        g.pointers[slot][f] = g.data[slot][f];
        g.lengths[slot][f] = bytes;
    }
    g.eps[slot] = (struct usb_fs_endpoint){.ppBuffer = g.pointers[slot], .pLength = g.lengths[slot],
        .nFrames = frames, .timeout = 250,
        .flags = slot < 2 || slot == HID_IN ? USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK : 0};
    struct usb_fs_start start = {.ep_index = (uint8_t)slot};
    if (ioctl(fd, USB_FS_START, &start)) {
        log_line("submit slot %u errno %d", slot, errno);
        return -1;
    }
    g.busy[slot] = 1;
    g.last[slot] = now_ms();
    return 0;
}

static int submit_route(int fd)
{
    if (g.busy[HID_OUT] || !g.route_dirty)
        return 0;
    ds5_usb_route_report(g.data[HID_OUT][0], &g.config, g.headphones, g.muted);
    g.sent_headphones = g.headphones;
    g.sent_muted = g.muted;
    g.route_dirty = 0;
    log_line("route requested: %s mic %s (built-in mic forced)",
             g.headphones ? "headphones" : "speaker", g.muted ? "muted" : "live");
    return submit(fd, HID_OUT);
}

static void process_hid(void)
{
    ds5_usb_controller controller;
    if (!ds5_usb_parse_controller(g.data[HID_IN][0], g.lengths[HID_IN][0], &controller))
        return;
    if (g.callbacks.controller)
        g.callbacks.controller(g.user, &controller, g.data[HID_IN][0]);
    int hp = !!(g.data[HID_IN][0][54] & 1);
    int muted = !!(g.data[HID_IN][0][54] & 4);
    uint8_t buttons = g.data[HID_IN][0][10];
    g.hid_reports++;
    if (g.initial) {
        g.headphones = g.observed = hp;
        g.muted = muted;
        g.previous_buttons = buttons;
        g.jack_changed = now_ms();
        g.initial = 0;
        g.route_dirty = 1;
        log_line("initial HID status %02x headphones %d mic-muted %d", g.data[HID_IN][0][54], hp, muted);
        return;
    }
    if ((buttons & 4) && !(g.previous_buttons & 4)) {
        g.muted = !g.muted;
        g.route_dirty = 1;
        log_line("mute button: mic %s", g.muted ? "muted" : "live");
    }
    if (g.config.stop_on_ps && (buttons & 1) && !(g.previous_buttons & 1))
        g_stop = 1;
    g.previous_buttons = buttons;
    if (hp != g.observed) {
        g.observed = hp;
        g.jack_changed = now_ms();
        log_line("jack observed: status %02x headphones %d", g.data[HID_IN][0][54], hp);
    }
    if (g.observed != g.headphones && now_ms() - g.jack_changed >= 100) {
        g.headphones = g.observed;
        g.route_dirty = 1;
    }
}

static int process_mic(unsigned slot)
{
    if (g.eps[slot].aFrames > FRAMES)
        return -1;
    int16_t mono[DS5_USB_MAX_MIC_FRAMES];
    size_t count = 0;
    g.mic_transfers++;
    for (unsigned f = 0; f < g.eps[slot].aFrames; f++) {
        uint32_t bytes = g.lengths[slot][f];
        if (bytes > MIC_PACKET || bytes % 4)
            return -1;
        for (unsigned off = 0; off < bytes; off += 4) {
            int left = ds5_usb_sample(g.data[slot][f] + off);
            int right = ds5_usb_sample(g.data[slot][f] + off + 2);
            g.clipped_input += left == 32767 || left == -32768 || right == 32767 || right == -32768;
            int value = (left + right) / 2;
            unsigned magnitude = (unsigned)(value < 0 ? -value : value);
            if (magnitude > g.input_peak)
                g.input_peak = magnitude;
            g.input_squares += (double)value * value;
            g.nonzero += value != 0;
            g.input_frames++;
            mono[count++] = (int16_t)(g.muted ? 0 : value);
        }
    }
    if (count && g.callbacks.microphone)
        g.callbacks.microphone(g.user, mono, count, g.muted);
    return 0;
}

static int submit_output(int fd, unsigned slot)
{
    int headphones = g.applied >= 0 ? g.applied : g.headphones;
    int16_t stereo[2 * DS5_USB_OUTPUT_FRAMES] = {0};
    size_t supplied = g.callbacks.playback ? g.callbacks.playback(g.user, stereo, DS5_USB_OUTPUT_FRAMES) : 0;
    if (supplied > DS5_USB_OUTPUT_FRAMES)
        return -1;
    // Only declared frames may be used, even if a callback touched its whole buffer.
    memset(stereo + 2 * supplied, 0, (DS5_USB_OUTPUT_FRAMES - supplied) * 2 * sizeof *stereo);
    g.silence_frames += DS5_USB_OUTPUT_FRAMES - supplied;
    uint32_t gain = headphones ? g.config.headphone_gain_q15 : g.config.speaker_gain_q15;
    for (unsigned f = 0; f < FRAMES; f++) {
        ds5_usb_output_packet(g.data[slot][f], stereo + f * 96, headphones, gain);
        for (unsigned s = 0; s < 48; s++)
            for (unsigned ch = 0; ch < 2; ch++) {
                int value = ds5_usb_sample(g.data[slot][f] + s * 8 + ch * 2);
                unsigned magnitude = (unsigned)(value < 0 ? -value : value);
                if (magnitude > g.output_peak)
                    g.output_peak = magnitude;
            }
    }
    if (headphones)
        g.headphone_frames += FRAMES * 48;
    else
        g.speaker_frames += FRAMES * 48;
    return submit(fd, slot);
}

static int complete_one(int fd, unsigned *slot)
{
    struct usb_fs_complete complete = {0};
    if (ioctl(fd, USB_FS_COMPLETE, &complete))
        return errno == EBUSY || errno == EINTR ? 0 : -1;
    *slot = complete.ep_index;
    if (*slot >= SLOTS || !g.busy[*slot] || g.eps[*slot].status) {
        log_line("bad completion slot %u status %d", *slot, *slot < SLOTS ? g.eps[*slot].status : -1);
        return -1;
    }
    g.busy[*slot] = 0;
    g.last[*slot] = now_ms();
    return 1;
}

static int run_usb(void)
{
    int fd = -1, result = 1, detached = 0, initialized = 0, hid = -1;
    unsigned changed = 0, opened = 0;
    struct ds5_mic_format formats[2];
    struct usb_alt_interface old[2];
    uint8_t hid_ep[2] = {0};
    log_line("start: DS5 USB controller + full-duplex audio; duration %u ms", g.config.duration_ms);
    log_line("gain: mic %02x headphones %02x speaker %02x, Q15 headphone/speaker %u/%u",
             g.config.mic_volume, g.config.headphone_volume, g.config.speaker_volume,
             g.config.headphone_gain_q15, g.config.speaker_gain_q15);
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
    if (!g.config.audio_only && ioctl(fd, USB_IFACE_DRIVER_DETACH, &hid))
        goto cleanup;
    for (unsigned n = 0; n < 2; n++) {
        struct usb_alt_interface alt = {.uai_interface_index = formats[n].interface_index,
                                        .uai_alt_index = formats[n].alt_index};
        if (ioctl(fd, USB_SET_ALTINTERFACE, &alt))
            goto cleanup;
        changed++;
    }
    struct usb_fs_init init = {.pEndpoints = g.eps, .ep_index_max = SLOTS};
    if (ioctl(fd, USB_FS_INIT, &init))
        goto cleanup;
    initialized = 1;
    const unsigned slots = g.config.audio_only ? HID_IN : SLOTS;
    for (unsigned slot = 0; slot < slots; slot++) {
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
    if (g.config.audio_only) {
        g.initial = 0;
        g.applied = 1; // stereo: the console picks speaker or headphones
    } else if (submit(fd, HID_IN))
        goto cleanup;
    uint64_t deadline = now_ms() + 1000;
    while (!g.config.audio_only && g.initial && !g_stop && now_ms() < deadline) {
        unsigned slot;
        int rc = complete_one(fd, &slot);
        if (rc < 0 || (rc && slot != HID_IN))
            goto cleanup;
        if (rc)
            process_hid();
        else
            usleep(1000);
    }
    if (g.initial || g_stop || (!g.config.audio_only && submit_route(fd)))
        goto cleanup;
    deadline = now_ms() + 1000;
    while (g.busy[HID_OUT] && !g_stop && now_ms() < deadline) {
        unsigned slot;
        int rc = complete_one(fd, &slot);
        if (rc < 0 || (rc && slot != HID_OUT))
            goto cleanup;
        if (rc) {
            if (g.eps[HID_OUT].aFrames != 1 || g.lengths[HID_OUT][0] != 63)
                goto cleanup;
            g.applied = g.sent_headphones;
            log_line("initial route applied: %s mic %s", g.applied ? "headphones" : "speaker",
                     g.sent_muted ? "muted" : "live");
            if (g.callbacks.route)
                g.callbacks.route(g.user, g.applied, g.sent_muted);
        } else {
            usleep(1000);
        }
    }
    if (g.busy[HID_OUT] || g_stop)
        goto cleanup;
    notify(g.muted ? "DS5 USB audio connected. Mic is muted; use mute button to enable."
                   : "DS5 USB audio connected. Headphone handoff active.");
    uint64_t start = now_ms(), last_log = start;
    for (unsigned slot = 0; slot < SLOTS; slot++)
        g.last[slot] = start;
    if ((!g.config.audio_only && submit(fd, HID_IN)) || submit(fd, 0) || submit(fd, 1) ||
        submit_output(fd, 2) || submit_output(fd, 3))
        goto cleanup;
    while (!g_stop && (!g.config.duration_ms || now_ms() - start < g.config.duration_ms)) {
        if (g.callbacks.service) {
            int action = g.callbacks.service(g.user);
            if (action < 0)
                goto cleanup;
            if (action > 0)
                break;
        }
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
            if (g.eps[slot].aFrames != FRAMES)
                goto cleanup;
            for (unsigned f = 0; f < FRAMES; f++)
                if (g.lengths[slot][f] != OUTPUT_PACKET)
                    goto cleanup;
            g.output_transfers++;
            g.output_frames += FRAMES * 48;
            if (submit_output(fd, slot))
                goto cleanup;
        } else if (slot == HID_IN) {
            process_hid();
            if (submit(fd, HID_IN))
                goto cleanup;
        } else {
            if (g.eps[HID_OUT].aFrames != 1 || g.lengths[HID_OUT][0] != 63)
                goto cleanup;
            if (g.applied != g.sent_headphones) {
                g.handoffs++;
                g.applied = g.sent_headphones;
                notify(g.applied ? "DS5 audio moved to headphones."
                                 : "DS5 audio moved to controller speaker.");
            }
            log_line("route applied: %s mic %s", g.sent_headphones ? "headphones" : "speaker",
                     g.sent_muted ? "muted" : "live");
            if (g.callbacks.route)
                g.callbacks.route(g.user, g.sent_headphones, g.sent_muted);
        }
        if (submit_route(fd))
            goto cleanup;
        for (unsigned n = 0; n < slots; n++)
            if (g.busy[n] && now_ms() - g.last[n] > 1000) {
                log_line("stalled slot %u", n);
                goto cleanup;
            }
        if (now_ms() - last_log >= 1000) {
            log_line("t=%us mic %u OUT %u HID %u route %s mute %d peak IN/OUT %u/%u silent-OUT %llu frames",
                (unsigned)((now_ms() - start) / 1000), g.mic_transfers, g.output_transfers, g.hid_reports,
                g.applied ? "headphones" : "speaker", g.muted, g.input_peak, g.output_peak,
                (unsigned long long)g.silence_frames);
            last_log = now_ms();
        }
    }
    result = 0;
    log_line("summary: IN %llu frames OUT %llu frames, mic transfers %u OUT transfers %u HID %u handoffs %u wall %llu ms",
             (unsigned long long)g.input_frames, (unsigned long long)g.output_frames,
             g.mic_transfers, g.output_transfers, g.hid_reports, g.handoffs,
             (unsigned long long)(now_ms() - start));
    log_line("levels: input RMS %.1f peak %u nonzero %llu output peak %u; routed headphone/speaker %llu/%llu frames",
        g.input_frames ? sqrt(g.input_squares / g.input_frames) : 0, g.input_peak,
        (unsigned long long)g.nonzero, g.output_peak, (unsigned long long)g.headphone_frames,
        (unsigned long long)g.speaker_frames);
    log_line("input full-scale stereo frames %llu", (unsigned long long)g.clipped_input);

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
    if (detached && g.config.audio_only) {
        // ponytail: the console's audio driver stays off this controller until it is
        // replugged; re-enumerating would disconnect the buttons too (press PS).
        log_line("audio released; the console's controller audio returns on replug");
    } else if (detached) {
        int zero = 0;
        int rc = ioctl(fd, USB_DEVICEENUMERATE, &zero);
        log_line("re-enumerate rc %d errno %d", rc, rc ? errno : 0);
        if (rc)
            result = 1;
    }
    if (fd >= 0)
        close(fd);
    if (!g.config.audio_only)
        notify(result ? "DS5 USB audio stopped with an error. Press PS to reconnect."
                      : "DS5 USB audio released. Press PS to reconnect controller.");
    log_line("done: result %d", result);
    return result ? DS5_USB_ERROR : DS5_USB_OK;
}

void ds5_usb_default_config(ds5_usb_config *config)
{
    if (!config)
        return;
    *config = (ds5_usb_config){.duration_ms = 60000, .headphone_gain_q15 = 16384,
        .speaker_gain_q15 = 32768, .mic_volume = 0x20, .headphone_volume = 0x7f,
        .speaker_volume = 0x64, .speaker_pre_gain = 2, .notifications = 1, .stop_on_ps = 0};
}

void ds5_usb_request_stop(void)
{
    atomic_store(&g_stop, 1);
}

int ds5_usb_run(const ds5_usb_config *config, const ds5_usb_callbacks *callbacks,
                void *user, ds5_usb_stats *stats)
{
    ds5_usb_config defaults;
    ds5_usb_default_config(&defaults);
    if (!config)
        config = &defaults;
    if (config->duration_ms > 3600000u || config->headphone_gain_q15 > 65536u ||
        config->speaker_gain_q15 > 65536u || config->mic_volume > 0x40 ||
        config->headphone_volume > 0x7f || config->speaker_volume > 0x64 || config->speaker_pre_gain > 7)
        return DS5_USB_BAD_CONFIG;
    if (atomic_flag_test_and_set(&running))
        return DS5_USB_BUSY;
    memset(&g, 0, sizeof g);
    atomic_store(&g_stop, 0);
    g.config = *config;
    if (callbacks)
        g.callbacks = *callbacks;
    g.user = user;
    g.applied = -1;
    g.initial = 1;
    int result = run_usb();
    if (stats)
        *stats = (ds5_usb_stats){.microphone_frames = g.input_frames,
            .playback_frames = g.output_frames, .controller_reports = g.hid_reports,
            .route_changes = g.handoffs, .playback_silence_frames = g.silence_frames,
            .input_full_scale_frames = g.clipped_input,
            .microphone_peak = g.input_peak, .playback_peak = g.output_peak};
    atomic_flag_clear(&running);
    return result;
}
