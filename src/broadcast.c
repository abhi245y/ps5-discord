// SPDX-License-Identifier: AGPL-3.0-or-later
// Broadcast (broadcast.h). The order matters: the core takes the USB DualSense from
// the system only once the Remote Play session is connected (the session's virtual
// pad is then the user's controller), and gives it back as soon as the session ends,
// so the user is never left without a controller. Afterwards the user presses PS to
// reconnect it to the console.
//
// Remote Play: vendored chiaki-ng (third_party/README.md), paired with the console itself
// per console user (rp_pair.c). Going live as the user on screen takes the USB controller
// over; as another console user it leaves the controller alone. USB:
// tools/ds5-usb-bridge/ds5_usb.c.
#include "broadcast.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "notify.h"
#include "prefs.h"
#include "rp_pair.h"
#include "stream.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char state[16] = "off", error[160];
static _Atomic(const bc_sink *) sink;

static void set_state(const char *s, const char *why)
{
    pthread_mutex_lock(&lock);
    snprintf(state, sizeof state, "%s", s);
    if (why)
        snprintf(error, sizeof error, "%s", why);
    pthread_mutex_unlock(&lock);
}

const char *bc_state(void)
{
    return state;
}

const char *bc_error(void)
{
    return error;
}

void bc_set_sink(const bc_sink *s)
{
    sink = s;
}

// The prefs' golive_quality picks one; a broadcast keeps the one it started with.
static const struct {
    int w, h, fps;
} profiles[] = {{1280, 720, 30}, {1280, 720, 60}, {1920, 1080, 30}, {1920, 1080, 60}};
static _Atomic int profile;

void bc_profile(int *w, int *h, int *fps)
{
    *w = profiles[profile].w;
    *h = profiles[profile].h;
    *fps = profiles[profile].fps;
}

#ifndef __PROSPERO__
int bc_start(void)
{
    set_state("error", "Broadcasting needs the console.");
    return -1;
}

void bc_stop(void)
{
}

int bc_holds_controller(void)
{
    return 0;
}

void bc_rescue_controller(void)
{
}

void bc_request_keyframe(void)
{
}

const char *bc_controller(void)
{
    return "ok";
}

#else

#include <dlfcn.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chiaki/regist.h>
#include <chiaki/session.h>

#include "ds5_usb.h"
#include "voice.h"
#include "voice_audio.h"

static ChiakiSession session;
static _Atomic int want, running, connected, quit, ds5_done, live, holding;
static unsigned frames, audio_frames, keyframes;
// bc_start's test-and-set of running, and the worker's way out: two presses on two request
// threads must not both start a session on the one struct above.
static pthread_mutex_t run_lock = PTHREAD_MUTEX_INITIALIZER;
static int exiting; // the worker is past run(): a start now would find nothing to keep it
static unsigned starts; // bc_start calls: one during run()'s teardown runs it again

static void rp_log(ChiakiLogLevel level, const char *msg, void *user)
{
    (void)level;
    (void)user;
    LOG("remote play: %s", msg);
}

static void event_cb(ChiakiEvent *e, void *user)
{
    (void)user;
    if (e->type == CHIAKI_EVENT_CONNECTED)
        connected = 1;
    else if (e->type == CHIAKI_EVENT_QUIT) {
        LOG("broadcast: Remote Play ended (%s)", chiaki_quit_reason_string(e->quit.reason));
        quit = 1;
    } else if (e->type == CHIAKI_EVENT_LOGIN_PIN_REQUEST)
        quit = 1;
}

// One Annex-B access unit. Its first frame carries only SPS and PPS (types 7, 8).
static int has_idr(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++)
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && ((b[i + 3] & 0x1f) == 5 || (b[i + 3] & 0x1f) == 7))
            return 1;
    return 0;
}

static bool video_cb(uint8_t *buf, size_t size, int32_t frames_lost, bool recovered, void *user)
{
    (void)frames_lost;
    (void)recovered;
    (void)user;
    int key = has_idr(buf, size);
    frames++;
    keyframes += key;
    const bc_sink *s = sink;
    if (s && s->video)
        s->video(buf, size, key);
    return true;
}

static void audio_header_cb(ChiakiAudioHeader *h, void *user)
{
    (void)user;
    LOG("broadcast: game audio %u ch %u Hz, %u samples per frame", h->channels, h->rate, h->frame_size);
}

static void audio_frame_cb(uint8_t *buf, size_t size, void *user)
{
    (void)user;
    audio_frames++;
    const bc_sink *s = sink;
    if (s && s->audio)
        s->audio(buf, size);
}

// live changes under it: a keyframe request (a stream thread) never reaches a session
// being torn down.
static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;

void bc_request_keyframe(void)
{
    pthread_mutex_lock(&live_lock);
    if (live)
        chiaki_session_request_idr(&session);
    pthread_mutex_unlock(&live_lock);
}

// DualSense USB report 0x01 to chiaki's controller state.
static void to_chiaki(const uint8_t *r, ChiakiControllerState *st)
{
    static const uint32_t dpad[8] = {
        CHIAKI_CONTROLLER_BUTTON_DPAD_UP,
        CHIAKI_CONTROLLER_BUTTON_DPAD_UP | CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT,
        CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT,
        CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT | CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN,
        CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN,
        CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN | CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT,
        CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT,
        CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT | CHIAKI_CONTROLLER_BUTTON_DPAD_UP,
    };
    static const struct {
        uint8_t byte, bit;
        uint32_t button;
    } map[] = {
        {8, 0x10, CHIAKI_CONTROLLER_BUTTON_BOX},   {8, 0x20, CHIAKI_CONTROLLER_BUTTON_CROSS},
        {8, 0x40, CHIAKI_CONTROLLER_BUTTON_MOON},  {8, 0x80, CHIAKI_CONTROLLER_BUTTON_PYRAMID},
        {9, 0x01, CHIAKI_CONTROLLER_BUTTON_L1},    {9, 0x02, CHIAKI_CONTROLLER_BUTTON_R1},
        {9, 0x10, CHIAKI_CONTROLLER_BUTTON_SHARE}, {9, 0x20, CHIAKI_CONTROLLER_BUTTON_OPTIONS},
        {9, 0x40, CHIAKI_CONTROLLER_BUTTON_L3},    {9, 0x80, CHIAKI_CONTROLLER_BUTTON_R3},
        {10, 0x01, CHIAKI_CONTROLLER_BUTTON_PS},   {10, 0x02, CHIAKI_CONTROLLER_BUTTON_TOUCHPAD},
    };
    chiaki_controller_state_set_idle(st);
    if ((r[8] & 0x0f) < 8)
        st->buttons |= dpad[r[8] & 0x0f];
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
        if (r[map[i].byte] & map[i].bit)
            st->buttons |= map[i].button;
    st->left_x = (int16_t)((r[1] - 128) * 256);
    st->left_y = (int16_t)((r[2] - 128) * 256);
    st->right_x = (int16_t)((r[3] - 128) * 256);
    st->right_y = (int16_t)((r[4] - 128) * 256);
    st->l2_state = r[5];
    st->r2_state = r[6];
}

// ---- the USB controller (callbacks run on its thread and must not block) ---------

static void usb_log(void *user, const char *message)
{
    (void)user;
    LOG("broadcast: %s", message);
}

static void usb_controller(void *user, const ds5_usb_controller *decoded, const uint8_t report[64])
{
    (void)user;
    (void)decoded;
    ChiakiControllerState st;
    to_chiaki(report, &st);
    chiaki_session_set_controller_state(&session, &st);
}

static void usb_mic(void *user, const int16_t *mono, size_t n, int muted)
{
    (void)user;
    (void)muted; // already silence while the mute button is on
    vau_ext_mic(mono, n);
}

static size_t usb_playback(void *user, int16_t *stereo, size_t n)
{
    (void)user;
    return vau_ext_playback(stereo, n);
}

static int usb_service(void *user)
{
    (void)user;
    return want && !quit ? 0 : 1;
}

static void *usb_main(void *arg)
{
    (void)arg;
    ds5_usb_config config;
    ds5_usb_default_config(&config);
    config.duration_ms = 0; // until the broadcast ends
    const ds5_usb_callbacks callbacks = {.log = usb_log, .controller = usb_controller, .microphone = usb_mic,
                                         .playback = usb_playback, .service = usb_service};
    ds5_usb_stats stats = {0};
    int r = ds5_usb_run(&config, &callbacks, NULL, &stats);
    LOG("broadcast: controller released (%d): mic %llu frames, played %llu, %llu reports", r,
        (unsigned long long)stats.microphone_frames, (unsigned long long)stats.playback_frames,
        (unsigned long long)stats.controller_reports);
    ds5_done = 1;
    return NULL;
}

// ---- the session -------------------------------------------------------------------

static const char *run(void)
{
    ChiakiRegisteredHost host;
    uint64_t id = 0;
    int mine = 0; // Go Live as the user on screen: the USB controller plays through the session
    if (rp_load(&host, sizeof host, &id, &mine) != 0)
        return "Remote Play is not paired with this console.";

    if (!rp_chiaki_ready())
        return "Remote Play could not start.";

    static ChiakiLog log;
    chiaki_log_init(&log, CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING, rp_log, NULL);
    ChiakiConnectInfo info;
    memset(&info, 0, sizeof info);
    info.ps5 = true;
    info.host = "127.0.0.1";
    memcpy(info.regist_key, host.rp_regist_key, sizeof info.regist_key);
    memcpy(info.morning, host.rp_key, sizeof info.morning);
    // Remote Play's presets stop at 1080p; Discord's viewers need H.264.
    chiaki_connect_video_profile_preset(&info.video_profile,
                                        profiles[profile].h == 1080 ? CHIAKI_VIDEO_RESOLUTION_PRESET_1080p
                                                                    : CHIAKI_VIDEO_RESOLUTION_PRESET_720p,
                                        profiles[profile].fps == 60 ? CHIAKI_VIDEO_FPS_PRESET_60
                                                                    : CHIAKI_VIDEO_FPS_PRESET_30);
    info.video_profile.codec = CHIAKI_CODEC_H264;
    info.packet_loss_max = 0.05;
    info.enable_idr_on_fec_failure = true;
    for (int i = 0; i < 8; i++)
        info.psn_account_id[i] = (uint8_t)(id >> (8 * i));

    connected = quit = ds5_done = 0;
    frames = audio_frames = keyframes = 0;
    if (chiaki_session_init(&session, &info, &log) != CHIAKI_ERR_SUCCESS)
        return "Remote Play could not start.";
    chiaki_session_set_event_cb(&session, event_cb, NULL);
    chiaki_session_set_video_sample_cb(&session, video_cb, NULL);
    ChiakiAudioSink audio = {NULL, audio_header_cb, audio_frame_cb};
    chiaki_session_set_audio_sink(&session, &audio);
    const char *why = NULL;
    const int started = chiaki_session_start(&session) == CHIAKI_ERR_SUCCESS;
    if (!started)
        why = "Remote Play could not start.";
    for (int i = 0; !why && !connected && !quit && want && i < 200; i++) // 20 s
        usleep(100000);
    if (!why && !connected)
        why = quit ? "Remote Play refused the session." : want ? "Remote Play did not answer." : NULL;

    pthread_t usb;
    int usb_running = 0;
    if (!why && connected) {
        if (mine && want) { // not when Go Live was stopped (or the core quit) meanwhile
            LOG("broadcast: Remote Play connected, taking the controller");
            vau_ext_set(1);
            holding = 1;
            usb_running = pthread_create(&usb, NULL, usb_main, NULL) == 0;
            holding = usb_running;
        } else // another console user's session: the controller stays the user's own
            LOG("broadcast: Remote Play connected as another console user; the controller stays yours");
        pthread_mutex_lock(&live_lock);
        live = 1;
        pthread_mutex_unlock(&live_lock);
        set_state("live", "");
        unsigned tick = 0;
        while (want && !quit && !(usb_running && ds5_done) && voice_wanted()) {
            usleep(200000);
            if (++tick % 25 == 0)
                LOG("broadcast: %u video frames (%u keyframes), %u audio frames", frames, keyframes, audio_frames);
        }
        pthread_mutex_lock(&live_lock);
        live = 0;
        pthread_mutex_unlock(&live_lock);
        if (usb_running && ds5_done && want && !quit)
            why = "Plug the controller in by USB to broadcast.";
        else if (quit && want)
            why = "Remote Play ended.";
    }
    if (usb_running) {
        // The hand-back runs on the USB thread; a wedged ioctl must not keep the pad (and
        // Quit) waiting for ever: after 10 s re-enumerate it from here.
        ds5_usb_request_stop();
        for (int i = 0; i < 100 && !ds5_done; i++)
            usleep(100000);
        if (!ds5_done) {
            LOG("broadcast: the controller's USB thread is stuck, re-enumerating it");
            ds5_usb_rescue();
            for (int i = 0; i < 20 && !ds5_done; i++)
                usleep(100000);
        }
        if (ds5_done)
            pthread_join(usb, NULL);
        else {
            pthread_detach(usb);
            LOG("broadcast: the USB thread is still stuck; Go Live cannot take the controller again "
                "until the core restarts");
        }
        holding = 0;
    }
    vau_ext_set(0);
    if (started) { // its thread exists only then
        chiaki_session_stop(&session);
        chiaki_session_join(&session);
    }
    chiaki_session_fini(&session);
    return why;
}

static void *worker(void *arg)
{
    (void)arg;
    const char *why;
    for (;;) {
        pthread_mutex_lock(&run_lock);
        const unsigned seen = starts;
        pthread_mutex_unlock(&run_lock);
        why = run();
        pthread_mutex_lock(&run_lock);
        // Stopped, then started again while run() was handing everything back: it saw the
        // stop, so it runs once more for the start (which was told yes).
        const int again = !why && want && starts != seen && voice_wanted();
        if (!again)
            exiting = 1;
        else
            profile = prefs_get("golive_quality");
        pthread_mutex_unlock(&run_lock);
        if (!again)
            break;
        LOG("broadcast: started again while stopping");
        set_state("starting", "");
    }
    LOG("broadcast: off%s%s", why ? ": " : "", why ? why : "");
    set_state(why ? "error" : "off", why ? why : "");
    if (why) {
        stream_stop(); // the picture is gone: no frozen stream left in the call
        app_alert("Go Live stopped", why, NULL, 1); // the game is in front: say so there
    }
    pthread_mutex_lock(&run_lock);
    want = 0;
    running = 0;
    exiting = 0;
    pthread_mutex_unlock(&run_lock);
    return NULL;
}

int bc_start(void)
{
    if (!voice_wanted()) {
        set_state("error", "Join a call first.");
        return -1;
    }
    if (rp_used() < 0) {
        set_state("error", "Remote Play is not paired with this console.");
        return -1;
    }
    pthread_mutex_lock(&run_lock);
    if (running && exiting) {
        pthread_mutex_unlock(&run_lock);
        set_state("error", "Go Live is stopping, try again.");
        return -1;
    }
    want = 1;
    starts++;
    if (running) {
        pthread_mutex_unlock(&run_lock);
        return 0;
    }
    running = 1;
    profile = prefs_get("golive_quality");
    set_state("starting", "");
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int failed = pthread_create(&t, &attr, worker, NULL) != 0;
    pthread_attr_destroy(&attr);
    if (failed)
        want = running = 0;
    pthread_mutex_unlock(&run_lock);
    if (failed) {
        set_state("error", "Broadcasting could not start.");
        return -1;
    }
    return 0;
}

void bc_stop(void)
{
    want = 0;
}

int bc_holds_controller(void)
{
    return holding;
}

void bc_rescue_controller(void)
{
    ds5_usb_rescue();
}

const char *bc_controller(void)
{
    static const char *last = "ok";
    static time_t at;
    static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
    // Only Go Live as the user on screen takes the controller, and not while it runs.
    if (strcmp(bc_state(), "off") != 0 && strcmp(bc_state(), "error") != 0)
        return last;
    if (rp_used() < 0 || rp_used() != rp_foreground())
        return "ok";
    pthread_mutex_lock(&once);
    if (time(NULL) - at >= 2) { // the app asks every poll; a scan opens USB devices
        static const char *const names[] = {"ok", "replug", "unknown"};
        last = names[ds5_usb_replug_state()];
        at = time(NULL);
    }
    const char *r = last;
    pthread_mutex_unlock(&once);
    return r;
}
#endif
