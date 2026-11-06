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
#include "notify.h"
#include "png_circle.h"
#include "voice.h"

// The overlay engine's data folder (overlay/src/core/protocol.hpp parent_dir).
#define OVERLAY_DIR "/data/discord-overlay-poc"
#define APP_ASSETS "/data/homebrew/PPSA99072/assets" // the app's /app0/assets
#define IDLE_LINE "3|x|o|0||0||||||" // state x: the core quit (the overlay goes inert)
// overlay/src/core/protocol.hpp Command, written by the overlay (Builder::send).
typedef struct {
    uint64_t signature, seq;
    char text[48];
} overlay_command;
#define OVERLAY_MAGIC 0x31434f504c564f44ULL
#define OVERLAY_TEXT_MAX 8192
// protocol.hpp Outbox (send.bin), written by the overlay (Builder::send_text).
typedef struct {
    uint64_t signature, seq;
    uint32_t len, reserved;
    char channel[24];
    char text[OVERLAY_TEXT_MAX];
} overlay_outbox;
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

// The typed message in the outbox file: its seq (0 when there is none, or it is not valid) and
// the text, NUL-terminated, into text (cap at least OVERLAY_TEXT_MAX + 1), and the chat's channel id
// into channel (digits, "" for the call's chat).
uint64_t overlay_read_outbox(const char *path, char *text, size_t cap, char *channel, size_t channel_cap)
{
    static overlay_outbox box; // the feed and tests only; 8 KB stays off the small thread stacks
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    const int ok = fread(&box, 1, sizeof box, f) == sizeof box && fgetc(f) == EOF;
    fclose(f);
    if (!ok || box.signature != OVERLAY_MAGIC || box.len == 0 || box.len > sizeof box.text || cap <= box.len)
        return 0;
    const size_t n = strnlen(box.channel, sizeof box.channel);
    if (n >= sizeof box.channel || n >= channel_cap || strspn(box.channel, "0123456789") != n)
        return 0; // an id is digits only: it goes into a URL
    memcpy(channel, box.channel, n + 1);
    memcpy(text, box.text, box.len);
    text[box.len] = 0;
    return box.seq;
}

// ---- the chat feed ----------------------------------------------------------------------------
// Whole characters, at most n bytes of src (and cap-1 in all) at out[at]; control characters (tab and
// newline separate the feed's fields and records) become spaces.
static int put_text(char *out, int cap, int at, const char *src, int n)
{
    int len = (int)strlen(src);
    if (len > n)
        len = n;
    if (at + len >= cap)
        len = cap - 1 - at;
    if (len < 0)
        return at;
    while (len > 0 && ((unsigned char)src[len] & 0xc0) == 0x80 && src[len] != 0)
        len--;
    for (int i = 0; i < len; i++)
        out[at++] = (unsigned char)src[i] < 0x20 || src[i] == 0x7f ? ' ' : src[i];
    out[at] = 0;
    return at;
}

// A tab, then put_text.
static int tab_put(char *out, int cap, int at, const char *src, int n)
{
    if (at + 1 >= cap)
        return at;
    out[at++] = '\t';
    return put_text(out, cap, at, src, n);
}

// The feed with the messages from skip on and the first nmen mentions; -1 when it does not fit in cap.
static int chat_build(char *out, int cap, const overlay_chat *c, int skip, int nmen, time_t now)
{
    char one[1024];
    int at = 0, n;
#define APPEND_LINE()                                                                                                  \
    do {                                                                                                               \
        if (at + n + 1 >= cap)                                                                                         \
            return -1;                                                                                                 \
        memcpy(out + at, one, (size_t)n);                                                                              \
        out[at += n] = '\n';                                                                                           \
        out[++at] = 0;                                                                                                 \
    } while (0)
    n = snprintf(one, sizeof one, "H\t%c\t", c->kind == 'c' || c->kind == 'd' ? c->kind : '-');
    n = put_text(one, sizeof one, n, c->chan, 23);
    n = tab_put(one, sizeof one, n, c->title, 64);
    n = tab_put(one, sizeof one, n, c->sub, 64);
    n = tab_put(one, sizeof one, n, c->icon, 63);
    n += snprintf(one + n, sizeof one - (size_t)n, "\t%d\t", c->unseen < 0 ? 0 : c->unseen);
    n = put_text(one, sizeof one, n, c->focus, 23);
    n += snprintf(one + n, sizeof one - (size_t)n, "\t%d", c->rev < 0 ? 0 : c->rev);
    APPEND_LINE();
    for (int i = skip; i < c->nmsg; i++) {
        const overlay_msg *m = c->msgs + i;
        n = snprintf(one, sizeof one, "M\t");
        n = put_text(one, sizeof one, n, m->id, 23);
        n = tab_put(one, sizeof one, n, m->avatar, 63);
        n = tab_put(one, sizeof one, n, m->name, 32);
        n = tab_put(one, sizeof one, n, m->time, 11);
        n += snprintf(one + n, sizeof one - (size_t)n, "\t%d\t", m->mine ? 1 : 0);
        n = put_text(one, sizeof one, n, m->text, 120);
        APPEND_LINE();
    }
    for (int i = 0; i < nmen; i++) {
        const overlay_mention *m = c->men + i;
        char age[12];
        overlay_age(age, sizeof age, (long)(now - m->at));
        n = snprintf(one, sizeof one, "N\t");
        n = put_text(one, sizeof one, n, m->id, 23);
        n = tab_put(one, sizeof one, n, m->chan, 23);
        n = tab_put(one, sizeof one, n, m->place, 76);
        n = tab_put(one, sizeof one, n, m->avatar, 63);
        n = tab_put(one, sizeof one, n, m->name, 32);
        n = tab_put(one, sizeof one, n, age, 7);
        n = tab_put(one, sizeof one, n, m->text, 120);
        APPEND_LINE();
    }
#undef APPEND_LINE
    return at;
}

int overlay_chat_feed(char *out, int cap, const overlay_chat *c, time_t now)
{
    if (cap > OVERLAY_FEED_MAX)
        cap = OVERLAY_FEED_MAX;
    const int nmsg = c->nmsg < OVERLAY_MSGS ? c->nmsg : OVERLAY_MSGS;
    const int nmen = c->nmen < OVERLAY_MENTIONS ? c->nmen : OVERLAY_MENTIONS;
    int n = -1;
    for (int skip = 0; n < 0 && skip <= nmsg; skip++) // the oldest messages go first
        n = chat_build(out, cap, c, skip, nmen, now);
    for (int m = nmen - 1; n < 0 && m >= 0; m--) // then the oldest mentions
        n = chat_build(out, cap, c, nmsg, m, now);
    if (n < 0) { // not even the header fits
        if (cap > 0)
            out[0] = 0;
        n = 0;
    }
    return n;
}

void overlay_age(char *out, size_t cap, long s)
{
    if (s < 60)
        snprintf(out, cap, "now");
    else if (s < 3600)
        snprintf(out, cap, "%ldm", s / 60);
    else if (s < 86400)
        snprintf(out, cap, "%ldh", s / 3600);
    else
        snprintf(out, cap, "%ldd", s / 86400);
}

void overlay_clock(char *out, size_t cap, const struct tm *tm)
{
    snprintf(out, cap, "%d:%02d %s", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
}

int overlay_mention_add(overlay_chat *c, const overlay_mention *m)
{
    for (int i = 0; i < c->nmen; i++)
        if (strcmp(c->men[i].id, m->id) == 0)
            return 0;
    for (int i = 0; m->dm && i < c->nmen; i++) // a DM is one entry: its latest message
        if (c->men[i].dm && strcmp(c->men[i].chan, m->chan) == 0) {
            memmove(c->men + i, c->men + i + 1, (size_t)(c->nmen - i - 1) * sizeof *c->men);
            c->nmen--;
            break;
        }
    int pos = 0;
    while (pos < c->nmen && c->men[pos].at >= m->at)
        pos++;
    if (pos >= OVERLAY_MENTIONS)
        return 0; // older than the ten kept
    if (c->nmen < OVERLAY_MENTIONS)
        c->nmen++;
    memmove(c->men + pos + 1, c->men + pos, (size_t)(c->nmen - 1 - pos) * sizeof *c->men);
    c->men[pos] = *m;
    return 1;
}

int overlay_msg_add(overlay_chat *c, const overlay_msg *m)
{
    for (int i = 0; i < c->nmsg; i++)
        if (strcmp(c->msgs[i].id, m->id) == 0)
            return 0;
    if (c->nmsg == OVERLAY_MSGS) {
        memmove(c->msgs, c->msgs + 1, (size_t)(OVERLAY_MSGS - 1) * sizeof *c->msgs);
        c->nmsg--;
    }
    c->msgs[c->nmsg++] = *m;
    return 1;
}

int overlay_msg_edit(overlay_chat *c, const char *id, const char *text)
{
    for (int i = 0; i < c->nmsg; i++)
        if (strcmp(c->msgs[i].id, id) == 0) {
            snprintf(c->msgs[i].text, sizeof c->msgs[i].text, "%s", text);
            return 1;
        }
    return 0;
}

int overlay_msg_remove(overlay_chat *c, const char *id)
{
    for (int i = 0; i < c->nmsg; i++)
        if (strcmp(c->msgs[i].id, id) == 0) {
            memmove(c->msgs + i, c->msgs + i + 1, (size_t)(c->nmsg - i - 1) * sizeof *c->msgs);
            c->nmsg--;
            return 1;
        }
    return 0;
}

int overlay_write_file(const char *path, const char *data)
{
    char tmp[256];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return -1;
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return -1;
    const int ok = fputs(data, f) >= 0;
    if (fclose(f) != 0 || !ok || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
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

// The avatars the call shows, for avatar_thread: downloads stay off the feed's loop, which
// writes the beat every second (a slow download made the overlay think the core was gone).
static pthread_mutex_t avatar_mu = PTHREAD_MUTEX_INITIALIZER;
static chat_person avatar_want[MAX_PEOPLE];
static int avatar_count;
// The chat's and the mentions' authors, and a DM's person (chat_tick).
#define EXTRA_PEOPLE (OVERLAY_MSGS + OVERLAY_MENTIONS + 2)
static chat_person extra_want[EXTRA_PEOPLE];
static int extra_count;

static void *avatar_thread(void *arg)
{
    (void)arg;
    static chat_person people[MAX_PEOPLE], extra[EXTRA_PEOPLE];
    for (;;) {
        pthread_mutex_lock(&avatar_mu);
        const int count = avatar_count, more = extra_count;
        memcpy(people, avatar_want, sizeof people);
        memcpy(extra, extra_want, sizeof extra);
        pthread_mutex_unlock(&avatar_mu);
        fetch_avatars(people, count);
        fetch_avatars(extra, more);
        usleep(200000);
    }
    return NULL;
}

static uint64_t mono_ms(void);

static char cur_call[24]; // the call's channel while in one (the feed thread's)

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
    snprintf(cur_call, sizeof cur_call, "%s", state != '-' && channel && strlen(channel) < sizeof cur_call ? channel : "");
    cJSON_Delete(v);
    pthread_mutex_lock(&avatar_mu);
    memcpy(avatar_want, people, sizeof(chat_person) * (size_t)count);
    avatar_count = count;
    pthread_mutex_unlock(&avatar_mu);

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
    if (bt.on && cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(hs, "headset"), "airpods")))
        bt.on = 2; // AirPods connected: Call mode can offer High quality
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
    // The beat: a digit after the state ("3|c7|...") that changes every second, so the line is
    // rewritten while the core lives; the overlay goes inert when it stops (crash, kill).
    const size_t len = strlen(line);
    if (len + 2 <= (size_t)cap) {
        memmove(line + 4, line + 3, len - 2);
        line[3] = (char)('0' + (mono_ms() / 1000) % 10);
    }
}

static int write_line(const char *line)
{
    FILE *f = fopen(OVERLAY_DIR "/text.tmp", "wb");
    if (!f)
        return -1;
    const int ok = fputs(line, f) >= 0;
    return fclose(f) == 0 && ok && rename(OVERLAY_DIR "/text.tmp", OVERLAY_DIR "/text.bin") == 0 ? 0 : -1;
}

static volatile int quitting; // overlay_shutdown: the feed writes no more lines (the beat would undo IDLE_LINE)

// ---- the Chat and Mentions tabs ------------------------------------------------------------
// One chat is shown: an Open chat's (until the call changes), else the call's own, else the channel the
// app showed last. Its latest messages are fetched on a thread when it changes, live ones come from the
// gateway (on_message); the mentions come from Discord's inbox once, then live. chat.bin has both.
static pthread_mutex_t chat_mu = PTHREAD_MUTEX_INITIALIZER;
static overlay_chat chat_state;              // chat_mu: msgs of chat_chan, and the mentions
static char chat_chan[24], loaded_focus[24]; // the channel the messages are of, the message they were fetched around
static char app_chan[24];                    // the channel the app showed last
static char open_chan[24], open_focus[24];   // Open chat's channel and message
static char last_call[24];
static int open_rev; // Open chat commands so far (in the feed: a repeat of the same one still changes it)
static int fetch_busy;
static char fail_chan[24];
static time_t fail_at;

void overlay_note_channel(const char *channel_id)
{
    if (!channel_id || strlen(channel_id) >= sizeof app_chan)
        return;
    pthread_mutex_lock(&chat_mu);
    if (strcmp(app_chan, channel_id) != 0) {
        snprintf(app_chan, sizeof app_chan, "%s", channel_id);
        open_chan[0] = open_focus[0] = 0; // the user opened another: that one holds now
    }
    pthread_mutex_unlock(&chat_mu);
}

// A message as the feed lists it. Not under chat_mu: it takes the chat's own lock.
static void to_msg(const chat_msg *m, overlay_msg *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "%s", m->id);
    snprintf(out->author, sizeof out->author, "%s", m->author);
    snprintf(out->ahash, sizeof out->ahash, "%s", m->avatar);
    snprintf(out->name, sizeof out->name, "%s", m->name);
    snprintf(out->text, sizeof out->text, "%s", m->text);
    struct tm tm;
    time_t at = m->at;
    localtime_r(&at, &tm);
    overlay_clock(out->time, sizeof out->time, &tm);
    out->mine = m->mine;
}

static void to_mention(const chat_msg *m, overlay_mention *out)
{
    memset(out, 0, sizeof *out);
    chat_place p;
    snprintf(out->id, sizeof out->id, "%s", m->id);
    snprintf(out->chan, sizeof out->chan, "%s", m->channel);
    snprintf(out->author, sizeof out->author, "%s", m->author);
    snprintf(out->ahash, sizeof out->ahash, "%s", m->avatar);
    snprintf(out->name, sizeof out->name, "%s", m->name);
    snprintf(out->text, sizeof out->text, "%s", m->text);
    out->at = m->at ? m->at : time(NULL);
    if (!chat_place_of(m->channel, &p))
        snprintf(out->place, sizeof out->place, "Mention");
    else if (p.dm) {
        snprintf(out->place, sizeof out->place, "Direct message");
        out->dm = 1;
    } else
        snprintf(out->place, sizeof out->place, "%s \xc2\xb7 %s%s", p.server, p.voice ? "" : "#", p.name);
}

// A message event from the gateway (gateway thread).
static void on_message(const char *type, cJSON *d, int mention)
{
    const char *chan = cJSON_GetStringValue(cJSON_GetObjectItem(d, "channel_id")),
               *id = cJSON_GetStringValue(cJSON_GetObjectItem(d, "id"));
    if (!chan || !id || strlen(id) >= sizeof(((overlay_msg *)0)->id))
        return;
    if (strcmp(type, "MESSAGE_DELETE") == 0) {
        pthread_mutex_lock(&chat_mu);
        if (strcmp(chan, chat_chan) == 0)
            overlay_msg_remove(&chat_state, id);
        pthread_mutex_unlock(&chat_mu);
        return;
    }
    chat_msg m;
    if (!cJSON_GetObjectItem(d, "content") || !chat_msg_read(d, &m))
        return; // an update without text (an embed arriving) changes nothing here
    overlay_msg om;
    overlay_mention men;
    to_msg(&m, &om);
    const int edit = strcmp(type, "MESSAGE_UPDATE") == 0;
    if (!edit && mention)
        to_mention(&m, &men);
    pthread_mutex_lock(&chat_mu);
    if (strcmp(m.channel, chat_chan) == 0) {
        if (edit)
            overlay_msg_edit(&chat_state, om.id, om.text);
        else
            overlay_msg_add(&chat_state, &om);
    }
    if (!edit && mention && overlay_mention_add(&chat_state, &men))
        chat_state.unseen++;
    pthread_mutex_unlock(&chat_mu);
}

typedef struct {
    char chan[24], around[24];
} fetch_job;

// The latest messages of a channel (around a message, for Open chat), off the feed loop.
static void *fetch_thread(void *arg)
{
    fetch_job job = *(fetch_job *)arg;
    free(arg);
    char token[256], path[160];
    chat_token(token, sizeof token);
    if (job.around[0])
        snprintf(path, sizeof path, "/channels/%s/messages?limit=%d&around=%s", job.chan, OVERLAY_MSGS, job.around);
    else
        snprintf(path, sizeof path, "/channels/%s/messages?limit=%d", job.chan, OVERLAY_MSGS);
    http_response r = http_request("GET", path, NULL, token);
    cJSON *list = r.status == 200 && r.body ? cJSON_Parse(r.body) : NULL;
    free(r.body);
    static overlay_msg got[OVERLAY_MSGS]; // one fetch at a time (fetch_busy)
    int n = 0;
    cJSON *one;
    cJSON_ArrayForEach(one, list) // newest first
    {
        chat_msg m;
        if (n < OVERLAY_MSGS && chat_msg_read(one, &m))
            to_msg(&m, &got[n++]);
    }
    const int ok = cJSON_IsArray(list);
    cJSON_Delete(list);
    pthread_mutex_lock(&chat_mu);
    if (ok) {
        chat_state.nmsg = 0;
        for (int i = n - 1; i >= 0; i--) // oldest first, the ids that came live meanwhile kept once
            overlay_msg_add(&chat_state, &got[i]);
        snprintf(chat_chan, sizeof chat_chan, "%s", job.chan);
        snprintf(loaded_focus, sizeof loaded_focus, "%s", job.around);
    } else {
        snprintf(fail_chan, sizeof fail_chan, "%s", job.chan);
        fail_at = time(NULL);
        LOG("overlay: chat %s not fetched (%d)", job.chan, r.status);
    }
    fetch_busy = 0;
    pthread_mutex_unlock(&chat_mu);
    return NULL;
}

// The inbox, once (retried every 30 s until it comes): the mentions from before the core ran.
static void *mentions_thread(void *arg)
{
    (void)arg;
    char token[256], me[24];
    for (;;) {
        sleep(5);
        if (!chat_me_id(me, sizeof me))
            continue;
        chat_token(token, sizeof token);
        http_response r = http_request("GET", "/users/@me/mentions?limit=25&roles=true&everyone=true", NULL, token);
        cJSON *list = r.status == 200 && r.body ? cJSON_Parse(r.body) : NULL;
        free(r.body);
        if (cJSON_IsArray(list)) {
            cJSON *one;
            cJSON_ArrayForEach(one, list)
            {
                chat_msg m;
                overlay_mention men;
                if (!chat_msg_read(one, &m))
                    continue;
                to_mention(&m, &men);
                pthread_mutex_lock(&chat_mu);
                overlay_mention_add(&chat_state, &men); // not unseen: they were there before
                pthread_mutex_unlock(&chat_mu);
            }
            cJSON_Delete(list);
            return NULL;
        }
        cJSON_Delete(list);
        LOG("overlay: mentions not fetched (%d)", r.status);
        sleep(25);
    }
}

// Open chat (cmd.bin "chat <channel> <message>"): digits only, both.
static void open_chat(const char *what)
{
    char chan[32], msg[32];
    if (sscanf(what, "%31s %31s", chan, msg) != 2 || strspn(chan, "0123456789") != strlen(chan) ||
        strspn(msg, "0123456789") != strlen(msg) || strlen(chan) >= sizeof open_chan || strlen(msg) >= sizeof open_focus) {
        LOG("overlay: chat command not valid");
        return;
    }
    pthread_mutex_lock(&chat_mu);
    snprintf(open_chan, sizeof open_chan, "%s", chan);
    snprintf(open_focus, sizeof open_focus, "%s", msg);
    open_rev++;
    chat_state.unseen = 0; // Open chat is pressed on the Mentions tab: they were seen (a "seen" sent just before is overwritten in cmd.bin)
    fail_chan[0] = 0;
    pthread_mutex_unlock(&chat_mu);
}

// Square on the Chat tab: Open chat's choice ends; the call's chat, or the app's, shows again.
static void chat_back(void)
{
    pthread_mutex_lock(&chat_mu);
    open_chan[0] = open_focus[0] = 0;
    open_rev++;
    fail_chan[0] = 0;
    pthread_mutex_unlock(&chat_mu);
}

static void mentions_seen(void)
{
    pthread_mutex_lock(&chat_mu);
    chat_state.unseen = 0;
    pthread_mutex_unlock(&chat_mu);
}

// One pass of the Chat and Mentions feed (the feed thread): picks the chat, starts a fetch when it changed,
// and writes chat.bin when it did.
static void chat_tick(void)
{
    static overlay_chat snap; // 11 KB: off the stack
    static char feed[OVERLAY_FEED_MAX], last[OVERLAY_FEED_MAX];
    static chat_person extra[EXTRA_PEOPLE];
    const time_t now = time(NULL);
    fetch_job *job = NULL;
    pthread_mutex_lock(&chat_mu);
    if (strcmp(cur_call, last_call) != 0) { // the call changed: Open chat's choice is over
        snprintf(last_call, sizeof last_call, "%s", cur_call);
        open_chan[0] = open_focus[0] = 0;
    }
    const char *target = open_chan[0] ? open_chan : cur_call[0] ? cur_call : app_chan;
    const char *around = open_chan[0] ? open_focus : "";
    if (!target[0]) {
        chat_chan[0] = loaded_focus[0] = 0;
        chat_state.nmsg = 0;
    } else if (!fetch_busy && (strcmp(target, chat_chan) != 0 || strcmp(around, loaded_focus) != 0) &&
               !(strcmp(target, fail_chan) == 0 && now - fail_at < 10) && (job = malloc(sizeof *job))) {
        snprintf(job->chan, sizeof job->chan, "%s", target);
        snprintf(job->around, sizeof job->around, "%s", around);
        fetch_busy = 1;
    }
    memcpy(&snap, &chat_state, sizeof snap);
    const int focused = open_chan[0] && strcmp(open_chan, chat_chan) == 0;
    snprintf(snap.chan, sizeof snap.chan, "%s", chat_chan);
    snprintf(snap.focus, sizeof snap.focus, "%s", focused ? loaded_focus : "");
    snap.rev = open_rev;
    pthread_mutex_unlock(&chat_mu);
    if (job) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t t;
        if (pthread_create(&t, &attr, fetch_thread, job) != 0) {
            free(job);
            pthread_mutex_lock(&chat_mu);
            fetch_busy = 0;
            pthread_mutex_unlock(&chat_mu);
        }
        pthread_attr_destroy(&attr);
    }
    // The header: where this chat is. Avatars as far as the core has fetched them; the rest are wanted.
    chat_place p;
    int more = 0;
    snap.kind = '-';
    if (snap.chan[0]) {
        const int known = chat_place_of(snap.chan, &p);
        snap.kind = known && p.dm ? 'd' : 'c';
        if (!known)
            snprintf(snap.title, sizeof snap.title, "Chat");
        else if (p.dm) {
            snprintf(snap.title, sizeof snap.title, "%s", p.name);
            snprintf(snap.sub, sizeof snap.sub, "Direct message");
        } else {
            snprintf(snap.title, sizeof snap.title, "%s%s", p.voice ? "" : "#", p.name);
            snprintf(snap.sub, sizeof snap.sub, "%s", p.server);
        }
        if (known && p.dm && p.user[0]) {
            chat_person who = {0};
            snprintf(who.id, sizeof who.id, "%s", p.user);
            snprintf(who.avatar, sizeof who.avatar, "%s", p.avatar);
            extra[more++] = who;
            char key[64], path[160];
            struct stat sb;
            avatar_key(&who, key, sizeof key);
            snprintf(path, sizeof path, OVERLAY_DIR "/av/%s.png", key);
            snprintf(snap.icon, sizeof snap.icon, "%s", stat(path, &sb) == 0 ? key : "");
        }
    }
    for (int i = 0; i < snap.nmsg + snap.nmen; i++) {
        const int msg = i < snap.nmsg;
        const char *author = msg ? snap.msgs[i].author : snap.men[i - snap.nmsg].author,
                   *hash = msg ? snap.msgs[i].ahash : snap.men[i - snap.nmsg].ahash;
        char *shown = msg ? snap.msgs[i].avatar : snap.men[i - snap.nmsg].avatar;
        chat_person who = {0};
        snprintf(who.id, sizeof who.id, "%s", author);
        snprintf(who.avatar, sizeof who.avatar, "%s", hash);
        char key[64], path[160];
        struct stat sb;
        avatar_key(&who, key, sizeof key);
        snprintf(path, sizeof path, OVERLAY_DIR "/av/%s.png", key);
        snprintf(shown, 64, "%s", stat(path, &sb) == 0 ? key : "");
        if (!shown[0] && more < EXTRA_PEOPLE)
            extra[more++] = who;
    }
    pthread_mutex_lock(&avatar_mu);
    memcpy(extra_want, extra, sizeof(chat_person) * (size_t)more);
    extra_count = more;
    pthread_mutex_unlock(&avatar_mu);
    overlay_chat_feed(feed, sizeof feed, &snap, now);
    if (!quitting && strcmp(feed, last) != 0 && overlay_write_file(OVERLAY_DIR "/chat.bin", feed) == 0)
        memcpy(last, feed, strlen(feed) + 1);
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
    else if (strcmp(what, "seen") == 0)
        mentions_seen(); // the Mentions tab was opened
    else if (strcmp(what, "chat back") == 0)
        chat_back(); // Square on the Chat tab: the call's chat (or the app's) again
    else if (strncmp(what, "chat ", 5) == 0)
        open_chat(what + 5); // a mention's Open chat
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
    else if (strcmp(what, "bt on") == 0 || strcmp(what, "bt off") == 0)
        r = hs_power(what[4] == 'n'); // Settings' switch, kept for the next start too
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
    static uint64_t last_ok; // ponytail: racy across threads, a stale value only delays 3 s
    FILE *f = fopen(OVERLAY_DIR "/status.bin", "rb");
    const int ok = f && fread(&st, 1, sizeof st, f) == sizeof st;
    if (f)
        fclose(f);
    const uint64_t now = mono_ms();
    if (!ok || st.signature != OVERLAY_MAGIC) // caught mid-write (5 a second): the last good read stands
        return last_ok && now - last_ok < 3000;
    if (st.state == 2 && st.at_ms <= now + 1000 && now - st.at_ms < 3000) {
        last_ok = now;
        return 1;
    }
    last_ok = 0;
    return 0;
}

// A typed message, posted from its own thread: the feed writes the overlay's beat every second
// and a slow request must not stall it.
typedef struct {
    char channel[32];
    char text[];
} outgoing;

static void *post_thread(void *arg)
{
    outgoing *m = arg;
    char token[256], path[64];
    chat_token(token, sizeof token);
    char *json = chat_message_json(m->channel, m->text, NULL, NULL, NULL);
    snprintf(path, sizeof path, "/channels/%s/messages", m->channel);
    http_response r = http_request("POST", path, json, token);
    LOG("overlay: message of %zu bytes to %s -> %d", strlen(m->text), m->channel, r.status);
    free(r.body);
    free(json);
    free(m);
    return NULL;
}

// The overlay's keyboard sent text: to the text chat channel it names (digits, checked when the outbox is
// read), or, with none, to the call's own chat (dropped outside a call).
static void send_text(const char *text, const char *target)
{
    char *state = voice_state_json();
    cJSON *v = state ? cJSON_Parse(state) : NULL;
    free(state);
    const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(v, "state"));
    const char *channel = target[0] ? target : cJSON_GetStringValue(cJSON_GetObjectItem(v, "channel"));
    outgoing *m = NULL;
    if (!channel || !channel[0] || strlen(channel) >= sizeof m->channel ||
        (!target[0] && (!st || !strcmp(st, "idle") || !strcmp(st, "error"))))
        LOG("overlay: message dropped, no chat to send it to");
    else if ((m = malloc(sizeof *m + strlen(text) + 1))) {
        snprintf(m->channel, sizeof m->channel, "%s", channel);
        strcpy(m->text, text);
    }
    cJSON_Delete(v);
    if (!m)
        return;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    if (pthread_create(&t, &attr, post_thread, m) != 0) {
        LOG("overlay: message not sent, no thread");
        free(m);
    }
    pthread_attr_destroy(&attr);
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

// The running overlay is from another build than the app's (an update came): its build id
// (request.bin, after the signature) against assets/overlay.build (the overlay Makefile's).
static int outdated(void)
{
    uint64_t running_build = 0;
    char want[24] = "";
    FILE *f = fopen(OVERLAY_DIR "/request.bin", "rb");
    if (f) {
        uint64_t head[2];
        if (fread(head, 1, sizeof head, f) == sizeof head && head[0] == OVERLAY_MAGIC)
            running_build = head[1];
        fclose(f);
    }
    f = fopen(APP_ASSETS "/overlay.build", "r");
    if (f) {
        if (!fgets(want, sizeof want, f))
            want[0] = 0;
        fclose(f);
    }
    const uint64_t bundled = strtoull(want, NULL, 16);
    return running_build && bundled && running_build != bundled;
}

const char *overlay_state(void)
{
    if (prefs_get("overlay") != 1)
        return "off";
    if (reloading)
        return "reloading";
    if (running())
        return outdated() ? "outdated" : "running";
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
    static char typed[OVERLAY_TEXT_MAX + 1];
    char typed_channel[24];
    uint64_t seen_typed = overlay_read_outbox(OVERLAY_DIR "/send.bin", typed, sizeof typed, typed_channel, sizeof typed_channel);
    int auto_reloaded = 0;
    // Nothing is decided on the defaults: overlay reads 0 until the user's settings load, and a
    // running overlay (from the core before) was stopped as if switched off (2026-11-05).
    while (!prefs_loaded())
        usleep(200000);
    const uint64_t started = mono_ms();
    uint64_t off_since = 0;
    uint64_t down_since = 0, reload_at = 0;
    mkdir(OVERLAY_DIR, 0755);
    mkdir(OVERLAY_DIR "/av", 0755);
    for (;;) {
        const int on = prefs_get("overlay") == 1;
        const uint64_t now = mono_ms();
        off_since = on ? 0 : off_since ? off_since : now; // switched off for a second, not a reload's blip
        const int up = running();
        down_since = up || reloading ? 0 : down_since ? down_since : now; // a reload is not down
        if (on && !launched && down_since && now - down_since > 3000) {
            launch(); // one already running (a core before this one loaded it) is used as it is
            launched = 1;
        } else if (!on && off_since && now - off_since > 1000 && up && !reloading) {
            stop_service(); // switched off in Settings
            launched = 0;
        } else if (on && launched && !auto_reloaded && !reloading && prefs_get("overlay_autoload") == 1 &&
                   now - started < 60000 && down_since && now - down_since > 10000) {
            // Load at start: only while this core starts (it outlives the app), and only after the
            // overlay stayed down for 10 s (the launch was refused: this home screen had one).
            auto_reloaded = 1;
            reload_at = now + 5000;
            LOG("overlay: not loaded at start, reloading the home screen in 5 s");
            notify_system("Discord", "Reloading the home screen in 5 seconds to load the overlay.", NULL);
        } else if (on && up && !auto_reloaded && !reloading && now - started < 60000 && outdated()) {
            // An update came: the home screen still runs the old overlay. Reload it once, with notice.
            auto_reloaded = 1;
            reload_at = now + 5000;
            LOG("overlay: an older build is running, reloading the home screen in 5 s");
            notify_system("Discord", "Reloading the home screen in 5 seconds to update the overlay.", NULL);
        }
        if (reload_at && now >= reload_at) {
            reload_at = 0;
            if (!running() || outdated())
                overlay_reload();
        }
        compose(line, sizeof line, &live_since);
        chat_tick();
        if (!quitting && strcmp(line, last) != 0 && write_line(line) == 0)
            snprintf(last, sizeof last, "%s", line);
        // Any other seq is a new command: the overlay's come from the console's uptime clock,
        // which starts over at a reboot (newer ones were smaller and were all ignored).
        const uint64_t seq = read_command(&c);
        if (seq && seq != seen) {
            seen = seq;
            run_command(c.text);
        }
        const uint64_t typed_seq =
            overlay_read_outbox(OVERLAY_DIR "/send.bin", typed, sizeof typed, typed_channel, sizeof typed_channel);
        if (typed_seq && typed_seq != seen_typed) {
            seen_typed = typed_seq;
            send_text(typed, typed_channel);
        }
        usleep(200000); // the overlay reads every 12 frames (200 ms)
    }
    return NULL;
}

void overlay_shutdown(void)
{
    quitting = 1;
    usleep(250000); // a feed pass already past the check finishes (it sleeps 200 ms between)
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
    pthread_create(&t, &attr, avatar_thread, NULL);
    chat_set_message_sink(on_message);
    pthread_create(&t, &attr, mentions_thread, NULL);
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

void overlay_note_channel(const char *channel_id)
{
    (void)channel_id;
}

const char *overlay_state(void)
{
    return "off";
}
#endif
