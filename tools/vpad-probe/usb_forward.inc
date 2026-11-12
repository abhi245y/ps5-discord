// SPDX-License-Identifier: AGPL-3.0-or-later
// Included only by the USB build of vpad_probe.c; USB callbacks never do HID I/O.
#include "ds5_usb_owner.h"
static ds5_usb_owner g_usb_owner = DS5_USB_OWNER_INIT;
#if PROBE_USB_CALL
static atomic_int g_core_lost;
#include "usb_call.inc"
#endif
#if PROBE_USB_TONE
#include "usb_test_tone.h"
static uint64_t g_tone_cursor;
static size_t usb_playback(void *user, int16_t *stereo, size_t frames)
{
    (void)user;
    usb_test_tone(stereo, frames, &g_tone_cursor);
    return frames;
}
#endif
static pthread_t g_usb_thread;
static int g_usb_started, g_usb_result;
static atomic_int g_usb_done, g_usb_cancel;
static usb_forward_queue g_usb_states;
static ds5_usb_controller g_usb_latest;
static ds5_usb_stats g_usb_stats;
static uint64_t g_usb_received, g_usb_submitted, g_usb_last;
static struct {
    atomic_uint write, read, dropped;
    char lines[32][512];
} g_usb_messages;

static void usb_message(void *user, const char *message)
{
    (void)user;
    unsigned write = atomic_load_explicit(&g_usb_messages.write, memory_order_relaxed);
    unsigned next = (write + 1u) % 32u;
    if (next == atomic_load_explicit(&g_usb_messages.read, memory_order_acquire)) {
        atomic_fetch_add(&g_usb_messages.dropped, 1);
        return;
    }
    snprintf(g_usb_messages.lines[write], 512, "%s", message);
    atomic_store_explicit(&g_usb_messages.write, next, memory_order_release);
}

static void usb_drain(void)
{
    unsigned read = atomic_load_explicit(&g_usb_messages.read, memory_order_relaxed);
    while (read != atomic_load_explicit(&g_usb_messages.write, memory_order_acquire)) {
        log_line("USB: %s", g_usb_messages.lines[read]);
        read = (read + 1u) % 32u;
        atomic_store_explicit(&g_usb_messages.read, read, memory_order_release);
    }
    ds5_usb_controller state;
    uint64_t received_ms;
    while (usb_forward_pop(&g_usb_states, &state, &received_ms)) {
        g_usb_latest = state;
        ++g_usb_received;
        g_usb_last = received_ms;
    }
}

static void usb_controller(void *user, const ds5_usb_controller *state, const uint8_t report[64])
{
    (void)user;
    (void)report;
    (void)usb_forward_push(&g_usb_states, state, now_ms());
}

static int usb_service(void *user)
{
    (void)user;
    if (atomic_load(&g_usb_cancel))
        return 1;
#if PROBE_USB_CALL
    if (atomic_load(&g_core_lost))
        return 0; // Main deletes virtual pad before asking USB thread to stop.
    int rc = ds5_pcm_pump(&g_core_pcm, g_core_fd);
    if (rc) {
        usb_message(NULL, "call PCM socket closed or failed; releasing USB");
        atomic_store(&g_core_lost, 1);
    }
    return 0;
#else
    return 0;
#endif
}

static void *usb_owner(void *arg)
{
    (void)arg;
    ds5_usb_config config;
    ds5_usb_default_config(&config);
    config.duration_ms = 0; /* Main owns stop/deadline and stall supervision. */
    config.notifications = 0;
    const ds5_usb_callbacks callbacks = {
        .log = usb_message, .controller = usb_controller, .service = usb_service,
#if PROBE_USB_CALL
        .microphone = usb_call_mic, .playback = usb_playback
#elif PROBE_USB_TONE
        .playback = usb_playback
#endif
    }; /* Normal USB build sends silence; tone build has no mic loopback. */
    g_usb_result = ds5_usb_run(&config, &callbacks, NULL, &g_usb_stats);
    atomic_store_explicit(&g_usb_done, 1, memory_order_release);
    return NULL;
}

static int usb_forward_start(void)
{
#if PROBE_USB_CALL
    // Let the core reject a delayed launch during its Remote Play reservation before
    // even stale-owner recovery can reset the controller. No USB is touched by PCM setup.
    if (usb_call_connect() != 0)
        return -1;
#endif
    int acquired = 1;
    const uint64_t lock_deadline = now_ms() + 3000;
    while (running() && now_ms() < lock_deadline) {
        acquired = ds5_usb_owner_try(&g_usb_owner, DS5_USB_OWNER_DIR,
                                     PROBE_USB_SERVICE ? "controller" : "probe", usb_message, NULL);
        if (acquired != 1)
            break;
        if (wait_until(now_ms() + FRAME_MS) != 0)
            break;
    }
    if (acquired || !running()) {
        log_line("shared USB owner lock unavailable or cancelled (%d)", acquired);
        return -1;
    }
    int rc = pthread_create(&g_usb_thread, NULL, usb_owner, NULL);
    log_line("USB owner pthread_create rc=%d", rc);
    if (rc)
        return -1;
    g_usb_started = 1;
    const uint64_t deadline = now_ms() + 3000;
    while (running() && now_ms() < deadline) {
        usb_drain();
        if (atomic_load_explicit(&g_usb_done, memory_order_acquire))
            break;
        if (g_usb_received) {
            log_line("first real USB report after driver takeover; settle 250ms before virtual AddDevice");
            const uint64_t settle = now_ms() + 250;
            while (running() && now_ms() < settle) {
                usb_drain();
                if (atomic_load_explicit(&g_usb_done, memory_order_acquire) ||
                    wait_until(now_ms() + FRAME_MS) != 0)
                    return -1;
            }
            return running() ? 0 : -1;
        }
        if (wait_until(now_ms() + FRAME_MS) != 0)
            break;
    }
    log_line("USB takeover failed or no controller report within 3000ms");
    return -1;
}

static int usb_forward_run(void)
{
    if (!g_bound || !running()) {
        log_line("USB forwarding refused: successful virtual binding required");
        return -1;
    }
    log_line("USB FORWARD READY: move real controller buttons/sticks and speak; playback=%s",
             PROBE_USB_CALL ? "Discord PCM" : PROBE_USB_TONE ? "test beeps" : "silent");
    uint64_t last_log = now_ms();
    g_quiet = 0;
    while (running() && (PROBE_USB_SERVICE || now_ms() - g_start < 18000u)) {
        usb_drain();
#if PROBE_USB_CALL
        if (atomic_load(&g_core_lost)) {
            log_line("core released USB audio ownership; deleting virtual pad before handback");
            g_quiet = 0;
            return 0;
        }
#endif
        if (atomic_load_explicit(&g_usb_done, memory_order_acquire) || now_ms() - g_usb_last > 1000u) {
            log_line("USB owner ended or input stale; stopping virtual input");
            g_quiet = 0;
            return -1;
        }
        uint8_t report[160];
        ds5_usb_controller state = usb_forward_fresh_state(&g_usb_latest, g_usb_last, now_ms());
        usb_forward_report(report, &state);
        struct hid_request request = {.command = (uint32_t)g_handle, .data = report};
        if (checked_ioctl("InsertData USB", HID_INSERT_DATA, &request) != 0) {
            g_quiet = 0;
            return -1;
        }
        ++g_usb_submitted;
        g_quiet = 1;
        if (now_ms() - last_log >= 1000u) {
            log_line("USB forwarding: received=%llu submitted=%llu buttons=0x%08x dropped=%u",
                (unsigned long long)g_usb_received, (unsigned long long)g_usb_submitted,
                (unsigned)state.buttons, atomic_load(&g_usb_states.dropped));
            last_log = now_ms();
        }
        if (wait_until(now_ms() + FRAME_MS) != 0)
            break;
    }
    g_quiet = 0;
    return running() ? 0 : -1;
}

static void usb_forward_stop(void)
{
    if (!g_usb_started) {
#if PROBE_USB_CALL
        usb_call_close();
#endif
        (void)ds5_usb_owner_release(&g_usb_owner, 1); // No USB thread touched the device.
        usb_drain(); // Includes stale-owner recovery failures before pthread_create.
        return;
    }
    /* Called after virtual deletion: real USB pad comes back afterward. */
    atomic_store(&g_usb_cancel, 1);
    ds5_usb_request_stop();
    int rc = pthread_join(g_usb_thread, NULL); /* Outer guard bounds a stalled ioctl/join. */
    log_line("USB owner pthread_join rc=%d", rc);
    if (rc) {
        g_failed = 1;
        return;
    }
    g_usb_started = 0;
    int verified = ds5_usb_handback_verified();
    if (!verified) {
        log_line("USB handback not verified; recovering before releasing shared owner lock");
        verified = ds5_usb_recover() == DS5_USB_OK;
    }
    if (ds5_usb_owner_release(&g_usb_owner, verified))
        log_line("could not update USB taken marker errno=%d", errno);
    usb_drain();
    log_line("USB result=%d reports=%llu mic_frames=%llu mic_peak=%u playback_frames=%llu playback_peak=%u silent_playback_frames=%llu queue_dropped=%u logs_dropped=%u",
        g_usb_result, (unsigned long long)g_usb_stats.controller_reports,
        (unsigned long long)g_usb_stats.microphone_frames, g_usb_stats.microphone_peak,
        (unsigned long long)g_usb_stats.playback_frames, g_usb_stats.playback_peak,
        (unsigned long long)g_usb_stats.playback_silence_frames,
        atomic_load(&g_usb_states.dropped), atomic_load(&g_usb_messages.dropped));
    if (g_usb_result != DS5_USB_OK)
        g_failed = 1;
#if PROBE_USB_CALL
    usb_call_close();
#endif
#if PROBE_USB_SERVICE
    extern int32_t sceKernelSendNotificationRequest(int, void *, size_t, int);
    struct { char reserved[45]; char message[3075]; } notice = {0};
    snprintf(notice.message, sizeof notice.message, "%s", g_usb_result == DS5_USB_OK
        ? "Controller audio off. Press PS to use your controller."
        : "Controller audio stopped. Reconnect the controller if it does not respond.");
    (void)sceKernelSendNotificationRequest(0, &notice, sizeof notice, 0);
#endif
}
