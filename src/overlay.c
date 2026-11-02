// SPDX-License-Identifier: AGPL-3.0-or-later
#include "overlay.h"

#include <cJSON.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "chat.h"
#include "prefs.h"
#include "stream.h"
#include "voice.h"

// The overlay engine's data folder (overlay/src/core/protocol.hpp parent_dir).
#define OVERLAY_DIR "/data/discord-overlay-poc"
#define MAX_PEOPLE 8

// Copies at most n bytes of src, whole UTF-8 characters only; '|', ',' and '~' (the line's
// separators) and control characters become spaces.
static int put(char *out, int cap, int at, const char *src, int n)
{
    int len = (int)strlen(src);
    if (len > n)
        len = n;
    if (at + len >= cap)
        len = cap - 1 - at;
    if (len < 0)
        return at;
    while (len > 0 && ((unsigned char)src[len] & 0xc0) == 0x80 && src[len] != 0)
        len--; // do not cut a character
    for (int i = 0; i < len; i++) {
        const char c = src[i];
        out[at++] = c == '|' || c == ',' || c == '~' || ((unsigned char)c < 0x20) ? ' ' : c;
    }
    out[at] = 0;
    return at;
}

int overlay_line(char *out, int cap, char state, char me, int glance, const char *live, const char *call,
                 const char names[][64], const char *flags, int count)
{
    if (cap > 128)
        cap = 128; // the engine reads 127 bytes at most
    int at = snprintf(out, (size_t)cap, "1|%c|%c|%d|", state, me, glance ? 1 : 0);
    at = put(out, cap, at, live, 16);
    out[at++] = '|';
    at = put(out, cap, at, call, 30);
    out[at++] = '|';
    out[at] = 0;
    int first = 1;
    for (int pass = 0; pass < 2; pass++) // talking first
        for (int i = 0; i < count; i++) {
            if ((flags[i] == 't') != (pass == 0))
                continue;
            char one[40];
            int n = put(one, (int)sizeof one, 0, names[i], 16);
            if (at + !first + n + 2 >= cap)
                return at; // full: the rest do not fit
            if (!first)
                out[at++] = ',';
            memcpy(out + at, one, (size_t)n);
            at += n;
            out[at++] = '~';
            out[at++] = flags[i];
            out[at] = 0;
            first = 0;
        }
    return at;
}

#ifdef __PROSPERO__
static int speaking(cJSON *ids, const char *id)
{
    cJSON *v;
    cJSON_ArrayForEach(v, ids)
        if (cJSON_IsString(v) && strcmp(v->valuestring, id) == 0)
            return 1;
    return 0;
}

static void compose(char *line, int cap, time_t *live_since)
{
    char *text = voice_state_json();
    cJSON *v = text ? cJSON_Parse(text) : NULL;
    free(text);
    const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(v, "state"));
    const char *channel = cJSON_GetStringValue(cJSON_GetObjectItem(v, "channel"));
    const char state = !st ? '-' : strcmp(st, "connected") == 0 ? 'c'
                       : strcmp(st, "idle") == 0 || strcmp(st, "error") == 0 ? '-' : 'j';
    const char me = cJSON_IsTrue(cJSON_GetObjectItem(v, "deaf")) ? 'd'
                    : cJSON_IsTrue(cJSON_GetObjectItem(v, "mute")) ? 'm' : 'o';
    chat_person people[MAX_PEOPLE];
    char call[64] = "", names[MAX_PEOPLE][64], flags[MAX_PEOPLE];
    int count = state != '-' && channel ? chat_call_people(channel, call, sizeof call, people, MAX_PEOPLE) : 0;
    int talking = 0;
    for (int i = 0; i < count; i++) {
        snprintf(names[i], sizeof names[i], "%s", people[i].name);
        flags[i] = speaking(cJSON_GetObjectItem(v, "speaking"), people[i].id) ? 't'
                   : people[i].deaf ? 'd' : people[i].muted ? 'm' : '-';
        talking |= flags[i] == 't';
    }
    cJSON_Delete(v);

    char live[24] = "";
    if (strcmp(stream_state(), "live") == 0) {
        const time_t now = time(NULL);
        if (!*live_since)
            *live_since = now;
        const long s = (long)(now - *live_since);
        if (s >= 3600)
            snprintf(live, sizeof live, "%ld:%02ld:%02ld %d", s / 3600, (s / 60) % 60, s % 60, stream_viewers());
        else
            snprintf(live, sizeof live, "%ld:%02ld %d", s / 60, s % 60, stream_viewers());
    } else
        *live_since = 0;
    // Settings: 0 off, 1 while someone talks (or Go Live is on), 2 always.
    const int pref = prefs_get("overlay_glance");
    const int glance = pref == 2 || (pref == 1 && (talking || live[0]));
    overlay_line(line, cap, state, me, glance, live, call, (const char(*)[64])names, flags, count);
}

static void *feed(void *arg)
{
    (void)arg;
    char last[128] = "", line[128];
    time_t live_since = 0;
    mkdir(OVERLAY_DIR, 0755);
    for (;;) {
        compose(line, sizeof line, &live_since);
        if (strcmp(line, last) != 0) {
            FILE *f = fopen(OVERLAY_DIR "/text.tmp", "wb");
            if (f) {
                const int ok = fputs(line, f) >= 0;
                if (fclose(f) == 0 && ok && rename(OVERLAY_DIR "/text.tmp", OVERLAY_DIR "/text.bin") == 0)
                    snprintf(last, sizeof last, "%s", line);
            }
        }
        usleep(200000); // the overlay reads every 12 frames (200 ms)
    }
    return NULL;
}

void overlay_init(void)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &attr, feed, NULL);
    pthread_attr_destroy(&attr);
}
#else
void overlay_init(void)
{
}
#endif
