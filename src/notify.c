// SPDX-License-Identifier: AGPL-3.0-or-later
#include "notify.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "prefs.h"

// prefs quiet_hours: 1 22:00-07:00, 2 23:00-08:00, 3 00:00-08:00, 4 21:00-06:00 (the app's list).
static const struct {
    int from, to;
} quiet[] = {{0, 0}, {22, 7}, {23, 8}, {0, 8}, {21, 6}};

static int local_hour(void);

int notify_quiet_now(void)
{
    int q = prefs_get("quiet_hours");
    if (q <= 0 || q >= (int)(sizeof quiet / sizeof *quiet))
        return 0;
    int h = local_hour(), from = quiet[q].from, to = quiet[q].to;
    return from < to ? h >= from && h < to : h >= from || h < to;
}

#ifdef __PROSPERO__
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

void notify_system(const char *text)
{
    notify_request req;
    memset(&req, 0, sizeof req);
    size_t n = strlen(text);
    if (n > 200) { // the pop-up shows a few lines at most; cut on a character boundary
        n = 200;
        while (n > 0 && ((unsigned char)text[n] & 0xc0) == 0x80)
            n--;
    }
    memcpy(req.message, text, n);
    int r = sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
    if (r != 0)
        LOG("notify: system alert failed (%d)", r);
}
#else
static int local_hour(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour;
}

void notify_system(const char *text)
{
    LOG("notify: %s", text);
}
#endif
