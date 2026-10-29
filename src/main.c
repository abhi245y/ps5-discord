// ps5-discord core: background payload holding the Discord gateway session.
// PoC: logs in by QR code (printed to klog), keeps the token in a file,
// logs session state to klog.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "chat.h"
#include "gateway.h"
#include "log.h"
#include "remote_auth.h"
#include "browser_auth.h"
#include "build_config.h"
#include "browser_storage.h"
#include "status.h"
#include "web.h"
#include "voice.h"
#include "controller_voice.h"
#include "broadcast.h"
#include "http.h"
#include "notify.h"
#include "prefs.h"
#include "rp_pair.h"
#include "stream.h"
#include "tls.h"

#ifdef __PROSPERO__
#define DATA_DIR "/data/ps5-discord"
int sceKernelSetProcessName(const char *name);
#else
#define DATA_DIR "."
#endif
// The session is kept encrypted to this console (browser_storage.c session_save).
// One per console user (token-<user>): each signs in to their own Discord account, and the
// core runs the account of the user on screen.
static char token_name[32] = "token";
static volatile int32_t session_user = -1;

static void token_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", DATA_DIR, token_name);
}
// Control port: keeps one instance alive and serves the status web UI
// (http://<ps5>:9041, POST /api/quit for make stop). Future UI app IPC goes here too.
#define CONTROL_PORT 9041

static int read_token(char *out, size_t cap)
{
    const int r = session_load(DATA_DIR, token_name, out, cap);
    if (r == 1 && session_save(DATA_DIR, token_name, out) == 0) // an older build's, in the clear
        LOG("session: encrypted the saved sign-in");
    return r >= 0 ? 0 : -1;
}

// 0 when signed in; -1 when another console user came on screen first.
static int login(char *token, size_t cap)
{
    browser_auth_allow(1);
    for (int backoff = 2;;) {
        if (gateway_switching())
            return -1;
        int browser = browser_auth_pending();
        int result = browser ? browser_auth_login(token, cap) : remote_auth_login(token, cap);
        if (result == 0 && gateway_switching())
            return -1; // signed in on the page of a user who left: not theirs to keep
        if (result == 0) {
            browser_auth_allow(0);
            if (session_save(DATA_DIR, token_name, token) != 0)
                LOG("could not persist session; login will be required on next launch");
            return 0;
        }
        status_state("login");
        // A browser attempt ended (cancelled, timed out) or is starting: no wait for the next step.
        if (browser || browser_auth_pending()) {
            backoff = 2;
            continue;
        }
        LOG("login failed, new QR in %d s", backoff);
        for (int i = 0; i < backoff * 10 && !browser_auth_pending() && !gateway_switching(); ++i)
            usleep(100000);
        backoff = backoff < 30 ? backoff * 2 : 30;
    }
}

// POST /api/signout (web.c). Discord ends the session first, so the token is dead even
// if a copy was taken; only then does the console forget it. -1 when Discord could not
// be told: the session stays, so the sign-in is never left alive but unknown here.
int core_sign_out(void)
{
    http_response r = http_post_json("/auth/logout", "{\"provider\":null,\"voip_provider\":null}", chat_token());
    free(r.body);
    LOG("sign out: Discord answered %d", r.status);
    // 401: the token was dead already.
    return (r.status >= 200 && r.status < 300) || r.status == 401 ? 0 : -1;
}

// Quit (the app's Quit, or a sign out with forget_token): Go Live gives the USB
// controller back and the call ends first. After a sign out the app starts the core
// again, at the QR code.
// Go Live gives the USB controller back, then the call ends. Exiting while the controller
// is still detached from the system's drivers could leave it dead until a reboot, so the
// handback gets up to 30 s; Remote Play's teardown after it, the rest of 10 s.
static void end_calls(void)
{
    stream_stop();
    bc_stop();
    for (int i = 0; i < 300 && (bc_holds_controller() || (i < 100 && strcmp(bc_state(), "off") != 0 &&
                                                             strcmp(bc_state(), "error") != 0));
         i++)
        usleep(100000);
    if (bc_holds_controller())
        LOG("quit: the controller was not given back in 30 s");
    voice_leave();
}

void core_quit(int forget_token)
{
    end_calls();
    // The leave and the stream's end go out on the gateway thread (within a second): let
    // them, or the others see us in the call until Discord times us out.
    for (int i = 0; i < 20 && (voice_request_pending() || stream_gateway_pending()); i++)
        usleep(100000);
    if (forget_token) {
        char path[64];
        token_path(path, sizeof path);
        remove(path);
    }
    _exit(0);
}

// The account follows the console user on screen: the core takes theirs, its settings
// and notifications. The one sign-in and settings file of before go to the first user.
static void use_user(int32_t user)
{
    session_user = user;
    snprintf(token_name, sizeof token_name, "token-%08x", (unsigned)user);
    char path[64];
    token_path(path, sizeof path);
    FILE *f = fopen(path, "r"); // access() refuses payloads on the console
    if (f)
        fclose(f);
    else if (user >= 0) // the legacy sign-in is for a real user, not "nobody on screen"
        rename(DATA_DIR "/token", path);
    prefs_load(DATA_DIR, user);
    notify_set_owner(user);
    LOG("console user %08x: their Discord account", (unsigned)user);
}

// Another user on screen: the session ends within a second or two (gateway_switch).
static void *watch_user(void *arg)
{
    (void)arg;
    for (;;) {
        sleep(2);
        int32_t user = rp_foreground();
        if (user >= 0 && user != session_user && !gateway_switching()) {
            LOG("console user %08x came on screen: switching accounts", (unsigned)user);
            gateway_switch(1);
        }
    }
    return NULL;
}

// Any of the app's notifications (notify.h). With the app in front it shows its own;
// otherwise (home screen, a game, the PS menu, or closed with the PS button) the PS5's.
void app_alert(const char *context, const char *text, const char *icon, int urgent)
{
    if (web_app_on_screen() || !prefs_get("notify_system"))
        return;
    const char *b = bc_state();
    if ((strcmp(b, "live") == 0 || strcmp(b, "starting") == 0) && !prefs_get("notify_live"))
        return; // the stream shows the screen: viewers would read it
    if (urgent)
        notify_system(context, text, icon);
    else
        notify_message(context, text, icon);
}

static int claim_instance(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(CONTROL_PORT)};
    // The app on this console only; a development build (CORE_LAN) lets the LAN in too.
    a.sin_addr.s_addr = htonl(CORE_LAN ? INADDR_ANY : INADDR_LOOPBACK);
    int one = 1; // a just-stopped instance leaves TIME_WAIT behind; a live one still blocks bind
    if (fd >= 0)
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 16) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd; // kept open for the process lifetime
}

static void *control_loop(void *arg)
{
    int fd = (int)(intptr_t)arg;
    for (;;) {
        web_serve(fd);
        close(fd);
        // Our port again, or the app cannot reach us and starts another core. Another one
        // already has it: that is the app's core now, this one goes.
        for (int i = 0; i < 50 && (fd = claim_instance()) < 0; i++)
            usleep(100000);
        if (fd < 0) {
            LOG("control: another core has the port, quitting");
            core_quit(0);
        }
        LOG("control: listening again");
    }
    return NULL;
}

static void on_crash(int sig)
{
    bc_rescue_controller();
    signal(sig, SIG_DFL);
    raise(sig);
}

int main(void)
{
#ifdef __PROSPERO__
    // elfldr names every payload "payload.elf"; payload managers list only names ending in
    // .elf, and the name holds 19 characters at most.
    sceKernelSetProcessName("discord-core.elf");
#endif
    // A peer that hangs up mid-write (the app, a browser, Discord) must not kill the core.
    signal(SIGPIPE, SIG_IGN);
    // A crash while Go Live holds the controller would leave it dead until a reboot.
    for (int i = 0; i < 5; i++)
        signal((int[]){SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE}[i], on_crash);
    char token[256];
    int control = claim_instance();
    if (control < 0) {
        LOG("already running");
        return 1;
    }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    voice_init(DATA_DIR);
    cv_init();
    rp_init(DATA_DIR);
    chat_set_alert(app_alert);
    stream_init();
    if (tls_global_init() != 0) {
        LOG("tls init failed");
        return 1;
    }
    LOG("starting, version %s", CORE_VERSION);
    // Requests only once TLS, voice and Go Live are set up (the port is ours already).
    pthread_t t;
    pthread_create(&t, NULL, control_loop, (void *)(intptr_t)control);
    // Started before anyone signed in to the console (elfldr at boot): wait for them, or
    // their sign-in would be read from token-ffffffff. A minute, in case nobody ever shows.
#ifdef __PROSPERO__
    for (int i = 0; i < 60 && rp_foreground() < 0; i++)
        sleep(1);
#endif
    pthread_t watcher;
    if (pthread_create(&watcher, NULL, watch_user, NULL) == 0)
        pthread_detach(watcher);
    for (;;) {
        use_user(rp_foreground());
        gateway_switch(0);
        int code = GATEWAY_SWITCH;
        if (read_token(token, sizeof token) == 0 || login(token, sizeof token) == 0) {
            chat_set_token(token);
            code = gateway_run(token);
        }
        if (code == GATEWAY_SWITCH) { // the user on screen changed: their account next
            end_calls();
            // What end_calls queued for the old account's gateway is gone with it; on the next
            // account's it would end that account's stream or call.
            stream_forget_gateway();
            voice_op4 old;
            voice_take_request(&old);
            chat_reset();
            status_state("starting");
            continue;
        }
        end_calls(); // a live broadcast holds the controller: give it back before waiting
        if (code != 4004) {
            char note[128];
            snprintf(note, sizeof note, "Discord closed the session (code %d). Quit and start the app again.", code);
            status_note(note);
            status_state("stopped");
            LOG("stopped (%d)", code);
            pause(); // keep the control API up so the app can show why and quit
        }
        LOG("token rejected, logging in again");
        app_alert("Discord", "Signed out. Open the app to sign in again.", NULL, 1);
        char path[64];
        token_path(path, sizeof path);
        remove(path);
    }
}
