// SPDX-License-Identifier: AGPL-3.0-or-later
#include "prefs.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

typedef struct {
    const char *key;
    int min, max;
    int value; // starts as the default
} pref;

static pref prefs[] = {
    {"golive_quality", 0, 3, 0}, // broadcast.h bc_profile: 720p30, 720p60, 1080p30, 1080p60
    {"mic_gain", 0, 10, 5},      // 5 = as captured, 10 = twice
    {"notify_dm", 0, 1, 1},      // DMs and mentions (chat.c notify_kind)
    {"notify_server", 0, 2, 1},  // server messages: 0 off, 1 as set in Discord, 2 every one
    {"notify_system", 0, 1, 1},  // PS5 alerts while the app is not on screen
    {"notify_live", 0, 1, 0},    // ... and while Go Live is on (they would be on the stream)
    {"quiet_hours", 0, 4, 0},    // notify.c: no sounds, no PS5 alerts
};
#define COUNT ((int)(sizeof prefs / sizeof *prefs))

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char file[220];

static pref *find(const char *key)
{
    for (int i = 0; i < COUNT; i++)
        if (strcmp(prefs[i].key, key) == 0)
            return &prefs[i];
    return NULL;
}

void prefs_init(const char *data_dir)
{
    snprintf(file, sizeof file, "%s/prefs", data_dir);
    FILE *f = fopen(file, "r");
    if (!f)
        return;
    char key[64];
    int v;
    while (fscanf(f, "%63s %d", key, &v) == 2) {
        pref *p = find(key);
        if (p && v >= p->min && v <= p->max)
            p->value = v;
    }
    fclose(f);
}

int prefs_get(const char *key)
{
    pref *p = find(key);
    return p ? p->value : 0;
}

int prefs_set(const char *key, int value)
{
    pref *p = find(key);
    if (!p || value < p->min || value > p->max)
        return -1;
    pthread_mutex_lock(&lock);
    p->value = value;
    char tmp[240];
    snprintf(tmp, sizeof tmp, "%s.tmp", file);
    FILE *f = fopen(tmp, "w");
    int ok = f != NULL;
    for (int i = 0; ok && i < COUNT; i++)
        ok = fprintf(f, "%s %d\n", prefs[i].key, prefs[i].value) > 0;
    if (f && fclose(f) != 0)
        ok = 0;
    if (!ok || rename(tmp, file) != 0) {
        remove(tmp);
        LOG("prefs: could not save %s", file);
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

cJSON *prefs_json(void)
{
    cJSON *o = cJSON_CreateObject();
    for (int i = 0; i < COUNT; i++)
        cJSON_AddNumberToObject(o, prefs[i].key, prefs[i].value);
    return o;
}
