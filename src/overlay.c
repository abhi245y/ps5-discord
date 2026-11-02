// SPDX-License-Identifier: AGPL-3.0-or-later
#include "overlay.h"

#include <cJSON.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __PROSPERO__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "chat.h"
#include "prefs.h"
#include "stream.h"
#include "log.h"
#include "voice.h"

// The overlay engine's data folder (overlay/src/core/protocol.hpp parent_dir).
#define OVERLAY_DIR "/data/discord-overlay-poc"
#define APP_ASSETS "/data/homebrew/PPSA99072/assets" // the app's /app0/assets
#define IDLE_LINE "1|-|o|0|||"
// overlay/src/core/protocol.hpp Command, written by the overlay (Builder::send).
typedef struct {
    uint64_t signature, seq;
    char text[48];
} overlay_command;
#define OVERLAY_MAGIC 0x31434f504c564f44ULL
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

static int write_line(const char *line)
{
    FILE *f = fopen(OVERLAY_DIR "/text.tmp", "wb");
    if (!f)
        return -1;
    const int ok = fputs(line, f) >= 0;
    return fclose(f) == 0 && ok && rename(OVERLAY_DIR "/text.tmp", OVERLAY_DIR "/text.bin") == 0 ? 0 : -1;
}

// The newest command from the overlay; 0 when there is none.
static uint64_t read_command(overlay_command *c)
{
    FILE *f = fopen(OVERLAY_DIR "/cmd.bin", "rb");
    if (!f)
        return 0;
    const int ok = fread(c, 1, sizeof *c, f) == sizeof *c;
    fclose(f);
    if (!ok || c->signature != OVERLAY_MAGIC)
        return 0;
    c->text[sizeof c->text - 1] = 0;
    return c->seq;
}

// A press in the overlay's call panel.
static void run_command(const char *what)
{
    char *text = voice_state_json();
    cJSON *v = text ? cJSON_Parse(text) : NULL;
    free(text);
    const int mute = cJSON_IsTrue(cJSON_GetObjectItem(v, "mute")), deaf = cJSON_IsTrue(cJSON_GetObjectItem(v, "deaf"));
    cJSON_Delete(v);
    const char *why = "";
    int r = 0;
    if (strcmp(what, "mute") == 0)
        r = deaf ? voice_set_flags(0, 0) : voice_set_flags(!mute, -1); // unmuting also undeafens
    else if (strcmp(what, "deafen") == 0)
        r = voice_set_flags(-1, !deaf);
    else if (strcmp(what, "golive") == 0)
        r = stream_golive(strcmp(stream_state(), "off") == 0 || strcmp(stream_state(), "error") == 0, &why);
    else if (strcmp(what, "leave") == 0)
        voice_leave();
    else
        why = "unknown";
    LOG("overlay: %s -> %d %s", what, r, why);
}

static uint64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

// The service reports in status.bin (overlay/src/core/protocol.hpp Status: signature,
// token, at_ms on the shared monotonic clock, updates, state; 2 = running).
static int running(void)
{
    struct {
        uint64_t signature, token, at_ms, updates;
        int32_t state;
    } st;
    FILE *f = fopen(OVERLAY_DIR "/status.bin", "rb");
    if (!f)
        return 0;
    const int ok = fread(&st, 1, sizeof st, f) == sizeof st;
    fclose(f);
    const uint64_t now = mono_ms();
    return ok && st.signature == OVERLAY_MAGIC && st.state == 2 && st.at_ms <= now + 1000 && now - st.at_ms < 3000;
}

static uint64_t launched_at;
static volatile int reloading;

// A payload from the app's assets to elfldr.
static void send_elf(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    static char elf[1 << 20];
    const size_t n = fread(elf, 1, sizeof elf, f);
    fclose(f);
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    struct sockaddr_in to = {.sin_len = sizeof to, .sin_family = AF_INET, .sin_port = htons(9021)}; // elfldr
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    size_t sent = 0;
    if (n > 0 && n < sizeof elf && connect(fd, (struct sockaddr *)&to, sizeof to) == 0)
        for (ssize_t w; sent < n && (w = send(fd, elf + sent, n - sent, 0)) > 0;)
            sent += (size_t)w;
    close(fd);
    LOG("overlay: sent %s to elfldr (%zu of %zu bytes)", path, sent, n);
}

// The overlay service (overlay/, built into the app), when Settings have it on. A second
// launch exits by itself (its lock). The engine loads once per system UI instance: after
// a core restart it may need overlay_reload().
static void launch(void)
{
    send_elf(APP_ASSETS "/overlay.elf");
    launched_at = mono_ms();
}

// Stop the running service (its token from request.bin), restart the system UI (the
// engine's rollback payload; the system brings it back) and load the service again.
// Asks the running service to stop (stop.bin = its token, from request.bin); its widgets
// go and the game keeps its controller.
static void stop_service(void)
{
    unsigned char request[24];
    FILE *f = fopen(OVERLAY_DIR "/request.bin", "rb");
    const int ok = f && fread(request, 1, sizeof request, f) == sizeof request;
    if (f)
        fclose(f);
    FILE *o = ok ? fopen(OVERLAY_DIR "/stop.bin", "wb") : NULL;
    if (o) {
        fwrite(request + 16, 1, 8, o);
        fclose(o);
    }
}

static void *reload_thread(void *arg)
{
    (void)arg;
    if (running()) {
        stop_service();
        for (int i = 0; i < 60 && running(); i++)
            usleep(200000);
    }
    send_elf(APP_ASSETS "/overlay-restart.elf");
    sleep(30); // the home screen comes back
    launch();
    reloading = 0;
    return NULL;
}

int overlay_reload(void)
{
    if (reloading || prefs_get("overlay") != 1)
        return -1;
    reloading = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, reload_thread, NULL) != 0) {
        reloading = 0;
        return -1;
    }
    pthread_detach(t);
    return 0;
}

const char *overlay_state(void)
{
    if (prefs_get("overlay") != 1)
        return "off";
    if (reloading)
        return "reloading";
    if (running())
        return "running";
    return launched_at && mono_ms() - launched_at < 20000 ? "starting" : "stopped";
}

static void *feed(void *arg)
{
    (void)arg;
    char last[128] = "", line[128];
    time_t live_since = 0;
    int launched = 0;
    overlay_command c;
    uint64_t seen = read_command(&c); // one left from before this core ran is not new
    mkdir(OVERLAY_DIR, 0755);
    for (;;) {
        const int on = prefs_get("overlay") == 1;
        if (on && !launched && !running()) {
            launch();
            launched = 1;
        } else if (!on && running() && !reloading) {
            stop_service(); // switched off in Settings
            launched = 0;
        }
        compose(line, sizeof line, &live_since);
        if (strcmp(line, last) != 0 && write_line(line) == 0)
            snprintf(last, sizeof last, "%s", line);
        const uint64_t seq = read_command(&c);
        if (seq > seen) {
            seen = seq;
            run_command(c.text);
        }
        usleep(200000); // the overlay reads every 12 frames (200 ms)
    }
    return NULL;
}

void overlay_shutdown(void)
{
    write_line(IDLE_LINE); // the overlay stays loaded; it hides until the core is back
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

void overlay_shutdown(void)
{
}

int overlay_reload(void)
{
    return -1;
}

const char *overlay_state(void)
{
    return "off";
}
#endif
