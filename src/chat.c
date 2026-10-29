#include "chat.h"
#include "json_num.h"
#include "log.h"
#include "notify.h"
#include "prefs.h"

#include <pthread.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EVENT_RING 256

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_ready = PTHREAD_COND_INITIALIZER; // a new event, for long polls
static char token[256];
// The live account, served whole by /api/chat:
//   me, guilds[] (text channels with last_message_id, voice rooms), dms[],
//   users{id: user}, friends[ids], presence{id: {status, activity}},
//   voice{channel_id: [states]}, reads{channel_id: {last_read, mentions}},
//   calls{channel_id: {ringing[ids], message_id}} (DM calls in progress or ringing)
static cJSON *tree;
// Bumped on every change to tree (/api/chat?v= answers 304 while it holds), seeded from the
// clock at READY so a restarted core never repeats a number the app holds.
static long tree_version;
static time_t pruned; // when users was last cut down to the people still referenced
static char *events[EVENT_RING];
static long next_event; // sequence number of the next event

void chat_set_token(const char *t)
{
    pthread_mutex_lock(&lock);
    snprintf(token, sizeof token, "%s", t);
    pthread_mutex_unlock(&lock);
}

void chat_reset(void)
{
    pthread_mutex_lock(&lock);
    cJSON_Delete(tree);
    tree = NULL;
    token[0] = 0;
    pthread_mutex_unlock(&lock);
}

void chat_token(char *out, size_t cap)
{
    pthread_mutex_lock(&lock); // chat_reset clears it from the main thread
    snprintf(out, cap, "%s", token);
    pthread_mutex_unlock(&lock);
}

static void copy(cJSON *dst, cJSON *src, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(src, key);
    cJSON_AddItemToObject(dst, key, v ? cJSON_Duplicate(v, 1) : cJSON_CreateNull());
}

static const char *str(cJSON *o, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(o, key));
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

// Adds or refreshes a user in tree.users (lock held).
static void remember_user(cJSON *u)
{
    const char *id = str(u, "id");
    cJSON *users = cJSON_GetObjectItem(tree, "users");
    if (!id || !users)
        return;
    cJSON_DeleteItemFromObject(users, id);
    cJSON_AddItemToObject(users, id, trim_user(u));
}

static void set_presence(const char *user_id, const char *status, cJSON *activities)
{
    cJSON *presence = cJSON_GetObjectItem(tree, "presence");
    if (!user_id || !presence)
        return;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "status", status ? status : "offline");
    // The first "Playing" (type 0) activity names the game; custom status (type 4) is skipped.
    cJSON *a;
    cJSON_ArrayForEach(a, activities)
    {
        if ((int)json_int(a, "type", 0) == 0 && str(a, "name"))
        {
            cJSON_AddStringToObject(p, "activity", str(a, "name"));
            break;
        }
    }
    cJSON_DeleteItemFromObject(presence, user_id);
    cJSON_AddItemToObject(presence, user_id, p);
}

// Moves a user's voice state to channel_id (NULL: left voice). Lock held.
static void set_voice(cJSON *state)
{
    cJSON *voice = cJSON_GetObjectItem(tree, "voice");
    const char *user_id = str(state, "user_id");
    if (!voice || !user_id)
        return;
    cJSON *list;
    cJSON_ArrayForEach(list, voice)
    {
        for (int i = cJSON_GetArraySize(list) - 1; i >= 0; i--)
        {
            const char *uid = str(cJSON_GetArrayItem(list, i), "user_id");
            if (uid && strcmp(uid, user_id) == 0)
                cJSON_DeleteItemFromArray(list, i);
        }
    }
    const char *channel = str(state, "channel_id");
    cJSON *member = cJSON_GetObjectItem(state, "member");
    if (member && cJSON_GetObjectItem(member, "user"))
        remember_user(cJSON_GetObjectItem(member, "user"));
    const char *me = str(cJSON_GetObjectItem(tree, "me"), "id");
    if (me && strcmp(me, user_id) == 0)
    {
        cJSON_DeleteItemFromObject(tree, "my_voice");
        cJSON_AddItemToObject(tree, "my_voice", channel ? cJSON_CreateString(channel) : cJSON_CreateNull());
    }
    if (!channel)
        return;
    list = cJSON_GetObjectItem(voice, channel);
    if (!list)
        list = cJSON_AddArrayToObject(voice, channel);
    cJSON *s = cJSON_CreateObject();
    copy(s, state, "user_id");
    copy(s, state, "guild_id");
    copy(s, state, "self_mute");
    copy(s, state, "self_deaf");
    copy(s, state, "mute");
    copy(s, state, "deaf");
    cJSON_AddItemToArray(list, s);
}

// The text channel or DM with this id, wherever it lives. Lock held.
static cJSON *find_channel(const char *id)
{
    cJSON *g, *c;
    if (!id)
        return NULL;
    cJSON_ArrayForEach(g, cJSON_GetObjectItem(tree, "guilds"))
        cJSON_ArrayForEach(c, cJSON_GetObjectItem(g, "channels"))
            if (str(c, "id") && strcmp(str(c, "id"), id) == 0)
                return c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(tree, "dms"))
        if (str(c, "id") && strcmp(str(c, "id"), id) == 0)
            return c;
    return NULL;
}

static cJSON *read_state(const char *channel_id)
{
    cJSON *reads = cJSON_GetObjectItem(tree, "reads");
    cJSON *r = cJSON_GetObjectItem(reads, channel_id);
    if (!r)
    {
        r = cJSON_AddObjectToObject(reads, channel_id);
        cJSON_AddNullToObject(r, "last_read");
        cJSON_AddNumberToObject(r, "mentions", 0);
    }
    return r;
}

static void set_string(cJSON *o, const char *key, const char *value)
{
    cJSON_DeleteItemFromObject(o, key);
    cJSON_AddItemToObject(o, key, value ? cJSON_CreateString(value) : cJSON_CreateNull());
}

// Discord permission bits used here.
#define PERM_ADMINISTRATOR (1ull << 3)
#define PERM_VIEW_CHANNEL (1ull << 10)
#define PERM_SEND_MESSAGES (1ull << 11)

static uint64_t perm_bits(cJSON *o, const char *key)
{
    const char *v = str(o, key); // permissions are decimal strings (64-bit)
    return v ? strtoull(v, NULL, 10) : 0;
}

static int has_role(cJSON *roles, const char *id)
{
    cJSON *r;
    cJSON_ArrayForEach(r, roles)
        if (cJSON_IsString(r) && strcmp(r->valuestring, id) == 0)
            return 1;
    return 0;
}

// The account's guild-wide permissions: @everyone plus its roles (Discord's
// documented order). my_roles is the member's role id list.
static uint64_t guild_permissions(cJSON *g, const char *me, cJSON *my_roles)
{
    const char *gid = str(g, "id");
    cJSON *props = cJSON_GetObjectItem(g, "properties");
    const char *owner = str(props ? props : g, "owner_id");
    if (me && owner && strcmp(me, owner) == 0)
        return ~0ull;
    uint64_t p = 0;
    cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItem(g, "roles"))
    {
        const char *id = str(r, "id");
        if (id && gid && (strcmp(id, gid) == 0 || has_role(my_roles, id)))
            p |= perm_bits(r, "permissions");
    }
    return (p & PERM_ADMINISTRATOR) ? ~0ull : p;
}

// Channel overwrites on top: @everyone, then all roles together, then the member.
static uint64_t channel_permissions(uint64_t base, cJSON *c, const char *gid, const char *me, cJSON *my_roles)
{
    if (base == ~0ull)
        return base;
    uint64_t allow = 0, deny = 0;
    cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItem(c, "permission_overwrites"))
        if (gid && str(o, "id") && strcmp(str(o, "id"), gid) == 0)
            base = (base & ~perm_bits(o, "deny")) | perm_bits(o, "allow");
    cJSON_ArrayForEach(o, cJSON_GetObjectItem(c, "permission_overwrites"))
    {
        const char *id = str(o, "id");
        int type = (int)json_int(o, "type", 0);
        if (id && type == 0 && strcmp(id, gid ? gid : "") != 0 && has_role(my_roles, id)) {
            allow |= perm_bits(o, "allow");
            deny |= perm_bits(o, "deny");
        }
    }
    base = (base & ~deny) | allow;
    cJSON_ArrayForEach(o, cJSON_GetObjectItem(c, "permission_overwrites"))
    {
        const char *id = str(o, "id");
        int type = (int)json_int(o, "type", 0);
        if (id && me && type == 1 && strcmp(id, me) == 0)
            base = (base & ~perm_bits(o, "deny")) | perm_bits(o, "allow");
    }
    return base;
}

// The member's roles in guild index i: merged_members runs parallel to guilds.
static cJSON *my_roles_in(cJSON *d, int i, const char *me)
{
    cJSON *m, *members = cJSON_GetArrayItem(cJSON_GetObjectItem(d, "merged_members"), i);
    cJSON_ArrayForEach(m, members)
        if (me && str(m, "user_id") && strcmp(str(m, "user_id"), me) == 0)
            return cJSON_GetObjectItem(m, "roles");
    return NULL;
}

// Adds (or replaces) one guild channel in og, if this account can see it:
// text-like ones under "channels" (with can_send), voice and stage under "rooms".
static void put_channel(cJSON *og, cJSON *c)
{
    const char *id = str(c, "id");
    const char *lists[] = {"channels", "rooms"};
    for (int l = 0; l < 2; l++) {
        cJSON *list = cJSON_GetObjectItem(og, lists[l]), *e;
        int i = 0;
        cJSON_ArrayForEach(e, list)
        {
            if (id && str(e, "id") && strcmp(str(e, "id"), id) == 0) {
                cJSON_DeleteItemFromArray(list, i);
                break;
            }
            ++i;
        }
    }
    const char *me = str(cJSON_GetObjectItem(tree, "me"), "id");
    const uint64_t base = perm_bits(og, "_base");
    const uint64_t perms = channel_permissions(base, c, str(og, "id"), me, cJSON_GetObjectItem(og, "_my_roles"));
    int type = (int)json_int(c, "type", 0);
    // ponytail: categories are kept regardless; their children carry their own overwrites.
    if (type != 4 && !(perms & PERM_VIEW_CHANNEL))
        return; // hidden from this account, as in the official client
    cJSON *oc = cJSON_CreateObject();
    copy(oc, c, "id");
    copy(oc, c, "name");
    copy(oc, c, "type");
    copy(oc, c, "position");
    copy(oc, c, "parent_id");
    if (type == 2 || type == 13) // voice and stage
        cJSON_AddItemToArray(cJSON_GetObjectItem(og, "rooms"), oc);
    else if (type == 0 || type == 4 || type == 5) // text, category, announcement
    {
        copy(oc, c, "topic");
        copy(oc, c, "last_message_id");
        cJSON_AddBoolToObject(oc, "can_send", (perms & PERM_SEND_MESSAGES) != 0);
        cJSON_AddItemToArray(cJSON_GetObjectItem(og, "channels"), oc);
    }
    else
        cJSON_Delete(oc);
}

static cJSON *guild_of(const char *id)
{
    cJSON *g;
    cJSON_ArrayForEach(g, cJSON_GetObjectItem(tree, "guilds"))
        if (id && str(g, "id") && strcmp(str(g, "id"), id) == 0)
            return g;
    return NULL;
}

// A guild's own emoji or stickers, trimmed: {id, name, animated, available} or
// {id, name, format_type, available}. Lock held.
static void put_emojis(cJSON *og, cJSON *src, const char *key)
{
    cJSON *list = cJSON_CreateArray(), *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(src, key))
    {
        cJSON *o = cJSON_CreateObject();
        copy(o, e, "id");
        copy(o, e, "name");
        copy(o, e, strcmp(key, "emojis") == 0 ? "animated" : "format_type");
        copy(o, e, "available");
        cJSON_AddItemToArray(list, o);
    }
    cJSON_DeleteItemFromObject(og, key);
    cJSON_AddItemToObject(og, key, list);
}

// One guild as the tree keeps it: visible channels (with can_send), rooms,
// and its voice states recorded. my_roles: the account's role ids there.
static cJSON *guild_tree(cJSON *g, const char *me, cJSON *my_roles)
{
    cJSON *c;
    const uint64_t base = guild_permissions(g, me, my_roles);
    cJSON *og = cJSON_CreateObject(), *props = cJSON_GetObjectItem(g, "properties");
    copy(og, g, "id");
    copy(og, props ? props : g, "name");
    copy(og, props ? props : g, "icon");
    copy(og, props ? props : g, "banner");
    copy(og, props ? props : g, "splash");
    copy(og, g, "member_count");
    copy(og, props ? props : g, "default_message_notifications"); // 0 all messages, 1 mentions
    put_emojis(og, cJSON_GetObjectItem(g, "emojis") ? g : props, "emojis");
    put_emojis(og, cJSON_GetObjectItem(g, "stickers") ? g : props, "stickers");
    cJSON_AddArrayToObject(og, "channels");
    cJSON_AddArrayToObject(og, "rooms");
    // Role names by id, so <@&id> in a message reads as the role.
    cJSON *names = cJSON_AddObjectToObject(og, "roles"), *role;
    cJSON_ArrayForEach(role, cJSON_GetObjectItem(g, "roles"))
        if (str(role, "id") && str(role, "name"))
            cJSON_AddStringToObject(names, str(role, "id"), str(role, "name"));
    // Kept for channel events later (CHANNEL_CREATE/UPDATE need the same inputs).
    char base_text[24];
    snprintf(base_text, sizeof base_text, "%llu", (unsigned long long)base);
    cJSON_AddStringToObject(og, "_base", base_text);
    cJSON_AddItemToObject(og, "_my_roles", my_roles ? cJSON_Duplicate(my_roles, 1) : cJSON_CreateArray());
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(g, "channels"))
        put_channel(og, c);
    cJSON *vs;
    cJSON_ArrayForEach(vs, cJSON_GetObjectItem(g, "voice_states"))
    {
        if (!cJSON_GetObjectItem(vs, "guild_id"))
            cJSON_AddItemToObject(vs, "guild_id", cJSON_Duplicate(cJSON_GetObjectItem(g, "id"), 1));
        set_voice(vs);
    }
    return og;
}

// GUILD_CREATE (joined or created a server) and GUILD_DELETE (left it).
static void guild_changed(cJSON *d, int created)
{
    cJSON *guilds = cJSON_GetObjectItem(tree, "guilds"), *g;
    const char *id = str(d, "id");
    int i = 0;
    cJSON_ArrayForEach(g, guilds)
    {
        if (id && str(g, "id") && strcmp(str(g, "id"), id) == 0)
        {
            cJSON_DeleteItemFromArray(guilds, i);
            break;
        }
        ++i;
    }
    if (!created || cJSON_IsTrue(cJSON_GetObjectItem(d, "unavailable")))
        return;
    const char *me = str(cJSON_GetObjectItem(tree, "me"), "id");
    cJSON *my_roles = NULL, *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(d, "members"))
        if (me && str(cJSON_GetObjectItem(m, "user"), "id") && strcmp(str(cJSON_GetObjectItem(m, "user"), "id"), me) == 0)
            my_roles = cJSON_GetObjectItem(m, "roles");
    cJSON_AddItemToArray(guilds, guild_tree(d, me, my_roles));
}

static void put_notify(cJSON *e);

static void load_ready(cJSON *d)
{
    cJSON *t = cJSON_CreateObject(), *g, *c;
    tree = t; // remember_user and friends write here; the lock is held by the caller
    cJSON_AddItemToObject(t, "me", trim_user(cJSON_GetObjectItem(d, "user")));
    copy(cJSON_GetObjectItem(t, "me"), cJSON_GetObjectItem(d, "user"), "premium_type"); // Nitro: emoji from anywhere
    cJSON_AddObjectToObject(t, "users");
    cJSON_AddObjectToObject(t, "presence");
    cJSON_AddObjectToObject(t, "voice");
    cJSON_AddObjectToObject(t, "reads");
    cJSON_AddObjectToObject(t, "calls");
    cJSON_AddNullToObject(t, "my_voice");
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(d, "users"))
        remember_user(c);
    remember_user(cJSON_GetObjectItem(d, "user"));

    cJSON *guilds = cJSON_AddArrayToObject(t, "guilds");
    const char *me = str(cJSON_GetObjectItem(d, "user"), "id");
    int gi = -1;
    cJSON_ArrayForEach(g, cJSON_GetObjectItem(d, "guilds"))
    {
        ++gi; // merged_members follows READY's guild order, unavailable ones included
        if (cJSON_IsTrue(cJSON_GetObjectItem(g, "unavailable")))
            continue; // only an id; GUILD_CREATE brings it when it is back
        cJSON_AddItemToArray(guilds, guild_tree(g, me, my_roles_in(d, gi, me)));
    }

    cJSON *dms = cJSON_AddArrayToObject(t, "dms");
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(d, "private_channels"))
    {
        cJSON *oc = cJSON_CreateObject();
        copy(oc, c, "id");
        copy(oc, c, "type");
        copy(oc, c, "last_message_id");
        copy(oc, c, "recipient_ids");
        copy(oc, c, "name"); // group DMs only
        cJSON_AddItemToArray(dms, oc);
    }

    cJSON *friends = cJSON_AddArrayToObject(t, "friends");
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(d, "relationships"))
    {
        if ((int)json_int(c, "type", 0) != 1) // 1 = friend
            continue;
        const char *id = str(c, "id");
        if (!id)
            id = str(cJSON_GetObjectItem(c, "user"), "id");
        if (cJSON_GetObjectItem(c, "user"))
            remember_user(cJSON_GetObjectItem(c, "user"));
        if (id)
            cJSON_AddItemToArray(friends, cJSON_CreateString(id));
    }

    // Discord's own notification settings: an object with entries (or, older, a bare list).
    cJSON_AddObjectToObject(t, "notify");
    cJSON *ugs = cJSON_GetObjectItem(d, "user_guild_settings");
    cJSON *entries = cJSON_IsArray(ugs) ? ugs : cJSON_GetObjectItem(ugs, "entries");
    cJSON_ArrayForEach(c, entries)
        put_notify(c);

    cJSON_ArrayForEach(c, cJSON_GetObjectItem(cJSON_GetObjectItem(d, "read_state"), "entries"))
    {
        const char *id = str(c, "id");
        if (!id)
            continue;
        cJSON *r = read_state(id);
        set_string(r, "last_read", str(c, "last_message_id"));
        cJSON_DeleteItemFromObject(r, "mentions");
        const int64_t count = json_int(c, "mention_count", 0);
        cJSON_AddNumberToObject(r, "mentions", count >= 0 && count <= INT_MAX ? (double)count : 0);
    }
}

static void load_supplemental(cJSON *d)
{
    cJSON *p, *g, *vs;
    cJSON_ArrayForEach(p, cJSON_GetObjectItem(cJSON_GetObjectItem(d, "merged_presences"), "friends"))
        set_presence(str(p, "user_id"), str(p, "status"), cJSON_GetObjectItem(p, "activities"));
    cJSON_ArrayForEach(g, cJSON_GetObjectItem(d, "guilds"))
    {
        cJSON_ArrayForEach(vs, cJSON_GetObjectItem(g, "voice_states"))
        {
            if (!cJSON_GetObjectItem(vs, "guild_id"))
                cJSON_AddItemToObject(vs, "guild_id", cJSON_Duplicate(cJSON_GetObjectItem(g, "id"), 1));
            set_voice(vs);
        }
    }
}

// Adds e to the event ring and wakes the long polls. Lock held; e stays the caller's.
static void store_event(cJSON *e)
{
    cJSON_AddNumberToObject(e, "seq", (double)next_event);
    char *s = cJSON_PrintUnformatted(e);
    free(events[next_event % EVENT_RING]);
    events[next_event % EVENT_RING] = s;
    next_event++;
    pthread_cond_broadcast(&event_ready);
}

static void push_event(const char *type, cJSON *d)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "t", type);
    copy(e, d, "id");
    copy(e, d, "channel_id");
    copy(e, d, "guild_id");
    if (strncmp(type, "MESSAGE_REACTION_", 17) == 0) {
        copy(e, d, "message_id");
        copy(e, d, "user_id");
        cJSON *em = cJSON_GetObjectItem(d, "emoji");
        cJSON *oe = cJSON_AddObjectToObject(e, "emoji");
        copy(oe, em, "name");
        copy(oe, em, "id");
    }
    else if (strcmp(type, "MESSAGE_DELETE") != 0)
    {
        copy(e, d, "content");
        copy(e, d, "timestamp");
        copy(e, d, "edited_timestamp");
        copy(e, d, "nonce");
        copy(e, d, "attachments");
        copy(e, d, "mentions");
        copy(e, d, "referenced_message");
        copy(e, d, "type");
        copy(e, d, "sticker_items");
        if (cJSON_GetObjectItem(d, "notify")) { // MESSAGE_CREATE: worth a banner (message_created)
            copy(e, d, "notify");
            copy(e, d, "quiet");
        }
        copy(e, d, "reactions");
        copy(e, d, "message_snapshots");
        cJSON *embeds = cJSON_AddArrayToObject(e, "embeds"), *em; // titles only: embeds can be large
        cJSON_ArrayForEach(em, cJSON_GetObjectItem(d, "embeds"))
        {
            cJSON *t = cJSON_CreateObject();
            copy(t, em, "title");
            cJSON_AddItemToArray(embeds, t);
        }
        cJSON *a = cJSON_GetObjectItem(d, "author");
        if (a)
            cJSON_AddItemToObject(e, "author", trim_user(a));
    }
    store_event(e);
    cJSON_Delete(e);
}

// CALL_CREATE / CALL_UPDATE / CALL_DELETE. The calls entry holds who is ringing; the
// event tells the app to look (the app's ring screen does not wait for a tree refresh).
static void ring_alert(cJSON *d);

static int has_ringing(cJSON *call, const char *me)
{
    cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItem(call, "ringing"))
        if (me && cJSON_IsString(r) && strcmp(r->valuestring, me) == 0)
            return 1;
    return 0;
}

static void call_changed(cJSON *d, int gone)
{
    const char *cid = str(d, "channel_id");
    if (!cid)
        return;
    cJSON *calls = cJSON_GetObjectItem(tree, "calls");
    // The caller is kept while the ring message is the same one (CALL_UPDATE re-sends the call).
    char caller[24] = "";
    cJSON *old = cJSON_GetObjectItem(calls, cid);
    if (old && !gone && strcmp(str(old, "message_id") ? str(old, "message_id") : "", str(d, "message_id") ? str(d, "message_id") : "") == 0)
        snprintf(caller, sizeof caller, "%s", str(old, "caller") ? str(old, "caller") : "");
    const char *me = str(cJSON_GetObjectItem(tree, "me"), "id");
    int was_ringing = old && has_ringing(old, me);
    if (!gone && !was_ringing && has_ringing(d, me))
        ring_alert(d);
    cJSON_DeleteItemFromObject(calls, cid);
    if (!gone) {
        cJSON *c = cJSON_AddObjectToObject(calls, cid), *r, *vs;
        cJSON *ringing = cJSON_AddArrayToObject(c, "ringing");
        cJSON_ArrayForEach(r, cJSON_GetObjectItem(d, "ringing"))
            if (cJSON_IsString(r))
                cJSON_AddItemToArray(ringing, cJSON_CreateString(r->valuestring));
        set_string(c, "message_id", str(d, "message_id"));
        set_string(c, "caller", caller);
        cJSON_ArrayForEach(vs, cJSON_GetObjectItem(d, "voice_states"))
            set_voice(vs);
    }
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "t", "CALL");       // docs/voice-api.md
    cJSON_AddStringToObject(e, "channel", cid);
    store_event(e);
    cJSON_Delete(e);
}

// ---- notifications ----------------------------------------------------------------
// Discord's own settings decide first (mutes, each server's "All messages / Only
// mentions / Nothing", @everyone and role mentions), then the app's (prefs.h).

static void (*alert_sink)(const char *context, const char *text, const char *icon, int urgent);
// Composed under the lock, sent after it (gateway thread only).
static char alert_title[160], alert_body[200], alert_icon[160];
static int alert_urgent;

void chat_set_alert(void (*sink)(const char *context, const char *text, const char *icon, int urgent))
{
    alert_sink = sink;
}

// One user_guild_settings entry, by guild id ("@me" for DMs). Lock held.
static void put_notify(cJSON *e)
{
    cJSON *all = cJSON_GetObjectItem(tree, "notify");
    const char *key = str(e, "guild_id") ? str(e, "guild_id") : "@me";
    cJSON *o = cJSON_CreateObject();
    copy(o, e, "muted");
    copy(o, e, "mute_config");
    copy(o, e, "message_notifications");
    copy(o, e, "suppress_everyone");
    copy(o, e, "suppress_roles");
    copy(o, e, "channel_overrides");
    cJSON_DeleteItemFromObject(all, key);
    cJSON_AddItemToObject(all, key, o);
}

// Muted, and the mute has not run out ("until 8 AM" ends; "until I turn it back on" does not).
static int muted_now(cJSON *o)
{
    if (!cJSON_IsTrue(cJSON_GetObjectItem(o, "muted")))
        return 0;
    const char *end = str(cJSON_GetObjectItem(o, "mute_config"), "end_time");
    struct tm tm = {0};
    if (!end || sscanf(end, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                       &tm.tm_sec) != 6)
        return 1;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return timegm(&tm) > time(NULL);
}

static cJSON *override_of(cJSON *entry, const char *channel)
{
    cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItem(entry, "channel_overrides"))
        if (channel && str(o, "channel_id") && strcmp(str(o, "channel_id"), channel) == 0)
            return o;
    return NULL;
}

static int has_id(cJSON *list, const char *id)
{
    cJSON *x;
    cJSON_ArrayForEach(x, list)
        if (id && cJSON_GetStringValue(x) && strcmp(cJSON_GetStringValue(x), id) == 0)
            return 1;
    return 0;
}

// The message mentions us as Discord counts it: by name, @everyone/@here (unless the server
// suppresses it for us), or one of our roles (unless suppressed).
static int mentions_me(cJSON *d, const char *me)
{
    cJSON *m, *e = cJSON_GetObjectItem(cJSON_GetObjectItem(tree, "notify"), str(d, "guild_id") ? str(d, "guild_id") : "");
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(d, "mentions"))
        if (me && str(m, "id") && strcmp(str(m, "id"), me) == 0)
            return 1;
    if (cJSON_IsTrue(cJSON_GetObjectItem(d, "mention_everyone")) && !cJSON_IsTrue(cJSON_GetObjectItem(e, "suppress_everyone")))
        return 1;
    if (!cJSON_IsTrue(cJSON_GetObjectItem(e, "suppress_roles")))
        cJSON_ArrayForEach(m, cJSON_GetObjectItem(d, "mention_roles"))
            if (has_id(cJSON_GetObjectItem(guild_of(str(d, "guild_id")), "_my_roles"), cJSON_GetStringValue(m)))
                return 1;
    return 0;
}

// What a new message is worth: 0 nothing, 1 a server message, 2 a mention, 3 a DM.
// ponytail: a thread's parent channel is not looked up, so a muted channel's threads still alert.
static int notify_kind(cJSON *d, const char *me)
{
    int type = (int)json_int(d, "type", 0);
    if (type != 0 && type != 19) // plain messages and replies; not joins, pins, calls
        return 0;
    const char *channel = str(d, "channel_id"), *guild = str(d, "guild_id");
    cJSON *notify = cJSON_GetObjectItem(tree, "notify");
    if (!guild)
        return muted_now(override_of(cJSON_GetObjectItem(notify, "@me"), channel)) || !prefs_get("notify_dm") ? 0 : 3;
    cJSON *e = cJSON_GetObjectItem(notify, guild), *g = guild_of(guild), *c = find_channel(channel);
    cJSON *chain[3] = {override_of(e, channel), override_of(e, c ? str(c, "parent_id") : NULL), e};
    int level = 3; // 3: as the next one up says
    for (int i = 0; i < 3; i++) {
        if (muted_now(chain[i]))
            return 0;
        cJSON *n = cJSON_GetObjectItem(chain[i], "message_notifications");
        if (level == 3 && cJSON_IsNumber(n))
            level = n->valueint;
    }
    if (level == 3) {
        cJSON *n = cJSON_GetObjectItem(g, "default_message_notifications");
        level = cJSON_IsNumber(n) ? n->valueint : 1;
    }
    if (mentions_me(d, me))
        return level != 2 && prefs_get("notify_dm") ? 2 : 0;
    int server = prefs_get("notify_server"); // 0 off, 1 as set in Discord, 2 every message
    return server == 2 || (server == 1 && level == 0) ? 1 : 0;
}

// Discord's markup as text: <@id> as @name, <@&id> as @role, <#id> as #channel, <:name:id> as :name:.
static void plain_text(const char *in, cJSON *guild, char *out, size_t cap)
{
    size_t n = 0;
    while (in && *in && n + 1 < cap) {
        const char *end = *in == '<' ? strchr(in, '>') : NULL;
        char tok[96], name[128] = "";
        if (end && end - in - 1 < (long)sizeof tok) {
            memcpy(tok, in + 1, (size_t)(end - in - 1));
            tok[end - in - 1] = 0;
            const char *u = NULL;
            if (tok[0] == '@' && tok[1] == '&')
                u = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(guild, "roles"), tok + 2));
            else if (tok[0] == '@') {
                cJSON *who = cJSON_GetObjectItem(cJSON_GetObjectItem(tree, "users"), tok + (tok[1] == '!' ? 2 : 1));
                u = str(who, "global_name") ? str(who, "global_name") : str(who, "username");
            } else if (tok[0] == '#')
                u = str(find_channel(tok + 1), "name");
            if (u)
                snprintf(name, sizeof name, "%c%s", tok[0], u);
            else if (tok[0] == ':' || (tok[0] == 'a' && tok[1] == ':')) {
                const char *s = strchr(tok, ':') + 1, *t = strchr(s, ':');
                if (t)
                    snprintf(name, sizeof name, ":%.*s:", (int)(t - s), s);
            }
        }
        if (name[0]) {
            int w = snprintf(out + n, cap - n, "%s", name);
            n = w < 0 ? n : n + (size_t)w < cap ? n + (size_t)w : cap - 1;
            in = end + 1;
        } else
            out[n++] = *in++;
    }
    out[n] = 0;
}

// The PS5 alert: who and where, what, and the sender's avatar.
static void alert_text(cJSON *d)
{
    cJSON *author = cJSON_GetObjectItem(d, "author"), *g = guild_of(str(d, "guild_id"));
    const char *who = str(cJSON_GetObjectItem(d, "member"), "nick");
    if (!who)
        who = str(author, "global_name") ? str(author, "global_name") : str(author, "username");
    plain_text(str(d, "content"), g, alert_body, sizeof alert_body);
    if (!alert_body[0])
        snprintf(alert_body, sizeof alert_body, "%s",
                 cJSON_GetArraySize(cJSON_GetObjectItem(d, "sticker_items")) > 0 ? "sent a sticker"
                                                                                : "sent an attachment");
    cJSON *c = find_channel(str(d, "channel_id"));
    if (g)
        snprintf(alert_title, sizeof alert_title, "%s · %s #%s", who ? who : "Someone",
                 str(g, "name") ? str(g, "name") : "a server", str(c, "name") ? str(c, "name") : "");
    else
        snprintf(alert_title, sizeof alert_title, "%s", who ? who : "Someone");
    if (str(author, "id") && str(author, "avatar"))
        snprintf(alert_icon, sizeof alert_icon, "https://cdn.discordapp.com/avatars/%s/%s.png?size=128",
                 str(author, "id"), str(author, "avatar"));
    else
        snprintf(alert_icon, sizeof alert_icon, "https://cdn.discordapp.com/embed/avatars/0.png");
}

// A DM call starts ringing us: "Incoming call", who, their avatar; at once, quiet hours or not.
static void ring_alert(cJSON *d)
{
    if (!alert_sink)
        return;
    cJSON *dm = find_channel(str(d, "channel_id"));
    cJSON *ids = cJSON_GetObjectItem(dm, "recipient_ids");
    cJSON *who = cJSON_GetObjectItem(cJSON_GetObjectItem(tree, "users"), cJSON_GetStringValue(cJSON_GetArrayItem(ids, 0)));
    const char *name = str(dm, "name");
    if (!name || !*name)
        name = str(who, "global_name") ? str(who, "global_name") : str(who, "username");
    snprintf(alert_title, sizeof alert_title, "Incoming call");
    snprintf(alert_body, sizeof alert_body, "%s is calling", name ? name : "Someone");
    if (cJSON_GetArraySize(ids) == 1 && str(who, "id") && str(who, "avatar"))
        snprintf(alert_icon, sizeof alert_icon, "https://cdn.discordapp.com/avatars/%s/%s.png?size=128", str(who, "id"),
                 str(who, "avatar"));
    else
        snprintf(alert_icon, sizeof alert_icon, "https://cdn.discordapp.com/embed/avatars/0.png");
    alert_urgent = 1;
}

static void message_created(cJSON *d)
{
    const char *channel_id = str(d, "channel_id");
    const char *id = str(d, "id");
    if (!channel_id || !id)
        return;
    // A call's ring message (type 3) names who started the call.
    cJSON *call = cJSON_GetObjectItem(cJSON_GetObjectItem(tree, "calls"), channel_id);
    if (call && (int)json_int(d, "type", 0) == 3 &&
        strcmp(str(call, "message_id") ? str(call, "message_id") : "", id) == 0) {
        const char *who = str(cJSON_GetObjectItem(d, "author"), "id");
        set_string(call, "caller", who ? who : "");
    }
    cJSON *c = find_channel(channel_id);
    if (c)
        set_string(c, "last_message_id", id);
    const char *me = str(cJSON_GetObjectItem(tree, "me"), "id");
    const char *author = str(cJSON_GetObjectItem(d, "author"), "id");
    cJSON *r = read_state(channel_id);
    if (me && author && strcmp(me, author) == 0)
    {
        set_string(r, "last_read", id); // your own message reads the channel
        return;
    }
    if (!str(d, "guild_id") || mentions_me(d, me)) // every DM message counts, as in Discord
    {
        const double n = (double)json_int(r, "mentions", 0);
        cJSON_DeleteItemFromObject(r, "mentions");
        cJSON_AddNumberToObject(r, "mentions", n + 1);
    }
    // For the app's banner (notify, quiet) and, sent after the lock, the PS5 alert.
    const int kind = notify_kind(d, me), quiet = kind && notify_quiet_now();
    cJSON_AddNumberToObject(d, "notify", kind);
    cJSON_AddBoolToObject(d, "quiet", quiet);
    if (kind && !quiet && alert_sink)
        alert_text(d);
}

static int dm_listed(const char *id)
{
    cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(tree, "dms"))
        if (id && str(c, "id") && strcmp(str(c, "id"), id) == 0)
            return 1;
    return 0;
}

void chat_dispatch(const char *type, cJSON *d)
{
    pthread_mutex_lock(&lock);
    if (strcmp(type, "READY") == 0)
    {
        cJSON_Delete(tree);
        tree = NULL;
        load_ready(d);
        tree_version = (long)time(NULL) << 16;
    }
    else if (!tree)
    {
        // Nothing to update before READY.
    }
    else if (strcmp(type, "READY_SUPPLEMENTAL") == 0)
        load_supplemental(d);
    else if (strcmp(type, "PRESENCE_UPDATE") == 0 && !cJSON_GetObjectItem(d, "guild_id"))
        set_presence(str(cJSON_GetObjectItem(d, "user"), "id"), str(d, "status"), cJSON_GetObjectItem(d, "activities"));
    else if (strcmp(type, "VOICE_STATE_UPDATE") == 0)
        set_voice(d);
    else if (strcmp(type, "CALL_CREATE") == 0 || strcmp(type, "CALL_UPDATE") == 0)
        call_changed(d, 0);
    else if (strcmp(type, "CALL_DELETE") == 0)
        call_changed(d, 1);
    else if (strcmp(type, "GUILD_CREATE") == 0 || strcmp(type, "GUILD_DELETE") == 0)
        guild_changed(d, type[6] == 'C');
    else if ((strcmp(type, "CHANNEL_CREATE") == 0 || strcmp(type, "CHANNEL_UPDATE") == 0) && guild_of(str(d, "guild_id")))
        put_channel(guild_of(str(d, "guild_id")), d);
    else if (strcmp(type, "CHANNEL_DELETE") == 0 && guild_of(str(d, "guild_id")))
    {
        cJSON *gone = cJSON_Duplicate(d, 1); // same id, nothing visible: put_channel only removes
        cJSON_DeleteItemFromObject(gone, "permission_overwrites"); // replacing a missing key leaks
        cJSON_AddArrayToObject(gone, "permission_overwrites");
        cJSON_DeleteItemFromObject(gone, "type");
        cJSON_AddNumberToObject(gone, "type", 99);
        put_channel(guild_of(str(d, "guild_id")), gone);
        cJSON_Delete(gone);
    }
    // ponytail: role and member updates (permission changes) show after a reconnect.
    else if (strcmp(type, "MESSAGE_ACK") == 0 && str(d, "channel_id"))
    {
        cJSON *r = read_state(str(d, "channel_id"));
        set_string(r, "last_read", str(d, "message_id"));
        cJSON_DeleteItemFromObject(r, "mentions");
        cJSON_AddNumberToObject(r, "mentions", 0);
    }
    else if (strcmp(type, "CHANNEL_CREATE") == 0 && (json_int(d, "type", 0) == 1 || json_int(d, "type", 0) == 3) &&
             !dm_listed(str(d, "id"))) // a DM (1) or group DM (3), once: Discord resends a known one
    {
        cJSON *oc = cJSON_CreateObject(), *r;
        copy(oc, d, "id");
        copy(oc, d, "type");
        copy(oc, d, "last_message_id");
        copy(oc, d, "name"); // group DMs only
        cJSON *ids = cJSON_AddArrayToObject(oc, "recipient_ids");
        cJSON_ArrayForEach(r, cJSON_GetObjectItem(d, "recipients"))
        {
            remember_user(r);
            cJSON_AddItemToArray(ids, cJSON_CreateString(str(r, "id") ? str(r, "id") : ""));
        }
        cJSON_AddItemToArray(cJSON_GetObjectItem(tree, "dms"), oc);
    }
    else if (strncmp(type, "MESSAGE_", 8) == 0 &&
             (strcmp(type + 8, "CREATE") == 0 || strcmp(type + 8, "UPDATE") == 0 || strcmp(type + 8, "DELETE") == 0 ||
              strcmp(type + 8, "REACTION_ADD") == 0 || strcmp(type + 8, "REACTION_REMOVE") == 0))
    {
        if (strcmp(type, "MESSAGE_CREATE") == 0)
            message_created(d);
        push_event(type, d);
    }
    else if (strcmp(type, "USER_GUILD_SETTINGS_UPDATE") == 0)
        put_notify(d);
    else if ((strcmp(type, "GUILD_EMOJIS_UPDATE") == 0 || strcmp(type, "GUILD_STICKERS_UPDATE") == 0) &&
             guild_of(str(d, "guild_id")))
        put_emojis(guild_of(str(d, "guild_id")), d, type[6] == 'E' ? "emojis" : "stickers");
    if (tree) // ponytail: any dispatch counts as a change; a needless one costs one full reply
        tree_version++;
    pthread_mutex_unlock(&lock);
    if (alert_title[0]) {
        alert_sink(alert_title, alert_body, alert_icon, alert_urgent);
        alert_title[0] = 0;
        alert_urgent = 0;
    }
}

int chat_me_id(char *out, size_t cap)
{
    int ok = 0;
    pthread_mutex_lock(&lock);
    const char *id = tree ? str(cJSON_GetObjectItem(tree, "me"), "id") : NULL;
    if (id) {
        snprintf(out, cap, "%s", id);
        ok = 1;
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

int chat_call_active(const char *channel_id)
{
    pthread_mutex_lock(&lock);
    int on = tree && cJSON_GetObjectItem(cJSON_GetObjectItem(tree, "calls"), channel_id) != NULL;
    pthread_mutex_unlock(&lock);
    return on;
}

static int listed(cJSON *array, const char *id)
{
    cJSON *v;
    cJSON_ArrayForEach(v, array)
        if (cJSON_IsString(v) && strcmp(v->valuestring, id) == 0)
            return 1;
    return 0;
}

// Lock held. users gains everyone seen (authors, voice, presence) and would only grow: keep
// me, friends, DM recipients, people in voice and ringing calls. Messages carry their
// author themselves, so the app needs no others.
static void prune_users(void)
{
    cJSON *users = cJSON_GetObjectItem(tree, "users"), *dms = cJSON_GetObjectItem(tree, "dms");
    cJSON *voice = cJSON_GetObjectItem(tree, "voice"), *calls = cJSON_GetObjectItem(tree, "calls");
    const char *me = str(cJSON_GetObjectItem(tree, "me"), "id");
    cJSON *u = users ? users->child : NULL;
    while (u) {
        cJSON *next = u->next, *x, *y;
        const char *id = u->string;
        int keep = !id || (me && strcmp(id, me) == 0) || listed(cJSON_GetObjectItem(tree, "friends"), id);
        cJSON_ArrayForEach(x, dms)
            keep = keep || listed(cJSON_GetObjectItem(x, "recipient_ids"), id);
        cJSON_ArrayForEach(x, voice)
            cJSON_ArrayForEach(y, x)
                keep = keep || (str(y, "user_id") && strcmp(str(y, "user_id"), id) == 0);
        cJSON_ArrayForEach(x, calls)
            keep = keep || listed(cJSON_GetObjectItem(x, "ringing"), id);
        if (!keep)
            cJSON_Delete(cJSON_DetachItemViaPointer(users, u));
        u = next;
    }
}

long chat_tree_version(void)
{
    pthread_mutex_lock(&lock);
    long v = tree ? tree_version : -1;
    pthread_mutex_unlock(&lock);
    return v;
}

char *chat_tree_json(void)
{
    pthread_mutex_lock(&lock);
    char *s = NULL;
    if (tree) {
        if (time(NULL) - pruned >= 60) { // ponytail: a full scan, once a minute at most
            prune_users();
            pruned = time(NULL);
        }
        cJSON_DeleteItemFromObject(tree, "version");
        cJSON_AddNumberToObject(tree, "version", (double)tree_version);
        s = cJSON_PrintUnformatted(tree);
    }
    pthread_mutex_unlock(&lock);
    return s;
}

char *chat_events_json(long since, int wait_ms)
{
    pthread_mutex_lock(&lock);
    // Long poll: with nothing new, wait for the next event (or the time limit).
    // A cursor past the end (the core restarted) answers at once so the app resyncs.
    if (wait_ms > 0 && since == next_event) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += wait_ms / 1000;
        until.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        while (since == next_event)
            if (pthread_cond_timedwait(&event_ready, &lock, &until) != 0)
                break;
    }
    long first = next_event > EVENT_RING ? next_event - EVENT_RING : 0;
    int gap = since < first; // the app missed events (suspended in a game): it reloads
    if (gap)
        since = first;
    size_t cap = 64;
    for (long i = since; i < next_event; i++)
        cap += strlen(events[i % EVENT_RING]) + 1;
    char *out = malloc(cap);
    if (out)
    {
        size_t n = (size_t)snprintf(out, cap, "{\"next\":%ld,%s\"events\":[", next_event, gap ? "\"gap\":true," : "");
        for (long i = since; i < next_event; i++)
            n += (size_t)snprintf(out + n, cap - n, "%s%s", i > since ? "," : "", events[i % EVENT_RING]);
        snprintf(out + n, cap - n, "]}");
    }
    pthread_mutex_unlock(&lock);
    return out;
}
