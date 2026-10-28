// SPDX-License-Identifier: AGPL-3.0-or-later
// Broadcast (broadcast.h). The order matters: the core takes the USB DualSense from
// the system only once the Remote Play session is connected (the session's virtual
// pad is then the user's controller), and gives it back as soon as the session ends,
// so the user is never left without a controller. Afterwards the user presses PS to
// reconnect it to the console.
//
// Remote Play: vendored chiaki-ng (third_party/README.md), paired with the console
// itself (remoteplay.bin = ChiakiRegisteredHost). USB: tools/ds5-usb-bridge/ds5_usb.c.
#include "broadcast.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char state[16] = "off", error[160];
static char pair_file[256];
static const bc_sink *volatile sink;

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

static char info_file[256];
static void set_pair_state(const char *s, const char *why);

void bc_init(const char *data_dir)
{
    snprintf(pair_file, sizeof pair_file, "%s/remoteplay.bin", data_dir);
    snprintf(info_file, sizeof info_file, "%s/remoteplay.txt", data_dir);
}

// remoteplay.txt: three lines, the console's name, the console user and the time it was made.
void bc_pair_info(char console[64], char user[64], long long *since)
{
    console[0] = user[0] = 0;
    *since = 0;
    FILE *f = fopen(info_file, "r");
    if (!f)
        return;
    char line[64];
    if (fgets(console, 64, f))
        console[strcspn(console, "\n")] = 0;
    if (fgets(user, 64, f))
        user[strcspn(user, "\n")] = 0;
    if (fgets(line, sizeof line, f))
        *since = atoll(line);
    fclose(f);
}

int bc_unpair(void)
{
    int r = remove(pair_file);
    remove(info_file);
    set_pair_state("idle", "");
    LOG("remote play: pairing removed");
    return r == 0 || !bc_paired() ? 0 : -1;
}

void bc_set_sink(const bc_sink *s)
{
    sink = s;
}

static char pair_state[16] = "idle", pair_error[160];

int bc_paired(void)
{
    FILE *f = fopen(pair_file, "rb"); // access() refuses payloads on the console
    if (f)
        fclose(f);
    return f != NULL;
}

const char *bc_pair_state(void)
{
    return pair_state;
}

const char *bc_pair_error(void)
{
    return pair_error;
}

static void set_pair(const char *s, const char *why)
{
    set_pair_state(s, why);
}

static void set_pair_state(const char *s, const char *why)
{
    pthread_mutex_lock(&lock);
    snprintf(pair_state, sizeof pair_state, "%s", s);
    snprintf(pair_error, sizeof pair_error, "%s", why ? why : "");
    pthread_mutex_unlock(&lock);
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

void bc_request_keyframe(void)
{
}

int bc_pair(const char *pin)
{
    (void)pin;
    set_pair("error", "Pairing needs the console.");
    return -1;
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
static volatile int want, running, connected, quit, ds5_done, live;
static unsigned frames, audio_frames, keyframes;

static int chiaki_ready(void)
{
    static int ok;
    if (!ok)
        ok = chiaki_lib_init() == CHIAKI_ERR_SUCCESS;
    return ok;
}

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

void bc_request_keyframe(void)
{
    if (live)
        chiaki_session_request_idr(&session);
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

int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(int32_t *);
int sceUserServiceGetUserName(int32_t, char *, size_t);

// The foreground console user's PSN account id (0 when it has none) and name. The core
// does not use the user service otherwise, so it starts it here (already-started is fine).
static uint64_t account_id_named(char *name, size_t cap)
{
    int init = sceUserServiceInitialize(NULL);
    int32_t user = -1;
    int fg = sceUserServiceGetForegroundUser(&user);
    if (name && cap) {
        name[0] = 0;
        if (fg == 0)
            sceUserServiceGetUserName(user, name, cap);
    }
    int (*acct)(int32_t, uint64_t *) = NULL;
    void *lib = dlopen("libSceUserService.sprx", RTLD_LAZY);
    if (lib)
        acct = (int (*)(int32_t, uint64_t *))dlsym(lib, "sceUserServiceGetNpAccountId");
    uint64_t id = 0;
    int ar = fg == 0 && acct ? acct(user, &id) : -1;
    LOG("remote play: user service %#x, foreground user %#x (%#x), account id %#x %s", (unsigned)init,
        (unsigned)user, (unsigned)fg, (unsigned)ar, id ? "present" : "missing");
    return id;
}

static uint64_t account_id(void)
{
    return account_id_named(NULL, 0);
}

// The console's own Remote Play service (what Settings > Remote Play > Link Device uses):
// a PIN made here opens the console for pairing, no screen needed. 0 when it refuses.
static int (*rp_notify_pin_error)(int);

static uint32_t system_pin(void)
{
    void *lib = dlopen("libSceRemoteplay.sprx", RTLD_LAZY);
    int (*init)(void *, size_t) = lib ? (int (*)(void *, size_t))dlsym(lib, "sceRemoteplayInitialize") : NULL;
    int (*gen)(uint32_t *) = lib ? (int (*)(uint32_t *))dlsym(lib, "sceRemoteplayGeneratePinCode") : NULL;
    rp_notify_pin_error = lib ? (int (*)(int))dlsym(lib, "sceRemoteplayNotifyPinCodeError") : NULL;
    if (!init || !gen) {
        LOG("remote play: the console's pairing service is not reachable (%s)", lib ? "symbols" : "library");
        return 0;
    }
    int ir = init(NULL, 0);
    uint32_t pin = 0;
    int gr = gen(&pin);
    LOG("remote play: console pairing service init %#x, PIN %s (%#x)", (unsigned)ir, gr == 0 && pin ? "made" : "refused",
        (unsigned)gr);
    return gr == 0 ? pin : 0;
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

    if (!chiaki_ready())
        return "Remote Play could not start.";

    static ChiakiLog log;
    chiaki_log_init(&log, CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING, rp_log, NULL);
    ChiakiConnectInfo info;
    memset(&info, 0, sizeof info);
    info.ps5 = true;
    info.host = "127.0.0.1";
    memcpy(info.regist_key, host.rp_regist_key, sizeof info.regist_key);
    memcpy(info.morning, host.rp_key, sizeof info.morning);
    // ponytail: 720p30 to start; a quality setting comes with the viewers' feedback.
    chiaki_connect_video_profile_preset(&info.video_profile, CHIAKI_VIDEO_RESOLUTION_PRESET_720p,
                                        CHIAKI_VIDEO_FPS_PRESET_30);
    info.video_profile.codec = CHIAKI_CODEC_H264;
    info.packet_loss_max = 0.05;
    info.enable_idr_on_fec_failure = true;
    uint64_t id = account_id();
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
    if (chiaki_session_start(&session) != CHIAKI_ERR_SUCCESS)
        why = "Remote Play could not start.";
    for (int i = 0; !why && !connected && !quit && want && i < 200; i++) // 20 s
        usleep(100000);
    if (!why && !connected)
        why = quit ? "Remote Play refused the session." : want ? "Remote Play did not answer." : NULL;

    pthread_t usb;
    int usb_running = 0;
    if (!why && connected) {
        LOG("broadcast: Remote Play connected, taking the controller");
        vau_ext_set(1);
        usb_running = pthread_create(&usb, NULL, usb_main, NULL) == 0;
        live = 1;
        set_state("live", "");
        unsigned tick = 0;
        while (want && !quit && !ds5_done && voice_wanted()) {
            usleep(200000);
            if (++tick % 25 == 0)
                LOG("broadcast: %u video frames (%u keyframes), %u audio frames", frames, keyframes, audio_frames);
        }
        live = 0;
        if (ds5_done && want && !quit)
            why = "Plug the controller in by USB to broadcast.";
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
    LOG("broadcast: off%s%s", why ? ": " : "", why ? why : "");
    set_state(why ? "error" : "off", why ? why : "");
    want = 0;
    running = 0;
    return NULL;
}

// ---- pairing ----------------------------------------------------------------------

static volatile int regist_done;
static ChiakiRegisteredHost regist_host;

static void regist_cb(ChiakiRegistEvent *e, void *user)
{
    (void)user;
    if (e->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS)
        regist_host = *e->registered_host;
    regist_done = e->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS ? 1 : 2;
}

static void *pair_main(void *arg)
{
    uint32_t pin = (uint32_t)(uintptr_t)arg;
    const char *why = NULL;
    int made_pin = pin == 0;
    if (made_pin && !(pin = system_pin())) {
        set_pair("need_pin", "Open Settings > System > Remote Play > Link Device on the PS5 and enter its PIN.");
        return NULL;
    }
    char user[64];
    uint64_t id = account_id_named(user, sizeof user);
    static ChiakiLog log;
    chiaki_log_init(&log, CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING, rp_log, NULL);
    ChiakiRegistInfo info;
    memset(&info, 0, sizeof info);
    info.target = CHIAKI_TARGET_PS5_1;
    info.host = "127.0.0.1";
    info.pin = pin;
    for (int i = 0; i < 8; i++) // little-endian, as Sony's account id is sent
        info.psn_account_id[i] = (uint8_t)(id >> (8 * i));
    ChiakiRegist regist;
    regist_done = 0;
    if (!id)
        why = "The console user has no account id (sign in to PSN once on this user).";
    else if (chiaki_regist_start(&regist, &log, &info, regist_cb, NULL) != CHIAKI_ERR_SUCCESS)
        why = "Pairing could not start.";
    else {
        for (int i = 0; i < 300 && !regist_done; i++) // 30 s
            usleep(100000);
        if (!regist_done)
            chiaki_regist_stop(&regist);
        chiaki_regist_fini(&regist);
        if (regist_done != 1)
            why = regist_done ? "The console refused the PIN. Open Link Device again and use the new PIN."
                              : "The console did not answer. Is Link Device open?";
    }
    if (made_pin && rp_notify_pin_error)
        rp_notify_pin_error(1); // close the console's pairing window again
    if (!why) { // write it whole, then move it into place
        char tmp[300];
        snprintf(tmp, sizeof tmp, "%s.new", pair_file);
        FILE *f = fopen(tmp, "wb");
        int ok = f && fwrite(&regist_host, sizeof regist_host, 1, f) == 1;
        if (f)
            ok &= fclose(f) == 0;
        if (ok) {
            chmod(tmp, 0600);
            ok = rename(tmp, pair_file) == 0;
        }
        FILE *info = ok ? fopen(info_file, "w") : NULL;
        if (info) {
            fprintf(info, "%.63s\n%.63s\n%lld\n", regist_host.server_nickname, user, (long long)time(NULL));
            fclose(info);
        }
        if (!ok)
            why = "Could not save the pairing.";
        memset(&regist_host, 0, sizeof regist_host); // the keys stay only in the file
    }
    LOG("remote play: pairing %s%s%s", why ? "failed: " : "done", why ? why : "", "");
    set_pair(why ? "error" : "paired", why);
    return NULL;
}

int bc_pair(const char *pin)
{
    char *end = NULL;
    unsigned long n = pin && *pin ? strtoul(pin, &end, 10) : 0;
    if (pin && *pin && (strlen(pin) != 8 || *end)) {
        set_pair("error", "The PIN has 8 digits.");
        return -1;
    }
    pthread_mutex_lock(&lock);
    int busy = strcmp(pair_state, "pairing") == 0;
    if (!busy)
        snprintf(pair_state, sizeof pair_state, "pairing"), pair_error[0] = 0;
    pthread_mutex_unlock(&lock);
    if (busy)
        return 0;
    if (!chiaki_ready()) {
        set_pair("error", "Remote Play could not start.");
        return -1;
    }
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, pair_main, (void *)(uintptr_t)n) != 0) {
        set_pair("error", "Pairing could not start.");
        return -1;
    }
    return 0;
}

int bc_start(void)
{
    if (!voice_wanted()) {
        set_state("error", "Join a call first.");
        return -1;
    }
    FILE *f = fopen(pair_file, "rb"); // access() refuses payloads on the console
    if (!f) {
        set_state("error", "Remote Play is not paired with this console.");
        return -1;
    }
    fclose(f);
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
        set_state("error", "Broadcasting could not start.");
        return -1;
    }
    return 0;
}

void bc_stop(void)
{
    want = 0;
}
#endif
