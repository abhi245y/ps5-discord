#include "browser_auth.h"
#include "browser_storage.h"
#include "http.h"
#include "log.h"
#include "status.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <cJSON.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int allowed, pending, active, cancelled, reopen;
static time_t finished;

void browser_auth_allow(int value)
{
    pthread_mutex_lock(&lock);
    allowed = value;
    if (!value) pending = 0;
    pthread_mutex_unlock(&lock);
}

int browser_auth_request(void)
{
    pthread_mutex_lock(&lock);
    int ok = allowed && !pending;
    if (ok && active) reopen = 1; // the person closed the browser before signing in
    else if (ok) { pending = 1; cancelled = 0; }
    pthread_mutex_unlock(&lock);
    return ok ? 0 : -1;
}

void browser_auth_cancel(void)
{
    pthread_mutex_lock(&lock);
    cancelled = 1;
    pending = 0;
    pthread_mutex_unlock(&lock);
}

int browser_auth_pending(void)
{
    pthread_mutex_lock(&lock);
    int value = pending;
    pthread_mutex_unlock(&lock);
    return value;
}

int browser_auth_keepalive(void)
{
    pthread_mutex_lock(&lock);
    int value = active || pending || (finished && time(NULL) - finished < 60);
    pthread_mutex_unlock(&lock);
    return value;
}

static int is_cancelled(void)
{
    pthread_mutex_lock(&lock);
    int value = cancelled;
    pthread_mutex_unlock(&lock);
    return value;
}

static int take_reopen(void)
{
    pthread_mutex_lock(&lock);
    int value = reopen;
    reopen = 0;
    pthread_mutex_unlock(&lock);
    return value;
}

#ifdef __PROSPERO__
int sceUserServiceInitialize(void *);
int sceUserServiceTerminate(void);
int sceUserServiceGetForegroundUser(int *);
int sceSystemServiceLaunchWebBrowser(const char *, void *);

#define USER_SERVICE_ALREADY_INITIALIZED 0x80960003 // PS4 SDK numbering

// Opens Discord in the browser for the foreground user, whose id it stores.
static int launch(int *user)
{
    int init = sceUserServiceInitialize(NULL);
    if (init != 0 && (unsigned)init != USER_SERVICE_ALREADY_INITIALIZED) {
        LOG("browser login: UserService init failed (%x)", (unsigned)init);
        return -1;
    }
    int user_result = sceUserServiceGetForegroundUser(user);
    int launched = user_result == 0 && *user >= 0 ?
        sceSystemServiceLaunchWebBrowser("https://discord.com/login", NULL) : -1;
    if (init == 0) // someone else's initialisation stays theirs
        sceUserServiceTerminate();
    if (launched != 0)
        LOG("browser login: launch failed (%x), user lookup (%x)", (unsigned)launched, (unsigned)user_result);
    return launched == 0 ? 0 : -1;
}
#endif

int browser_auth_login(char *out, size_t cap)
{
    pthread_mutex_lock(&lock);
    if (!pending) { pthread_mutex_unlock(&lock); return -1; }
    pending = 0;
    active = 1;
    pthread_mutex_unlock(&lock);
    int result = -1;
    status_qr(NULL);
    status_state("browser");
    status_note("Opening Discord. Sign in, then close the browser to return here.");
#ifdef __PROSPERO__
    int user = -1;
    take_reopen();
    if (launch(&user) != 0) {
        status_note("Could not open Discord. Try browser login again.");
        goto done;
    }
    char path[256];
    snprintf(path, sizeof path, "/user/home/%08x/webkit/shell/local/https_discord.com_0.localstorage", (unsigned)user);
    char rejected[256] = "";
    time_t retry_after = 0;
    time_t deadline = time(NULL) + 600;
    // Existing browser sessions are eligible only after explicit user activation.
    // No fork/watchdog: the main login thread owns this bounded polling loop.
    while (time(NULL) < deadline && !is_cancelled()) {
        for (int i = 0; i < 20 && !is_cancelled(); ++i) usleep(100000);
        if (is_cancelled()) break;
        if (take_reopen()) {
            if (launch(&user) == 0) {
                snprintf(path, sizeof path, "/user/home/%08x/webkit/shell/local/https_discord.com_0.localstorage",
                         (unsigned)user);
                deadline = time(NULL) + 600;
                status_note("Opening Discord. Sign in, then close the browser to return here.");
            } else
                status_note("Could not open Discord again. Try again in a moment.");
        }
        char token[256] = "";
        if (browser_storage_token(path, token, sizeof token) != 0)
            continue;
        if (strcmp(token, rejected) == 0 || time(NULL) < retry_after) {
            storage_wipe(token, sizeof token);
            continue;
        }
        http_response response = http_request("GET", "/users/@me", NULL, token);
        cJSON *account = response.status == 200 && response.body ? cJSON_Parse(response.body) : NULL;
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(account, "id"));
        if (id && *id && strspn(id, "0123456789") == strlen(id) && strlen(token) < cap && !is_cancelled()) {
            snprintf(out, cap, "%s", token);
            result = 0;
        } else if (response.status == 401 || response.status == 403) {
            snprintf(rejected, sizeof rejected, "%s", token);
            status_note("Browser session was rejected. Sign in again in Discord, then return here.");
        } else {
            retry_after = time(NULL) + 30;
            status_note("Waiting for Discord to verify your session. Keep this app open.");
        }
        cJSON_Delete(account);
        free(response.body);
        storage_wipe(token, sizeof token);
        if (result == 0) break;
    }
    storage_wipe(rejected, sizeof rejected);
    if (result == 0) {
        status_note(NULL);
        LOG("browser login: session validated");
    } else if (is_cancelled()) {
        status_note("Browser login cancelled. Scan the QR code or try again.");
    } else {
        status_note("Browser login timed out. Try again when ready.");
    }
done:
#else
    (void)out;
    (void)cap;
    (void)is_cancelled;
    (void)take_reopen;
    status_note("Browser login is available on PS5.");
#endif
    pthread_mutex_lock(&lock);
    active = 0;
    reopen = 0;
    finished = time(NULL);
    pthread_mutex_unlock(&lock);
    return result;
}
