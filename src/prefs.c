// SPDX-License-Identifier: AGPL-3.0-or-later
#include "prefs.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#include "log.h"

typedef struct {
    const char *key;
    int min, max, def;
    int value;
} pref;

static pref prefs[] = {
    {"golive_quality", 0, 3, 0, 0}, // broadcast.h bc_profile: 720p30, 720p60, 1080p30, 1080p60
    {"golive_method", 0, 1, 0, 0}, // broadcast.c: 0 hardware encoder, 1 Remote Play (legacy)
    {"golive_bitrate", 0, 19, 0, 0}, // broadcast.c bitrate_for: 0 recommended, n: n + 1 Mbit/s (wireless)
    {"mic_gain", 0, 10, 5, 5},      // 5 = as captured, 10 = twice
    {"call_volume", 0, 10, 5, 5},   // the voices heard: 5 = as received, 10 = twice
    {"controller_audio", 0, 1, 0, 0}, // opt-in automatic USB mic/speaker capture during calls
    {"notify_dm", 0, 1, 1, 1},      // DMs and mentions (chat.c notify_kind)
    {"notify_server", 0, 2, 1, 1},  // server messages: 0 off, 1 as set in Discord, 2 every one
    {"notify_system", 0, 1, 1, 1},  // PS5 alerts while the app is not on screen
    {"notify_live", 0, 1, 0, 0},    // ... and while Go Live is on (they would be on the stream)
    {"quiet_hours", 0, 4, 0, 0},    // notify.c: no sounds, no PS5 alerts
    {"share_game", 0, 1, 1, 1},     // Discord shows the game in front on this console (gateway.c)
    {"bt_handsfree", 0, 1, 0, 0},   // Bluetooth headset: hands-free for calls even on AirPods (src/bt)
    {"overlay_glance", 0, 2, 1, 1}, // in-game overlay's glance card: 0 off, 1 while someone talks, 2 always
    {"overlay_pill_corner", 0, 3, 1, 1}, // 0 top left, 1 top right, 2 bottom left, 3 bottom right
    {"overlay_card_corner", 0, 3, 1, 1}, // independent talking-card corner; same order
    {"overlay", 0, 1, 0, 0},        // in-game overlay (overlay/, loaded into the system UI): off until tested
    {"overlay_pos", 0, 5, 0, 0},    // legacy position, retained for older overlays
    {"overlay_hold", 0, 12, 3, 3},  // holding Options 0.3 s + this many tenths opens the panel
    {"overlay_autoload", 0, 1, 0, 0}, // the core starts and the overlay is not loaded: reload the home screen
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

static atomic_int loaded; // a console user's settings are in (prefs_load ran once)

int prefs_loaded(void)
{
    return loaded;
}

void prefs_load(const char *data_dir, int32_t user)
{
    pthread_mutex_lock(&lock);
    snprintf(file, sizeof file, "%s/prefs-%08x", data_dir, (unsigned)user);
    for (int i = 0; i < COUNT; i++)
        prefs[i].value = prefs[i].def;
    FILE *f = fopen(file, "r");
    if (!f) { // the one file of before per-user settings goes to the first user who runs
        char legacy[220];
        snprintf(legacy, sizeof legacy, "%s/prefs", data_dir);
        if (rename(legacy, file) == 0)
            f = fopen(file, "r");
    }
    if (!f) {
        loaded = 1;
        pthread_mutex_unlock(&lock);
        return;
    }
    char key[64];
    int v;
    while (fscanf(f, "%63s %d", key, &v) == 2) {
        pref *p = find(key);
        if (p && v >= p->min && v <= p->max)
            p->value = v;
    }
    fclose(f);
    loaded = 1;
    pthread_mutex_unlock(&lock);
}

int prefs_get(const char *key)
{
    pthread_mutex_lock(&lock);
    pref *p = find(key);
    int value = p ? p->value : 0;
    pthread_mutex_unlock(&lock);
    return value;
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
    pthread_mutex_lock(&lock);
    for (int i = 0; i < COUNT; i++)
        cJSON_AddNumberToObject(o, prefs[i].key, prefs[i].value);
    pthread_mutex_unlock(&lock);
    return o;
}
