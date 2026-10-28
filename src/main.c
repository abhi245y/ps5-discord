// ps5-discord core: background payload holding the Discord gateway session.
// PoC: logs in by QR code (printed to klog), keeps the token in a file,
// logs session state to klog.
#include <arpa/inet.h>
#include <ctype.h>
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
// ponytail: plaintext token on the console's data partition; encrypt at rest before release.
#define TOKEN_PATH DATA_DIR "/token"
// Control port: keeps one instance alive and serves the status web UI
// (http://<ps5>:9041, POST /api/quit for make stop). Future UI app IPC goes here too.
#define CONTROL_PORT 9041

static int read_token(char *out, size_t cap)
{
    FILE *f = fopen(TOKEN_PATH, "r");
    if (!f)
        return -1;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    while (n > 0 && isspace((unsigned char)out[n - 1]))
        n--;
    out[n] = 0;
    return n > 0 ? 0 : -1;
}

static void login(char *token, size_t cap)
{
    browser_auth_allow(1);
    for (int backoff = 2;;) {
        int browser = browser_auth_pending();
        int result = browser ? browser_auth_login(token, cap) : remote_auth_login(token, cap);
        if (result == 0) {
            browser_auth_allow(0);
            if (session_save(DATA_DIR, token) != 0)
                LOG("could not persist session; login will be required on next launch");
            return;
        }
        status_state("login");
        // A browser attempt ended (cancelled, timed out) or is starting: no wait for the next step.
        if (browser || browser_auth_pending()) {
            backoff = 2;
            continue;
        }
        LOG("login failed, new QR in %d s", backoff);
        for (int i = 0; i < backoff * 10 && !browser_auth_pending(); ++i)
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
void core_quit(int forget_token)
{
    stream_stop();
    bc_stop();
    for (int i = 0; i < 100 && strcmp(bc_state(), "off") != 0 && strcmp(bc_state(), "error") != 0; i++)
        usleep(100000);
    voice_leave();
    if (forget_token)
        remove(TOKEN_PATH);
    _exit(0);
}

// A message worth an alert (chat.c). With the app in front it shows its own banner;
// otherwise (home screen, a game, the PS menu, or closed with the PS button) the PS5's.
static void alert(const char *title, const char *body, const char *icon)
{
    if (web_app_on_screen() || !prefs_get("notify_system"))
        return;
    const char *b = bc_state();
    if ((strcmp(b, "live") == 0 || strcmp(b, "starting") == 0) && !prefs_get("notify_live"))
        return; // the stream shows the screen: viewers would read it
    notify_message(title, body, icon);
}

static int claim_instance(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(CONTROL_PORT)};
    // ponytail: open to the LAN for the PoC web UI; no auth, shows no secrets.
    // Loopback-only (or authenticated) once it carries app IPC.
    a.sin_addr.s_addr = htonl(INADDR_ANY);
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
    web_serve((int)(intptr_t)arg);
    return NULL;
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
    char token[256];
    int control = claim_instance();
    if (control < 0) {
        LOG("already running");
        return 1;
    }
    pthread_t t;
    pthread_create(&t, NULL, control_loop, (void *)(intptr_t)control);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    voice_init(DATA_DIR);
    cv_init();
    rp_init(DATA_DIR);
    prefs_init(DATA_DIR);
    chat_set_alert(alert);
    stream_init();
    if (tls_global_init() != 0) {
        LOG("tls init failed");
        return 1;
    }
    LOG("starting");
    for (;;) {
        if (read_token(token, sizeof token) != 0)
            login(token, sizeof token);
        chat_set_token(token);
        int code = gateway_run(token);
        if (code != 4004) {
            status_state("stopped");
            LOG("stopped (%d)", code);
            pause(); // keep the web UI up so the failure is visible; quit from there
        }
        LOG("token rejected, logging in again");
        remove(TOKEN_PATH);
    }
}
