// SPDX-License-Identifier: AGPL-3.0-or-later
#include "notify.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "prefs.h"

// prefs quiet_hours: 1 22:00-07:00, 2 23:00-08:00, 3 00:00-08:00, 4 21:00-06:00 (the app's list).
static const struct {
    int from, to;
} quiet[] = {{0, 0}, {22, 7}, {23, 8}, {0, 8}, {21, 6}};

static int local_hour(void);

#define GAP_SECONDS 8
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static char latest_title[160], latest_body[200], latest_icon[160];
static int waiting; // messages since the last alert
static time_t next_ok;

static void *pacer(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    for (;;) {
        while (!waiting)
            pthread_cond_wait(&wake, &lock);
        time_t now = time(NULL);
        if (now < next_ok) {
            struct timespec until = {.tv_sec = next_ok};
            pthread_cond_timedwait(&wake, &lock, &until);
            continue;
        }
        char title[200], body[200], icon[160];
        if (waiting == 1)
            snprintf(title, sizeof title, "%s", latest_title);
        else
            snprintf(title, sizeof title, "%d new messages · %s", waiting, latest_title);
        snprintf(body, sizeof body, "%s", latest_body);
        snprintf(icon, sizeof icon, "%s", latest_icon);
        waiting = 0;
        next_ok = now + GAP_SECONDS;
        pthread_mutex_unlock(&lock);
        notify_system(title, body, icon[0] ? icon : NULL);
        pthread_mutex_lock(&lock);
    }
    return NULL;
}

static void start_pacer(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, pacer, NULL) == 0)
        pthread_detach(t);
}

void notify_message(const char *title, const char *body, const char *icon)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, start_pacer);
    pthread_mutex_lock(&lock);
    snprintf(latest_title, sizeof latest_title, "%s", title);
    snprintf(latest_body, sizeof latest_body, "%s", body);
    snprintf(latest_icon, sizeof latest_icon, "%s", icon ? icon : "");
    waiting++;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

int notify_quiet_now(void)
{
    int q = prefs_get("quiet_hours");
    if (q <= 0 || q >= (int)(sizeof quiet / sizeof *quiet))
        return 0;
    int h = local_hour(), from = quiet[q].from, to = quiet[q].to;
    return from < to ? h >= from && h < to : h >= from || h < to;
}

#ifdef __PROSPERO__
#include <dlfcn.h>
#include <stdbool.h>

#include <cJSON.h>

typedef struct {
    char reserved[45];
    char message[3075];
} notify_request;

int sceKernelSendNotificationRequest(int, notify_request *, size_t, int);
int sceSystemServiceParamGetInt(int, int *);

static int local_hour(void)
{
    int zone = 0, summer = 0; // the console's time zone, as the app reads it
    sceSystemServiceParamGetInt(4, &zone);
    sceSystemServiceParamGetInt(5, &summer);
    long t = (long)time(NULL) + (zone + (summer ? 60 : 0)) * 60L;
    return (int)((t / 3600) % 24);
}

// Cut on a character boundary.
static void cut(char *s, size_t max)
{
    size_t n = strlen(s);
    if (n <= max)
        return;
    while (max > 0 && ((unsigned char)s[max] & 0xc0) == 0x80)
        max--;
    s[max] = 0;
}

// The plain toast every payload can show, when the real one cannot be had.
static void debug_toast(const char *title, const char *body)
{
    notify_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.message, 201, "%s\n%s", title, body);
    cut(req.message, 200);
    int r = sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
    if (r != 0)
        LOG("notify: system alert failed (%d)", r);
}

// The PS5's own notification (libSceNotification, as the payload SDK's notify sample):
// a toast with an icon, title and text that stays in the notification list. Only a
// payload can load the library (an app gets 0x80020063). Logged, or it never shows;
// "ServiceFeedback" shows during games.
static int (*send_json)(int user, bool logged, const char *json);

static void load_notification(void)
{
    void *lib = dlopen("libSceNotification.sprx", RTLD_LAZY);
    send_json = lib ? (int (*)(int, bool, const char *))dlsym(lib, "sceNotificationSend") : NULL;
    if (!send_json)
        LOG("notify: libSceNotification unavailable, plain toasts only");
}

void notify_system(const char *title, const char *body, const char *icon)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    static unsigned id;
    pthread_once(&once, load_notification);
    char t[200], b[200];
    snprintf(t, sizeof t, "%s", title);
    snprintf(b, sizeof b, "%s", body);
    cut(t, 120);
    cut(b, 160);
    if (!send_json) {
        debug_toast(t, b);
        return;
    }
    cJSON *o = cJSON_CreateObject(), *raw = cJSON_AddObjectToObject(o, "rawData");
    cJSON_AddStringToObject(raw, "viewTemplateType", "InteractiveToastTemplateB");
    cJSON_AddStringToObject(raw, "channelType", "ServiceFeedback");
    cJSON_AddStringToObject(raw, "bundleName", "Discord");
    cJSON_AddStringToObject(raw, "useCaseId", "IDC");
    cJSON_AddStringToObject(raw, "toastOverwriteType", "No");
    cJSON_AddTrueToObject(raw, "isImmediate");
    cJSON_AddNumberToObject(raw, "priority", 100);
    cJSON *view = cJSON_AddObjectToObject(raw, "viewData");
    cJSON *ic = cJSON_AddObjectToObject(view, "icon");
    if (icon) {
        cJSON_AddStringToObject(ic, "type", "Url");
        cJSON_AddStringToObject(cJSON_AddObjectToObject(ic, "parameters"), "url", icon);
    } else {
        cJSON_AddStringToObject(ic, "type", "Predefined");
        cJSON_AddStringToObject(cJSON_AddObjectToObject(ic, "parameters"), "icon", "community");
    }
    cJSON_AddStringToObject(cJSON_AddObjectToObject(view, "message"), "body", t);
    cJSON_AddStringToObject(cJSON_AddObjectToObject(view, "subMessage"), "body", b);
    char when[40], idtext[16];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
    snprintf(idtext, sizeof idtext, "%u", ++id + (unsigned)now % 100000u * 1000u);
    cJSON_AddStringToObject(o, "createdDateTime", when);
    cJSON_AddStringToObject(o, "localNotificationId", idtext);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    int r = json ? send_json(0xFE, true, json) : -1; // 0xFE: the system, for every user
    free(json);
    if (r != 0) {
        LOG("notify: sceNotificationSend failed (0x%x), plain toast instead", (unsigned)r);
        debug_toast(t, b);
    }
}
#else
static int local_hour(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour;
}

void notify_system(const char *title, const char *body, const char *icon)
{
    LOG("notify: %s | %s | %s", title, body, icon ? icon : "-");
}
#endif
