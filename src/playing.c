// SPDX-License-Identifier: AGPL-3.0-or-later
#include "playing.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include "http.h"
#include "log.h"

#define APPMETA "/system_data/priv/appmeta" // the console's copy of each title's param file
#define OWN_TITLE "PPSA99072"

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = malloc(65537); // param files are a few KB
    size_t n = buf ? fread(buf, 1, 65536, f) : 0;
    fclose(f);
    if (buf)
        buf[n] = 0;
    *len = n;
    return buf;
}

// A name cut at cap bytes may end inside a UTF-8 character, and Discord drops a frame with
// broken UTF-8: drop the partial character.
static void utf8_cut(char *s)
{
    size_t n = strlen(s), i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80)
        i--;
    if (i == 0)
        return;
    unsigned char lead = (unsigned char)s[i - 1];
    size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (i - 1) < want)
        s[i - 1] = 0;
}

static int from_json(const char *data, char *name, size_t cap)
{
    cJSON *j = cJSON_Parse(data);
    cJSON *loc = cJSON_GetObjectItem(j, "localizedParameters");
    cJSON *lang = cJSON_GetObjectItem(loc, "en-US");
    const char *def = cJSON_GetStringValue(cJSON_GetObjectItem(loc, "defaultLanguage"));
    if (!lang && def)
        lang = cJSON_GetObjectItem(loc, def);
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(lang, "titleName"));
    int ok = title && title[0];
    if (ok)
        snprintf(name, cap, "%s", title);
    cJSON_Delete(j);
    return ok ? 0 : -1;
}

static uint32_t le32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

// param.sfo: a header, 16-byte entries (key offset, format, length, max, data offset).
static int from_sfo(const unsigned char *b, size_t n, char *name, size_t cap)
{
    if (n < 20 || memcmp(b, "\0PSF", 4) != 0)
        return -1;
    size_t keys = le32(b + 8), data = le32(b + 12), count = le32(b + 16);
    for (size_t i = 0; i < count && 20 + 16 * (i + 1) <= n; i++) {
        const unsigned char *e = b + 20 + 16 * i;
        size_t key = keys + (e[0] | e[1] << 8), len = le32(e + 4), at = data + le32(e + 12);
        if (key > n - 6 || at > n || len > n - at)
            return -1;
        if (memcmp(b + key, "TITLE", 6) == 0) {
            snprintf(name, cap, "%.*s", (int)strnlen((const char *)b + at, len), (const char *)b + at);
            return name[0] ? 0 : -1;
        }
    }
    return -1;
}

int playing_title_name(const char *dir, const char *title, char *name, size_t cap)
{
    char path[256];
    size_t len;
    snprintf(path, sizeof path, "%s/%s/param.json", dir, title);
    char *buf = slurp(path, &len);
    int r = buf ? from_json(buf, name, cap) : -1;
    if (!buf) {
        snprintf(path, sizeof path, "%s/%s/param.sfo", dir, title);
        if ((buf = slurp(path, &len)))
            r = from_sfo((const unsigned char *)buf, len, name, cap);
    }
    free(buf);
    if (r == 0)
        utf8_cut(name);
    return r;
}

#ifdef __PROSPERO__
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceGetAppTitleId(int app_id, char *title_id);
#endif

void playing_now(char title[16], char *name, size_t cap)
{
    name[0] = 0;
    title[0] = 0;
#ifdef __PROSPERO__
    static char last_title[16], last_name[128];
    int id = sceSystemServiceGetAppIdOfRunningBigApp();
    if (id < 0 || sceSystemServiceGetAppTitleId(id, title) != 0 || strcmp(title, OWN_TITLE) == 0 ||
        strncmp(title, "NPXS", 4) == 0) {
        title[0] = 0;
        return;
    }
    if (strcmp(title, last_title) != 0) {
        snprintf(last_title, sizeof last_title, "%s", title);
        if (playing_title_name(APPMETA, title, last_name, sizeof last_name) != 0)
            last_name[0] = 0; // no "Playing PPSA01325"
    }
    snprintf(name, cap, "%s", last_name);
#else
    (void)cap;
#endif
}

int playing_icon_url(const char *lookup_json, char *out, size_t cap)
{
    cJSON *j = cJSON_Parse(lookup_json);
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "metadata"), "icon"));
    int ok = cJSON_IsTrue(cJSON_GetObjectItem(j, "success")) && url && strncmp(url, "https://", 8) == 0 &&
             (size_t)snprintf(out, cap, "%s", url) < cap;
    cJSON_Delete(j);
    return ok ? 0 : -1;
}

int playing_asset(const char *assets_json, char *out, size_t cap)
{
    cJSON *j = cJSON_Parse(assets_json);
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(j, 0), "external_asset_path"));
    int ok = path && path[0] && (size_t)snprintf(out, cap, "mp:%s", path) < cap;
    cJSON_Delete(j);
    return ok ? 0 : -1;
}

static pthread_mutex_t cover_lock = PTHREAD_MUTEX_INITIALIZER;
static char cover_title[16], cover[512]; // cover_title set: looked up or being looked up

typedef struct {
    char title[16];
    char *token;
} lookup;

static void *look_up(void *arg)
{
    lookup *l = arg;
    char path[64], icon[400] = "", asset[512] = "";
    // PS5 titles on prosperopatches, PS4 (CUSA) on orbispatches: the same API.
    const char *host = strncmp(l->title, "CUSA", 4) == 0 ? "orbispatches.com" : "prosperopatches.com";
    snprintf(path, sizeof path, "/api/lookup?titleid=%s", l->title);
    http_response r = http_get(host, path);
    if (r.status == 200 && playing_icon_url(r.body, icon, sizeof icon) == 0) {
        cJSON *urls = cJSON_CreateObject();
        cJSON_AddItemToArray(cJSON_AddArrayToObject(urls, "urls"), cJSON_CreateString(icon));
        char *body = cJSON_PrintUnformatted(urls);
        cJSON_Delete(urls);
        http_response a = http_post_json("/applications/" PLAYING_APP_ID "/external-assets", body, l->token);
        if (a.status != 200 || playing_asset(a.body, asset, sizeof asset) != 0)
            LOG("playing: no cover from Discord (%d)", a.status);
        free(a.body);
        free(body);
    } else {
        LOG("playing: no cover for %s (%d)", l->title, r.status);
    }
    free(r.body);
    pthread_mutex_lock(&cover_lock);
    if (strcmp(cover_title, l->title) == 0)
        snprintf(cover, sizeof cover, "%s", asset);
    pthread_mutex_unlock(&cover_lock);
    free(l->token);
    free(l);
    return NULL;
}

void playing_cover(const char *title, const char *token, char *out, size_t cap)
{
    out[0] = 0;
    if (!PLAYING_APP_ID[0] || !title[0])
        return;
    pthread_mutex_lock(&cover_lock);
    int start = strcmp(cover_title, title) != 0;
    if (start) { // ponytail: the last title only; a cache per title if people switch games a lot
        snprintf(cover_title, sizeof cover_title, "%s", title);
        cover[0] = 0;
    } else {
        snprintf(out, cap, "%s", cover);
    }
    pthread_mutex_unlock(&cover_lock);
    lookup *l = start ? malloc(sizeof *l) : NULL;
    if (!l)
        return;
    snprintf(l->title, sizeof l->title, "%s", title);
    l->token = strdup(token);
    pthread_t t;
    if (!l->token || pthread_create(&t, NULL, look_up, l) != 0) {
        free(l->token);
        free(l);
        return;
    }
    pthread_detach(t);
}
