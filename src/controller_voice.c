// SPDX-License-Identifier: AGPL-3.0-or-later
// Controller voice (controller_voice.h). The order matters: the core takes the USB
// DualSense from the system only once the Remote Play session is connected (the
// session's virtual pad is then the user's controller), and gives it back as soon as
// the session ends, so the user is never left without a controller. After that the
// user presses PS to reconnect it to the console.
//
// Remote Play: vendored chiaki-ng (third_party/README.md), paired with the console
// itself (remoteplay.bin = ChiakiRegisteredHost). USB: tools/ds5-usb-bridge/ds5_usb.c.
#include "controller_voice.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char state[16] = "off", error[160];
static char pair_file[256];

static void set_state(const char *s, const char *why)
{
    pthread_mutex_lock(&lock);
    snprintf(state, sizeof state, "%s", s);
    if (why)
        snprintf(error, sizeof error, "%s", why);
    pthread_mutex_unlock(&lock);
}

const char *cv_state(void)
{
    return state;
}

const char *cv_error(void)
{
    return error;
}

void cv_init(const char *data_dir)
{
    snprintf(pair_file, sizeof pair_file, "%s/remoteplay.bin", data_dir);
}

#ifndef __PROSPERO__
int cv_set(int on)
{
    if (on)
        set_state("error", "Controller voice needs the console.");
    return on ? -1 : 0;
}
#else

#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>

#include <chiaki/regist.h>
#include <chiaki/session.h>

#include "ds5_usb.h"
#include "voice.h"
#include "voice_audio.h"

static ChiakiSession session;
static volatile int want, running, connected, quit, ds5_done;

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
        LOG("controller voice: Remote Play ended (%s)", chiaki_quit_reason_string(e->quit.reason));
        quit = 1;
    } else if (e->type == CHIAKI_EVENT_LOGIN_PIN_REQUEST)
        quit = 1;
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

// ---- the USB device (callbacks run on its thread and must not block) -------------

static void usb_log(void *user, const char *message)
{
    (void)user;
    LOG("controller voice: %s", message);
}

static void usb_controller(void *user, const ds5_usb_controller *decoded, const uint8_t report[64])
{
    (void)user;
    (void)decoded;
    ChiakiControllerState st;
    to_chiaki(report, &st);
    chiaki_session_set_controller_state(&session, &st);
}

static void usb_mic(void *user, const int16_t *mono, size_t frames, int muted)
{
    (void)user;
    (void)muted; // already silence while the mute button is on
    vau_ext_mic(mono, frames);
}

static size_t usb_playback(void *user, int16_t *stereo, size_t frames)
{
    (void)user;
    return vau_ext_playback(stereo, frames);
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
    config.duration_ms = 0; // until the session or the call ends
    const ds5_usb_callbacks callbacks = {.log = usb_log, .controller = usb_controller, .microphone = usb_mic,
                                         .playback = usb_playback, .service = usb_service};
    ds5_usb_stats stats = {0};
    int r = ds5_usb_run(&config, &callbacks, NULL, &stats);
    LOG("controller voice: USB released (%d): mic %llu frames, played %llu, %llu controller reports", r,
        (unsigned long long)stats.microphone_frames, (unsigned long long)stats.playback_frames,
        (unsigned long long)stats.controller_reports);
    ds5_done = 1;
    return NULL;
}

// ---- the session -------------------------------------------------------------------

static uint64_t account_id(void)
{
    int (*fg)(int32_t *) = NULL;
    int (*acct)(int32_t, uint64_t *) = NULL;
    void *lib = dlopen("libSceUserService.sprx", RTLD_LAZY);
    if (lib) {
        fg = (int (*)(int32_t *))dlsym(lib, "sceUserServiceGetForegroundUser");
        acct = (int (*)(int32_t, uint64_t *))dlsym(lib, "sceUserServiceGetNpAccountId");
    }
    int32_t user = -1;
    uint64_t id = 0;
    if (fg && acct && fg(&user) == 0)
        acct(user, &id);
    return id;
}

static const char *run(void)
{
    ChiakiRegisteredHost host;
    FILE *f = fopen(pair_file, "rb");
    size_t got = f ? fread(&host, sizeof host, 1, f) : 0;
    if (f)
        fclose(f);
    if (got != 1)
        return "Remote Play is not paired with this console.";

    static int lib_ready;
    if (!lib_ready && chiaki_lib_init() != CHIAKI_ERR_SUCCESS)
        return "Remote Play could not start.";
    lib_ready = 1;

    static ChiakiLog log;
    chiaki_log_init(&log, CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING, rp_log, NULL);
    ChiakiConnectInfo info;
    memset(&info, 0, sizeof info);
    info.ps5 = true;
    info.host = "127.0.0.1";
    memcpy(info.regist_key, host.rp_regist_key, sizeof info.regist_key);
    memcpy(info.morning, host.rp_key, sizeof info.morning);
    chiaki_connect_video_profile_preset(&info.video_profile, CHIAKI_VIDEO_RESOLUTION_PRESET_720p,
                                        CHIAKI_VIDEO_FPS_PRESET_30);
    info.video_profile.codec = CHIAKI_CODEC_H264;
    // ponytail: input only for now; Go Live turns video and audio back on.
    info.audio_video_disabled = CHIAKI_AUDIO_DISABLED | CHIAKI_VIDEO_DISABLED;
    info.packet_loss_max = 0.05;
    uint64_t id = account_id();
    for (int i = 0; i < 8; i++)
        info.psn_account_id[i] = (uint8_t)(id >> (8 * i));

    connected = quit = ds5_done = 0;
    if (chiaki_session_init(&session, &info, &log) != CHIAKI_ERR_SUCCESS)
        return "Remote Play could not start.";
    chiaki_session_set_event_cb(&session, event_cb, NULL);
    const char *why = NULL;
    if (chiaki_session_start(&session) != CHIAKI_ERR_SUCCESS)
        why = "Remote Play could not start.";
    for (int i = 0; !why && !connected && !quit && want && i < 200; i++) // 20 s
        usleep(100000);
    if (!why && !connected)
        why = quit ? "Remote Play refused the session." : want ? "Remote Play did not answer." : NULL;

    pthread_t usb;
    int usb_running = 0;
    if (!why && connected) {
        LOG("controller voice: Remote Play connected, taking the controller");
        vau_ext_set(1);
        usb_running = pthread_create(&usb, NULL, usb_main, NULL) == 0;
        set_state("on", "");
        while (want && !quit && !ds5_done && voice_wanted())
            usleep(200000);
        if (ds5_done && want && !quit)
            why = "The controller is not connected by USB.";
        else if (quit && want)
            why = "Remote Play ended.";
    }
    if (usb_running) {
        ds5_usb_request_stop();
        pthread_join(usb, NULL);
    }
    vau_ext_set(0);
    chiaki_session_stop(&session);
    chiaki_session_join(&session);
    chiaki_session_fini(&session);
    return why;
}

static void *worker(void *arg)
{
    (void)arg;
    const char *why = run();
    LOG("controller voice: off%s%s", why ? ": " : "", why ? why : "");
    set_state(why ? "error" : "off", why ? why : "");
    want = 0;
    running = 0;
    return NULL;
}

int cv_set(int on)
{
    if (!on) {
        want = 0;
        return 0;
    }
    if (!voice_wanted()) {
        set_state("error", "Join a call first.");
        return -1;
    }
    if (access(pair_file, R_OK) != 0) {
        set_state("error", "Remote Play is not paired with this console.");
        return -1;
    }
    want = 1;
    if (running)
        return 0;
    running = 1;
    set_state("starting", "");
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, worker, NULL) != 0) {
        running = 0;
        set_state("error", "Controller voice could not start.");
        return -1;
    }
    return 0;
}
#endif
