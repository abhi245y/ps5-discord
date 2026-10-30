// SPDX-License-Identifier: AGPL-3.0-or-later
#include "playing.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

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
    return r;
}

#ifdef __PROSPERO__
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceGetAppTitleId(int app_id, char *title_id);
#endif

void playing_now(char *name, size_t cap)
{
    name[0] = 0;
#ifdef __PROSPERO__
    static char last_title[16], last_name[128];
    char title[16] = "";
    int id = sceSystemServiceGetAppIdOfRunningBigApp();
    if (id < 0 || sceSystemServiceGetAppTitleId(id, title) != 0 || strcmp(title, OWN_TITLE) == 0 ||
        strncmp(title, "NPXS", 4) == 0)
        return;
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
