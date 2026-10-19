#include "chat.h"

#include <pthread.h>
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
//   voice{channel_id: [states]}, reads{channel_id: {last_read, mentions}}
static cJSON *tree;
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
        if ((int)cJSON_GetNumberValue(cJSON_GetObjectItem(a, "type")) == 0 && str(a, "name"))
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
        int type = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "type"));
        if (id && type == 0 && strcmp(id, gid ? gid : "") != 0 && has_role(my_roles, id)) {
            allow |= perm_bits(o, "allow");
            deny |= perm_bits(o, "deny");
        }
    }
    base = (base & ~deny) | allow;
    cJSON_ArrayForEach(o, cJSON_GetObjectItem(c, "permission_overwrites"))
    {
        const char *id = str(o, "id");
        int type = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "type"));
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
    int type = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(c, "type"));
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

static void load_ready(cJSON *d)
{
    cJSON *t = cJSON_CreateObject(), *g, *c;
    tree = t; // remember_user and friends write here; the lock is held by the caller
    cJSON_AddItemToObject(t, "me", trim_user(cJSON_GetObjectItem(d, "user")));
    cJSON_AddObjectToObject(t, "users");
    cJSON_AddObjectToObject(t, "presence");
    cJSON_AddObjectToObject(t, "voice");
    cJSON_AddObjectToObject(t, "reads");
    cJSON_AddNullToObject(t, "my_voice");
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(d, "users"))
        remember_user(c);
    remember_user(cJSON_GetObjectItem(d, "user"));

    cJSON *guilds = cJSON_AddArrayToObject(t, "guilds");
    const char *me = str(cJSON_GetObjectItem(d, "user"), "id");
    int gi = -1;
    cJSON_ArrayForEach(g, cJSON_GetObjectItem(d, "guilds"))
    {
        ++gi;
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
        if ((int)cJSON_GetNumberValue(cJSON_GetObjectItem(c, "type")) != 1) // 1 = friend
            continue;
        const char *id = str(c, "id");
        if (!id)
            id = str(cJSON_GetObjectItem(c, "user"), "id");
        if (cJSON_GetObjectItem(c, "user"))
            remember_user(cJSON_GetObjectItem(c, "user"));
        if (id)
            cJSON_AddItemToArray(friends, cJSON_CreateString(id));
    }

    cJSON_ArrayForEach(c, cJSON_GetObjectItem(cJSON_GetObjectItem(d, "read_state"), "entries"))
    {
        const char *id = str(c, "id");
        if (!id)
            continue;
        cJSON *r = read_state(id);
        set_string(r, "last_read", str(c, "last_message_id"));
        cJSON_DeleteItemFromObject(r, "mentions");
        cJSON_AddNumberToObject(r, "mentions", cJSON_GetNumberValue(cJSON_GetObjectItem(c, "mention_count")));
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
    cJSON_AddNumberToObject(e, "seq", (double)next_event);
    char *s = cJSON_PrintUnformatted(e);
    free(events[next_event % EVENT_RING]);
    events[next_event % EVENT_RING] = s;
    next_event++;
    pthread_cond_broadcast(&event_ready);
    cJSON_Delete(e);
}

static void message_created(cJSON *d)
{
    const char *channel_id = str(d, "channel_id");
    const char *id = str(d, "id");
    if (!channel_id || !id)
        return;
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
    cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(d, "mentions"))
    {
        if (me && str(m, "id") && strcmp(str(m, "id"), me) == 0)
        {
            const double n = cJSON_GetNumberValue(cJSON_GetObjectItem(r, "mentions"));
            cJSON_DeleteItemFromObject(r, "mentions");
            cJSON_AddNumberToObject(r, "mentions", n + 1);
            break;
        }
    }
}

void chat_dispatch(const char *type, cJSON *d)
{
    pthread_mutex_lock(&lock);
    if (strcmp(type, "READY") == 0)
    {
        cJSON_Delete(tree);
        tree = NULL;
        load_ready(d);
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
    else if (strcmp(type, "GUILD_CREATE") == 0 || strcmp(type, "GUILD_DELETE") == 0)
        guild_changed(d, type[6] == 'C');
    else if ((strcmp(type, "CHANNEL_CREATE") == 0 || strcmp(type, "CHANNEL_UPDATE") == 0) && guild_of(str(d, "guild_id")))
        put_channel(guild_of(str(d, "guild_id")), d);
    else if (strcmp(type, "CHANNEL_DELETE") == 0 && guild_of(str(d, "guild_id")))
    {
        cJSON *gone = cJSON_Duplicate(d, 1); // same id, nothing visible: put_channel only removes
        cJSON_ReplaceItemInObject(gone, "permission_overwrites", cJSON_Parse("[]"));
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
    else if (strcmp(type, "CHANNEL_CREATE") == 0 && (int)cJSON_GetNumberValue(cJSON_GetObjectItem(d, "type")) == 1)
    {
        cJSON *oc = cJSON_CreateObject(), *r;
        copy(oc, d, "id");
        copy(oc, d, "type");
        copy(oc, d, "last_message_id");
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
    pthread_mutex_unlock(&lock);
}

char *chat_tree_json(void)
{
    pthread_mutex_lock(&lock);
    char *s = tree ? cJSON_PrintUnformatted(tree) : NULL;
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
    if (since < first)
        since = first;
    size_t cap = 64;
    for (long i = since; i < next_event; i++)
        cap += strlen(events[i % EVENT_RING]) + 1;
    char *out = malloc(cap);
    if (out)
    {
        size_t n = (size_t)snprintf(out, cap, "{\"next\":%ld,\"events\":[", next_event);
        for (long i = since; i < next_event; i++)
            n += (size_t)snprintf(out + n, cap - n, "%s%s", i > since ? "," : "", events[i % EVENT_RING]);
        snprintf(out + n, cap - n, "]}");
    }
    pthread_mutex_unlock(&lock);
    return out;
}
