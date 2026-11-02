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

#include "broadcast.h"
#include "chat.h"
#include "headset.h"
#include "prefs.h"
#include "stream.h"
#include "http.h"
#include "log.h"
#include "png_circle.h"
#include "voice.h"

// The overlay engine's data folder (overlay/src/core/protocol.hpp parent_dir).
#define OVERLAY_DIR "/data/discord-overlay-poc"
#define APP_ASSETS "/data/homebrew/PPSA99072/assets" // the app's /app0/assets
#define IDLE_LINE "3|-|o|0||0||||||"
// overlay/src/core/protocol.hpp Command, written by the overlay (Builder::send).
typedef struct {
    uint64_t signature, seq;
    char text[48];
} overlay_command;
#define OVERLAY_MAGIC 0x31434f504c564f44ULL
#define MAX_PEOPLE 8
#define AVATAR_PX 128 // round PNGs in OVERLAY_DIR/av, scaled down by the overlay

// Copies at most n bytes of src, whole UTF-8 characters only; the line's separators (| , ~ ^ ;)
// and control characters become spaces.
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
        out[at++] = c == '|' || c == ',' || c == '~' || c == '^' || c == ';' || ((unsigned char)c < 0x20) ? ' ' : c;
    }
    out[at] = 0;
    return at;
}

static int sep(char *out, int cap, int at, char c)
{
    if (at + 1 < cap) {
        out[at++] = c;
        out[at] = 0;
    }
    return at;
}

int overlay_line(char *out, int cap, char state, char me, int glance, const char *live, int viewers,
                 const char *call, const char *timer, const char *quality, const char *settings,
                 const overlay_headset *bt, const overlay_person *people, int count)
{
    if (cap > 2048)
        cap = 2048; // the engine reads 2047 bytes at most
    int at = snprintf(out, (size_t)cap, "3|%c|%c|%d|", state, me, glance ? 1 : 0);
    at = put(out, cap, at, live, 16);
    at += snprintf(out + at, (size_t)(cap - at), "|%d|", viewers);
    at = put(out, cap, at, call, 40);
    at = sep(out, cap, at, '|');
    at = put(out, cap, at, timer, 12);
    at = sep(out, cap, at, '|');
    at = put(out, cap, at, quality, 24);
    at = sep(out, cap, at, '|');
    at = put(out, cap, at, settings, 80);
    at = sep(out, cap, at, '|');
    // The headset: on~connected address~address^name;... (paired ones, up to four)
    at += snprintf(out + at, (size_t)(cap - at), "%d~", bt->on);
    at = put(out, cap, at, bt->connected, 17);
    at = sep(out, cap, at, '~');
    for (int i = 0; i < bt->count; i++) {
        if (i)
            at = sep(out, cap, at, ';');
        at = put(out, cap, at, bt->addr[i], 17);
        at = sep(out, cap, at, '^');
        at = put(out, cap, at, bt->name[i], 32);
    }
    at = sep(out, cap, at, '|');
    int first = 1;
    for (int pass = 0; pass < 2; pass++) // talking first
        for (int i = 0; i < count; i++) {
            const overlay_person *p = people + i;
            if ((p->flag == 't') != (pass == 0))
                continue;
            char one[160];
            int n = put(one, (int)sizeof one, 0, p->id, 23);
            n = sep(one, (int)sizeof one, n, '~');
            n = put(one, (int)sizeof one, n, p->avatar, 63);
            n = sep(one, (int)sizeof one, n, '~');
            n = put(one, (int)sizeof one, n, p->name, 32);
            n = sep(one, (int)sizeof one, n, '~');
            n = sep(one, (int)sizeof one, n, p->flag);
            if (at + !first + n >= cap)
                return at; // full: the rest do not fit
            if (!first)
                out[at++] = ',';
            memcpy(out + at, one, (size_t)n + 1);
            at += n;
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

static void clock_text(char *out, size_t cap, long s)
{
    if (s >= 3600)
        snprintf(out, cap, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else
        snprintf(out, cap, "%ld:%02ld", s / 60, s % 60);
}

// The avatar's file stem: <id>_<hash>, or <id>_e<n> for Discord's default picture.
static void avatar_key(const chat_person *p, char *out, size_t cap)
{
    if (p->avatar[0])
        snprintf(out, cap, "%s_%s", p->id, p->avatar);
    else
        snprintf(out, cap, "%s_e%llu", p->id, (strtoull(p->id, NULL, 10) >> 22) % 6);
}

static int discord_cdn(const char *host)
{
    return strcmp(host, "cdn.discordapp.com") == 0;
}

typedef struct {
    unsigned char *data;
    size_t n;
} grow_buffer;

static int to_memory(void *user, const void *data, size_t len)
{
    grow_buffer *b = user;
    unsigned char *more = realloc(b->data, b->n + len);
    if (!more)
        return 0;
    memcpy(more + b->n, data, len);
    b->data = more;
    b->n += len;
    return 1;
}

// One missing avatar per call (a download is a fraction of a second): fetched, cut round and
// kept under its key. A failed one waits a minute.
static void fetch_avatars(const chat_person *people, int count)
{
    static char failed[16][64];
    static time_t failed_at[16];
    for (int i = 0; i < count; i++) {
        char key[64], path[160], url[200];
        avatar_key(&people[i], key, sizeof key);
        snprintf(path, sizeof path, OVERLAY_DIR "/av/%s.png", key);
        struct stat st;
        if (stat(path, &st) == 0)
            continue;
        int skip = 0;
        for (int f = 0; f < 16; f++)
            skip |= strcmp(failed[f], key) == 0 && time(NULL) - failed_at[f] < 60;
        if (skip)
            continue;
        if (people[i].avatar[0])
            snprintf(url, sizeof url, "https://cdn.discordapp.com/avatars/%s/%s.png?size=%d", people[i].id,
                     people[i].avatar, AVATAR_PX);
        else
            snprintf(url, sizeof url, "https://cdn.discordapp.com/embed/avatars/%s.png", strrchr(key, 'e') + 1);
        grow_buffer b = {0};
        int ok = http_get_url(url, discord_cdn, 2u << 20, to_memory, &b) == 200;
        char tmp[176];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *out = ok ? fopen(tmp, "wb") : NULL;
        ok = out && png_circle(b.data, b.n, AVATAR_PX, out) == 0;
        if (out)
            ok = fclose(out) == 0 && ok;
        ok = ok && rename(tmp, path) == 0;
        free(b.data);
        if (!ok) {
            remove(tmp);
            static int next;
            snprintf(failed[next], sizeof failed[next], "%s", key);
            failed_at[next] = time(NULL);
            next = (next + 1) % 16;
            LOG("overlay: avatar %s not fetched", key);
        }
        return; // one per call
    }
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
    const double since = cJSON_GetNumberValue(cJSON_GetObjectItem(v, "since_ms"));
    chat_person people[MAX_PEOPLE];
    overlay_person shown[MAX_PEOPLE];
    char call[64] = "";
    int count = state != '-' && channel ? chat_call_people(channel, call, sizeof call, people, MAX_PEOPLE) : 0;
    int talking = 0;
    for (int i = 0; i < count; i++) {
        overlay_person *p = shown + i;
        snprintf(p->id, sizeof p->id, "%s", people[i].id);
        snprintf(p->name, sizeof p->name, "%s", people[i].name);
        char key[64], path[160];
        avatar_key(&people[i], key, sizeof key);
        snprintf(path, sizeof path, OVERLAY_DIR "/av/%s.png", key);
        struct stat sb;
        snprintf(p->avatar, sizeof p->avatar, "%s", stat(path, &sb) == 0 ? key : "");
        p->flag = speaking(cJSON_GetObjectItem(v, "speaking"), people[i].id) ? 't'
                  : people[i].deaf ? 'd' : people[i].muted ? 'm' : '-';
        talking |= p->flag == 't';
    }
    cJSON_Delete(v);
    fetch_avatars(people, count);

    char timer[16] = "";
    if (state == 'c' && since > 0)
        clock_text(timer, sizeof timer, (long)((double)time(NULL) - since / 1000.0));
    char live[16] = "";
    if (strcmp(stream_state(), "live") == 0) {
        const time_t now = time(NULL);
        if (!*live_since)
            *live_since = now;
        clock_text(live, sizeof live, (long)(now - *live_since));
    } else
        *live_since = 0;
    // Settings: 0 off, 1 while someone talks (or Go Live is on), 2 always.
    const int pref = prefs_get("overlay_glance");
    const int glance = pref == 2 || (pref == 1 && (talking || live[0]));
    int w = 0, h = 0, fps = 0;
    bc_profile(&w, &h, &fps);
    char quality[24];
    snprintf(quality, sizeof quality, "%dp \xc2\xb7 %d fps", h, fps);
    // The mini settings' values, in the order discord_app.inc reads them.
    static const char *const keys[] = {"golive_quality", "mic_gain",     "call_volume",   "overlay_glance",
                                       "overlay_pos",    "overlay_hold", "notify_dm",     "notify_server",
                                       "notify_system",  "quiet_hours",  "bt_handsfree"};
    char settings[96];
    int n = 0;
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        n += snprintf(settings + n, sizeof settings - (size_t)n, "%s%d", i ? "." : "", prefs_get(keys[i]));
    overlay_headset bt = {0};
    cJSON *hs = hs_state_json();
    bt.on = cJSON_IsTrue(cJSON_GetObjectItem(hs, "on"));
    const char *addr = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(hs, "headset"), "addr"));
    snprintf(bt.connected, sizeof bt.connected, "%s", addr ? addr : "");
    cJSON *d;
    cJSON_ArrayForEach(d, cJSON_GetObjectItem(hs, "devices"))
        if (bt.count < 4 && cJSON_IsTrue(cJSON_GetObjectItem(d, "paired"))) {
            const char *a = cJSON_GetStringValue(cJSON_GetObjectItem(d, "addr")),
                       *nm = cJSON_GetStringValue(cJSON_GetObjectItem(d, "name"));
            snprintf(bt.addr[bt.count], sizeof bt.addr[0], "%s", a ? a : "");
            snprintf(bt.name[bt.count], sizeof bt.name[0], "%s", nm && nm[0] ? nm : "Headset");
            bt.count++;
        }
    cJSON_Delete(hs);
    overlay_line(line, cap, state, me, glance, live, live[0] ? stream_viewers() : 0, call, timer, quality, settings,
                 &bt, shown, count);
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
    else if (strncmp(what, "set ", 4) == 0) {
        // The mini settings: only these keys (cmd.bin is written by the overlay).
        static const char *const allowed[] = {"golive_quality", "mic_gain",     "call_volume",   "overlay_glance",
                                              "overlay_pos",    "overlay_hold", "notify_dm",     "notify_server",
                                              "notify_system",  "quiet_hours",  "bt_handsfree"};
        char key[32];
        int value = 0, known = 0;
        if (sscanf(what + 4, "%31s %d", key, &value) == 2)
            for (size_t i = 0; i < sizeof allowed / sizeof *allowed; i++)
                known |= strcmp(key, allowed[i]) == 0;
        r = known ? prefs_set(key, value) : -1;
        if (known && r == 0 && strcmp(key, "golive_quality") == 0)
            why = "from the next Go Live";
    } else if (strncmp(what, "bt connect ", 11) == 0)
        r = hs_connect(what + 11);
    else if (strcmp(what, "bt disconnect") == 0)
        r = hs_disconnect();
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
    static char last[2048], line[2048];
    time_t live_since = 0;
    int launched = 0;
    overlay_command c;
    uint64_t seen = read_command(&c); // one left from before this core ran is not new
    mkdir(OVERLAY_DIR, 0755);
    mkdir(OVERLAY_DIR "/av", 0755);
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
