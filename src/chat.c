#include "chat.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVENT_RING 256

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char token[256];
static cJSON *tree; // trimmed READY
static char *events[EVENT_RING];
static long next_event; // sequence number of the next event

void chat_set_token(const char *t)
{
    pthread_mutex_lock(&lock);
    snprintf(token, sizeof token, "%s", t);
    pthread_mutex_unlock(&lock);
}

const char *chat_token(void)
{
    return token; // written once per login before the gateway starts
}

static void copy(cJSON *dst, cJSON *src, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(src, key);
    cJSON_AddItemToObject(dst, key, v ? cJSON_Duplicate(v, 1) : cJSON_CreateNull());
}

static cJSON *trim_user(cJSON *u)
{
    cJSON *o = cJSON_CreateObject();
    copy(o, u, "id");
    copy(o, u, "username");
    copy(o, u, "global_name");
    copy(o, u, "avatar");
    return o;
}

static cJSON *find_user(cJSON *users, const char *id)
{
    cJSON *u;
    cJSON_ArrayForEach(u, users)
    {
        const char *uid = cJSON_GetStringValue(cJSON_GetObjectItem(u, "id"));
        if (uid && id && strcmp(uid, id) == 0)
            return u;
    }
    return NULL;
}

void chat_load_ready(cJSON *d)
{
    cJSON *t = cJSON_CreateObject(), *users = cJSON_GetObjectItem(d, "users"), *g, *c;
    cJSON_AddItemToObject(t, "me", trim_user(cJSON_GetObjectItem(d, "user")));

    cJSON *guilds = cJSON_AddArrayToObject(t, "guilds");
    cJSON_ArrayForEach(g, cJSON_GetObjectItem(d, "guilds"))
    {
        cJSON *og = cJSON_CreateObject(), *props = cJSON_GetObjectItem(g, "properties");
        copy(og, g, "id");
        copy(og, props ? props : g, "name");
        copy(og, props ? props : g, "icon");
        cJSON *chans = cJSON_AddArrayToObject(og, "channels");
        cJSON_ArrayForEach(c, cJSON_GetObjectItem(g, "channels"))
        {
            int type = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(c, "type"));
            if (type != 0 && type != 4 && type != 5) // text, category, announcement
                continue;
            cJSON *oc = cJSON_CreateObject();
            copy(oc, c, "id");
            copy(oc, c, "name");
            copy(oc, c, "type");
            copy(oc, c, "position");
            copy(oc, c, "parent_id");
            cJSON_AddItemToArray(chans, oc);
        }
        cJSON_AddItemToArray(guilds, og);
    }

    // DM names come from the deduplicated users list (capability bit 4).
    cJSON *dms = cJSON_AddArrayToObject(t, "dms");
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(d, "private_channels"))
    {
        cJSON *oc = cJSON_CreateObject(), *r;
        copy(oc, c, "id");
        copy(oc, c, "type");
        copy(oc, c, "last_message_id");
        char name[256] = "";
        cJSON_ArrayForEach(r, cJSON_GetObjectItem(c, "recipient_ids"))
        {
            cJSON *u = find_user(users, cJSON_GetStringValue(r));
            const char *n = u ? cJSON_GetStringValue(cJSON_GetObjectItem(u, "global_name")) : NULL;
            if (!n && u)
                n = cJSON_GetStringValue(cJSON_GetObjectItem(u, "username"));
            size_t len = strlen(name);
            snprintf(name + len, sizeof name - len, "%s%s", len ? ", " : "", n ? n : "?");
        }
        const char *group = cJSON_GetStringValue(cJSON_GetObjectItem(c, "name"));
        cJSON_AddStringToObject(oc, "name", group && *group ? group : name);
        cJSON_AddItemToArray(dms, oc);
    }

    pthread_mutex_lock(&lock);
    cJSON_Delete(tree);
    tree = t;
    pthread_mutex_unlock(&lock);
}

void chat_event(const char *type, cJSON *d)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "t", type);
    copy(e, d, "id");
    copy(e, d, "channel_id");
    copy(e, d, "guild_id");
    if (strcmp(type, "MESSAGE_DELETE") != 0) {
        copy(e, d, "content");
        copy(e, d, "timestamp");
        copy(e, d, "edited_timestamp");
        copy(e, d, "nonce");
        copy(e, d, "attachments");
        cJSON *a = cJSON_GetObjectItem(d, "author");
        if (a)
            cJSON_AddItemToObject(e, "author", trim_user(a));
    }
    pthread_mutex_lock(&lock);
    cJSON_AddNumberToObject(e, "seq", (double)next_event);
    char *s = cJSON_PrintUnformatted(e);
    free(events[next_event % EVENT_RING]);
    events[next_event % EVENT_RING] = s;
    next_event++;
    pthread_mutex_unlock(&lock);
    cJSON_Delete(e);
}

char *chat_tree_json(void)
{
    pthread_mutex_lock(&lock);
    char *s = tree ? cJSON_PrintUnformatted(tree) : NULL;
    pthread_mutex_unlock(&lock);
    return s;
}

char *chat_events_json(long since)
{
    pthread_mutex_lock(&lock);
    long first = next_event > EVENT_RING ? next_event - EVENT_RING : 0;
    if (since < first)
        since = first;
    size_t cap = 64;
    for (long i = since; i < next_event; i++)
        cap += strlen(events[i % EVENT_RING]) + 1;
    char *out = malloc(cap);
    if (out) {
        size_t n = (size_t)snprintf(out, cap, "{\"next\":%ld,\"events\":[", next_event);
        for (long i = since; i < next_event; i++)
            n += (size_t)snprintf(out + n, cap - n, "%s%s", i > since ? "," : "", events[i % EVENT_RING]);
        snprintf(out + n, cap - n, "]}");
    }
    pthread_mutex_unlock(&lock);
    return out;
}
