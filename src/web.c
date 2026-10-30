#include "web.h"
#include "json_num.h"

#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>

#include <cJSON.h>

#include "chat.h"
#include "core_key.h"
#include "gateway.h"
#include "http.h"
#include "log.h"
#include "ratelimit.h"
#include "status.h"
#include "voice.h"
#include "voice_audio.h"
#include "controller_voice.h"
#include "broadcast.h"
#include "prefs.h"
#include "rp_pair.h"
#include "stream.h"
#include "browser_auth.h"

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#endif


static void reply(int c, const char *status, const char *type, const void *body, size_t len)
{
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     status, type, len);
    send(c, head, (size_t)n, 0);
    for (const char *p = body; len > 0;) {
        ssize_t w = send(c, p, len, 0);
        if (w <= 0)
            break;
        p += w;
        len -= (size_t)w;
    }
}

// Snowflake check: IDs go into Discord API paths, so digits only.
static int is_id(const char *s)
{
    size_t n = strspn(s, "0123456789");
    return n > 0 && n < 24 && s[n] == 0;
}

// Value of key in a query string ("a=1&b=2") into out; 0 when found.
static int query_get(const char *q, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    for (const char *p = q; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t n = strcspn(p + klen + 1, "&");
            if (n >= cap)
                return -1;
            memcpy(out, p + klen + 1, n);
            out[n] = 0;
            return 0;
        }
    }
    return -1;
}

static void reply_json(int c, char *j)
{
    reply(c, j ? "200 OK" : "503 Service Unavailable", "application/json", j ? j : "null", j ? strlen(j) : 4);
    free(j);
}

// Proxy a Discord REST call; the UI gets {"status":N,"body":<Discord JSON>}.
// One REST call; a 429 is retried once after the wait Discord names, if short.
static http_response rest(const char *method, const char *path, const char *json)
{
    char token[256];
    chat_token(token, sizeof token);
    http_response res = http_request(method, path, json, token);
    const double wait = ratelimit_retry_after(&res, 5.0);
    if (wait > 0) {
        LOG("rate limited on %s %s, retrying in %.2f s", method, path, wait);
        free(res.body);
        usleep((useconds_t)(wait * 1e6) + 50000);
        res = http_request(method, path, json, token);
    }
    return res;
}

static void reply_rest(int c, http_response res)
{
    // Discord's reply goes in as JSON only when it is JSON: a proxy's HTML error page would
    // make the whole reply unreadable, and the app would lose the status with it.
    cJSON *parsed = res.body ? cJSON_Parse(res.body) : NULL;
    const char *body = parsed ? res.body : "null";
    cJSON_Delete(parsed);
    size_t cap = strlen(body) + 48;
    char *out = malloc(cap);
    if (out)
        snprintf(out, cap, "{\"status\":%d,\"body\":%s}", res.status, body);
    free(res.body);
    reply_json(c, out);
}

static void api_messages(int c, const char *query)
{
    char channel[32], guild[32], before[32], path[128];
    if (query_get(query, "channel", channel, sizeof channel) != 0 || !is_id(channel)) {
        reply(c, "400 Bad Request", "text/plain", "channel", 7);
        return;
    }
    if (query_get(query, "guild", guild, sizeof guild) == 0 && is_id(guild))
        gateway_subscribe(guild);
    if (query_get(query, "before", before, sizeof before) == 0 && is_id(before))
        snprintf(path, sizeof path, "/channels/%s/messages?limit=50&before=%s", channel, before);
    else
        snprintf(path, sizeof path, "/channels/%s/messages?limit=50", channel);
    reply_rest(c, rest("GET", path, NULL));
}

// %XX and + decoding for query values.
static void url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0};
            *out++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *out++ = *s == '+' ? ' ' : *s;
        }
    }
    *out = 0;
}

// Recent mentions of the account (Discord's inbox endpoint).
static void api_mentions(int c)
{
    reply_rest(c, rest("GET", "/users/@me/mentions?limit=25&roles=true&everyone=true", NULL));
}

// Avatars, icons and attachments for the app, which speaks no TLS. Only
// Discord's CDN hosts: the console must not become a general web proxy. The path to fetch
// from host (written there), or NULL when the URL is not one of them.
static const char *image_target(const char *url, char host[128])
{
    // Decoded CR, LF or space would end the CDN request line early and add headers.
    const char *rest = strncmp(url, "https://", 8) == 0 && !strpbrk(url, "\r\n ") ? url + 8 : NULL;
    size_t hlen = rest ? strcspn(rest, "/") : 0;
    if (!rest || hlen == 0 || hlen >= 128)
        return NULL;
    memcpy(host, rest, hlen);
    host[hlen] = 0;
    int allowed = strcmp(host, "cdn.discordapp.com") == 0 || strcmp(host, "media.discordapp.net") == 0 ||
                  (strncmp(host, "images-ext-", 11) == 0 && hlen > 26 &&
                   strcmp(host + hlen - 15, ".discordapp.net") == 0) ||
                  // Colour emoji pictures (Twemoji, as Discord draws them), that project only.
                  (strcmp(host, "cdn.jsdelivr.net") == 0 &&
                   strncmp(rest + hlen, "/gh/jdecked/twemoji@", 20) == 0 && !strstr(rest + hlen, ".."));
    return allowed ? rest + hlen : NULL;
}

static void api_image(int c, const char *query)
{
    char url[1024];
    if (query_get(query, "url", url, sizeof url) != 0) {
        reply(c, "400 Bad Request", "text/plain", "url", 3);
        return;
    }
    url_decode(url);
    char host[128];
    const char *path = image_target(url, host);
    if (!path) {
        reply(c, "403 Forbidden", "text/plain", "host", 4);
        return;
    }
    http_response res = http_get(host, path);
    if (res.status != 200 || !res.body)
        reply(c, "502 Bad Gateway", "text/plain", "fetch", 5);
    else
        reply(c, "200 OK", "application/octet-stream", res.body, res.body_len);
    free(res.body);
}

// Marks a channel read up to a message.
static void api_ack(int c, const char *body)
{
    cJSON *in = cJSON_Parse(body ? body : "");
    const char *channel = cJSON_GetStringValue(cJSON_GetObjectItem(in, "channel"));
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItem(in, "message"));
    if (!channel || !is_id(channel) || !message || !is_id(message)) {
        cJSON_Delete(in);
        reply(c, "400 Bad Request", "text/plain", "ack", 3);
        return;
    }
    char path[96];
    snprintf(path, sizeof path, "/channels/%s/messages/%s/ack", channel, message);
    cJSON_Delete(in);
    reply_rest(c, rest("POST", path, "{\"token\":null}"));
}

static void api_send(int c, const char *body)
{
    cJSON *in = cJSON_Parse(body ? body : "");
    const char *channel = cJSON_GetStringValue(cJSON_GetObjectItem(in, "channel"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(in, "content"));
    const char *nonce = cJSON_GetStringValue(cJSON_GetObjectItem(in, "nonce"));
    const char *sticker = cJSON_GetStringValue(cJSON_GetObjectItem(in, "sticker")); // sent on its own
    if (!content)
        content = "";
    // Byte cap only; Discord enforces its 2000 character limit itself.
    if (!channel || !is_id(channel) || (!*content && !(sticker && is_id(sticker))) || strlen(content) > 8000) {
        cJSON_Delete(in);
        reply(c, "400 Bad Request", "text/plain", "message", 7);
        return;
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "mobile_network_type", "unknown");
    cJSON_AddStringToObject(out, "content", content);
    if (nonce && is_id(nonce))
        cJSON_AddStringToObject(out, "nonce", nonce);
    cJSON_AddFalseToObject(out, "tts");
    cJSON_AddNumberToObject(out, "flags", 0);
    if (sticker && is_id(sticker))
        cJSON_AddItemToArray(cJSON_AddArrayToObject(out, "sticker_ids"), cJSON_CreateString(sticker));
    const char *reply_to = cJSON_GetStringValue(cJSON_GetObjectItem(in, "reply_to"));
    if (reply_to && is_id(reply_to)) {
        cJSON *ref = cJSON_AddObjectToObject(out, "message_reference");
        cJSON_AddStringToObject(ref, "channel_id", channel);
        cJSON_AddStringToObject(ref, "message_id", reply_to);
    }
    char *json = cJSON_PrintUnformatted(out), path[64];
    snprintf(path, sizeof path, "/channels/%s/messages", channel);
    cJSON_Delete(out);
    cJSON_Delete(in);
    reply_rest(c, rest("POST", path, json));
    free(json);
}

// Discord's own sticker packs (Wumpus and friends), trimmed to what the picker shows:
// {"packs": [{"id", "name", "stickers": [{"id", "name", "format_type"}]}]}. Fetched once.
static void api_sticker_packs(int c)
{
    static char *cached;
    static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&once);
    if (!cached) {
        char token[256];
        chat_token(token, sizeof token);
        http_response r = http_request("GET", "/sticker-packs", NULL, token);
        cJSON *in = r.status == 200 ? cJSON_Parse(r.body) : NULL, *p, *s;
        free(r.body);
        cJSON *out = cJSON_CreateObject(), *packs = cJSON_AddArrayToObject(out, "packs");
        cJSON_ArrayForEach(p, cJSON_GetObjectItem(in, "sticker_packs"))
        {
            cJSON *op = cJSON_CreateObject(), *list = NULL;
            cJSON_AddStringToObject(op, "id", cJSON_GetStringValue(cJSON_GetObjectItem(p, "id")) ?: "");
            cJSON_AddStringToObject(op, "name", cJSON_GetStringValue(cJSON_GetObjectItem(p, "name")) ?: "");
            list = cJSON_AddArrayToObject(op, "stickers");
            cJSON_ArrayForEach(s, cJSON_GetObjectItem(p, "stickers"))
            {
                cJSON *os = cJSON_CreateObject();
                cJSON_AddStringToObject(os, "id", cJSON_GetStringValue(cJSON_GetObjectItem(s, "id")) ?: "");
                cJSON_AddStringToObject(os, "name", cJSON_GetStringValue(cJSON_GetObjectItem(s, "name")) ?: "");
                cJSON_AddNumberToObject(os, "format_type", (double)json_int(s, "format_type", 1));
                cJSON_AddItemToArray(list, os);
            }
            cJSON_AddItemToArray(packs, op);
        }
        if (in) // a failed fetch is tried again next time
            cached = cJSON_PrintUnformatted(out);
        cJSON_Delete(in);
        cJSON_Delete(out);
    }
    const char *body = cached ? cached : "{\"packs\":[]}";
    reply(c, "200 OK", "application/json", body, strlen(body));
    pthread_mutex_unlock(&once);
}

// Adds the account's reaction: {channel, message, emoji} (a Unicode emoji).
static void api_react(int c, const char *body)
{
    cJSON *in = cJSON_Parse(body ? body : "");
    const char *channel = cJSON_GetStringValue(cJSON_GetObjectItem(in, "channel"));
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItem(in, "message"));
    const char *emoji = cJSON_GetStringValue(cJSON_GetObjectItem(in, "emoji")); // Unicode, or a server's name:id
    const int remove = cJSON_IsTrue(cJSON_GetObjectItem(in, "remove"));
    if (!channel || !is_id(channel) || !message || !is_id(message) || !emoji || !*emoji || strlen(emoji) > 80) {
        cJSON_Delete(in);
        reply(c, "400 Bad Request", "text/plain", "react", 5);
        return;
    }
    char path[512];
    int n = snprintf(path, sizeof path, "/channels/%s/messages/%s/reactions/", channel, message);
    for (const unsigned char *e = (const unsigned char *)emoji; *e && n < (int)sizeof path - 16; e++)
        n += snprintf(path + n, sizeof path - n, "%%%02X", *e);
    snprintf(path + n, sizeof path - n, "/@me?location=Message");
    cJSON_Delete(in);
    reply_rest(c, rest(remove ? "DELETE" : "PUT", path, NULL));
}

// Edits or deletes one of the account's messages: {channel, message[, content]}.
static void api_change(int c, const char *body, int remove)
{
    cJSON *in = cJSON_Parse(body ? body : "");
    const char *channel = cJSON_GetStringValue(cJSON_GetObjectItem(in, "channel"));
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItem(in, "message"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(in, "content"));
    if (!channel || !is_id(channel) || !message || !is_id(message) ||
        (!remove && (!content || !*content || strlen(content) > 8000))) {
        cJSON_Delete(in);
        reply(c, "400 Bad Request", "text/plain", "message", 7);
        return;
    }
    char path[96];
    snprintf(path, sizeof path, "/channels/%s/messages/%s", channel, message);
    char *json = NULL;
    if (!remove) {
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "content", content);
        json = cJSON_PrintUnformatted(out);
        cJSON_Delete(out);
    }
    cJSON_Delete(in);
    reply_rest(c, rest(remove ? "DELETE" : "PATCH", path, json));
    free(json);
}

// Opens (or finds) the DM with a user: {user}; Discord answers with the channel.
static void api_dm(int c, const char *body)
{
    cJSON *in = cJSON_Parse(body ? body : "");
    const char *user = cJSON_GetStringValue(cJSON_GetObjectItem(in, "user"));
    if (!user || !is_id(user)) {
        cJSON_Delete(in);
        reply(c, "400 Bad Request", "text/plain", "user", 4);
        return;
    }
    char json[96];
    snprintf(json, sizeof json, "{\"recipients\":[\"%s\"]}", user);
    cJSON_Delete(in);
    reply_rest(c, rest("POST", "/users/@me/channels", json));
}

// Voice (docs/voice-api.md). Each refusal is kept for GET /api/voice ("refused") and
// the next accepted command clears it.
static void reply_ok(int c)
{
    voice_refuse("");
    reply(c, "202 Accepted", "application/json", "{\"ok\":true}", 11);
}

// 400/409 with a short reason the app can show.
static void refuse(int c, const char *status, const char *why)
{
    voice_refuse(why);
    reply(c, status, "text/plain", why, strlen(why));
}

// The routes need a signed-in account: 409 until READY.
static int signed_in(int c)
{
    char me[32];
    if (chat_me_id(me, sizeof me))
        return 1;
    refuse(c, "409 Conflict", "Sign in first.");
    return 0;
}

// The body's "channel" (a snowflake), or NULL after a 400.
static const char *body_channel(int c, cJSON *in)
{
    const char *channel = cJSON_GetStringValue(cJSON_GetObjectItem(in, "channel"));
    if (channel && is_id(channel))
        return channel;
    refuse(c, "400 Bad Request", "That channel id is not valid.");
    return NULL;
}

// Join a server voice channel: {channel, guild}. The gateway thread sends op 4.
static void api_voice_join(int c, const char *body)
{
    cJSON *in = cJSON_Parse(body ? body : "");
    const char *guild = cJSON_GetStringValue(cJSON_GetObjectItem(in, "guild"));
    const char *channel = body_channel(c, in);
    if (channel && (!guild || !is_id(guild))) {
        refuse(c, "400 Bad Request", "That server id is not valid.");
        channel = NULL;
    }
    if (channel) {
        voice_join(guild, channel);
        reply_ok(c);
    }
    cJSON_Delete(in);
}

// Join a DM call: {channel}. With no call running, rings the other side first (answer is the
// same: the ring is already on, so no new ring is sent).
// Rings the others once we are in the call (arg: the channel, freed here). Discord rings
// only for a call its caller is in: rung before the join, nobody saw it (the official
// client joins, then rings).
static void *ring_when_joined(void *arg)
{
    char *channel = arg, g[32], ch[32], me[32], sess[64];
    for (int i = 0; i < 150 && voice_wanted(); i++) { // 15 s, while the call is still wanted
        if (voice_current(g, ch, me, sess) && strcmp(ch, channel) == 0) {
            char path[96];
            snprintf(path, sizeof path, "/channels/%s/call/ring", channel);
            http_response res = rest("POST", path, "{\"recipients\":null}");
            LOG("voice: ringing the call in %s: Discord answered %d", channel, res.status);
            free(res.body);
            break;
        }
        usleep(100000);
    }
    free(channel);
    return NULL;
}

static void api_voice_call(int c, const char *body)
{
    cJSON *in = cJSON_Parse(body ? body : "");
    const char *channel = body_channel(c, in);
    if (channel) {
        const int ring = !chat_call_active(channel); // a call already there (they rang us): join it
        voice_join(NULL, channel);
        pthread_t t;
        char *arg = ring ? strdup(channel) : NULL;
        if (arg && pthread_create(&t, NULL, ring_when_joined, arg) == 0)
            pthread_detach(t);
        else
            free(arg);
        reply_ok(c);
    }
    cJSON_Delete(in);
}

// Stop ringing us on a DM call: {channel}. We do not join it.
static void api_voice_decline(int c, const char *body)
{
    char me[32], path[96], json[96];
    if (!chat_me_id(me, sizeof me))
        return;
    cJSON *in = cJSON_Parse(body ? body : "");
    const char *channel = body_channel(c, in);
    cJSON_Delete(in);
    if (!channel)
        return;
    snprintf(path, sizeof path, "/channels/%s/call/stop-ringing", channel);
    snprintf(json, sizeof json, "{\"recipients\":[\"%s\"]}", me);
    http_response res = rest("POST", path, json);
    free(res.body);
    if (res.status >= 200 && res.status <= 299)
        reply_ok(c);
    else
        refuse(c, "409 Conflict", "Discord would not stop the ring.");
}

// Leave whatever we are in: no body.
static void api_voice_leave(int c)
{
    voice_leave();
    reply_ok(c);
}

// Self mute and deafen: {mute, deaf}, each optional. 409 when not in a call.
static void api_voice_set(int c, const char *body)
{
    cJSON *in = cJSON_Parse(body ? body : "");
    cJSON *m = cJSON_GetObjectItem(in, "mute"), *d = cJSON_GetObjectItem(in, "deaf");
    if ((m && !cJSON_IsBool(m)) || (d && !cJSON_IsBool(d))) {
        cJSON_Delete(in);
        refuse(c, "400 Bad Request", "mute and deaf must be true or false.");
        return;
    }
    int r = voice_set_flags(m ? cJSON_IsTrue(m) : -1, d ? cJSON_IsTrue(d) : -1);
    cJSON_Delete(in);
    if (r == 0)
        reply_ok(c);
    else
        refuse(c, "409 Conflict", "Not in a call.");
}

// Where the call's audio is while the app is on screen: {"tv": true} the app (TV and the
// console's own mic), false the USB controller. With the app covered it is always the
// controller, when one is plugged in (controller_voice.h).
static void api_voice_output(int c, const char *body)
{
    cJSON *in = cJSON_Parse(body ? body : "");
    cJSON *tv = cJSON_GetObjectItem(in, "tv");
    int ok = cJSON_IsBool(tv);
    if (ok)
        cv_set_tv(cJSON_IsTrue(tv));
    cJSON_Delete(in);
    if (ok)
        reply_ok(c);
    else
        refuse(c, "400 Bad Request", "tv must be true or false.");
}

// Go Live on or off: {"on": true|false} (broadcast.h). Needs the call and a USB controller.
static void api_voice_broadcast(int c, const char *body)
{
    cJSON *in = cJSON_Parse(body ? body : "");
    cJSON *on = cJSON_GetObjectItem(in, "on");
    int ok = cJSON_IsBool(on), start = cJSON_IsTrue(on);
    cJSON_Delete(in);
    if (!ok)
        refuse(c, "400 Bad Request", "on must be true or false.");
    else if (!start) {
        stream_stop();
        bc_stop();
        reply_ok(c);
    } else if (bc_start() != 0)
        refuse(c, "409 Conflict", bc_error());
    else if (stream_start() != 0) {
        bc_stop();
        refuse(c, "409 Conflict", stream_error());
    } else
        reply_ok(c);
}

// Remote Play pairing for Go Live, per console user (rp_pair.h). Users are hex ids.
//   pair   {"user": "15c5b66e"?, "pin": "12345678"?}  no pin: the core gets one itself
//   unpair {"user": ...}    use {"user": ...} (the user Go Live runs as)
static int32_t body_user(cJSON *in)
{
    const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(in, "user"));
    return u && *u ? (int32_t)strtoul(u, NULL, 16) : -1;
}

static void api_remoteplay(int c, const char *path, const char *body)
{
    cJSON *in = cJSON_Parse(body ? body : "");
    int32_t user = body_user(in);
    int r;
    if (strcmp(path, "/api/remoteplay/pair") == 0)
        r = rp_pair(user, cJSON_GetStringValue(cJSON_GetObjectItem(in, "pin")));
    else if (user < 0)
        r = -1;
    else if (strcmp(path, "/api/remoteplay/unpair") == 0)
        r = rp_unpair(user);
    else
        r = rp_use(user);
    cJSON_Delete(in);
    if (r == 0)
        reply_ok(c);
    else
        refuse(c, "400 Bad Request", strcmp(path, "/api/remoteplay/pair") == 0 ? rp_pair_error() : "No such pairing.");
}

// The console-side settings (prefs.h): {"golive_quality": 2, ...}, any of them.
static void api_prefs(int c, const char *body)
{
    cJSON *in = cJSON_Parse(body ? body : "");
    int ok = cJSON_IsObject(in) && in->child;
    for (cJSON *k = in ? in->child : NULL; ok && k; k = k->next)
        ok = cJSON_IsNumber(k) && prefs_set(k->string, k->valueint) == 0;
    cJSON_Delete(in);
    if (ok)
        reply_ok(c);
    else
        refuse(c, "400 Bad Request", "Unknown setting or value out of range.");
}

// GET /api/voice: the call, plus the controller audio, the pick and the broadcast.
static char *voice_json(void)
{
    char *s = voice_state_json();
    cJSON *o = cJSON_Parse(s ? s : "");
    free(s);
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "controller", cv_state());
    cJSON_AddStringToObject(o, "output", cv_tv() ? "tv" : "controller");
    // One state for the app: the picture (broadcast.c) and Discord's side (stream.c).
    const char *b = bc_state(), *st = stream_state();
    int failed = strcmp(st, "error") == 0 || strcmp(b, "error") == 0;
    cJSON *bc = cJSON_AddObjectToObject(o, "broadcast");
    cJSON_AddStringToObject(bc, "state", failed ? "error"
                                         : strcmp(b, "live") == 0 && strcmp(st, "live") == 0 ? "live"
                                         : strcmp(b, "off") == 0 && strcmp(st, "off") == 0   ? "off"
                                                                                           : "starting");
    cJSON_AddStringToObject(bc, "error", strcmp(st, "error") == 0 ? stream_error() : bc_error());
    cJSON_AddNumberToObject(bc, "viewers", stream_viewers());
    // Pairings: every console user, the one Go Live runs as, and the last pairing's state.
    rp_user users[RP_MAX_USERS];
    int n = rp_users(users, RP_MAX_USERS);
    int32_t used = rp_used(), fg = rp_foreground();
    char id[12];
    cJSON *list = cJSON_AddArrayToObject(bc, "users");
    for (int i = 0; i < n; i++) {
        cJSON *u = cJSON_CreateObject();
        snprintf(id, sizeof id, "%08x", (unsigned)users[i].id);
        cJSON_AddStringToObject(u, "id", id);
        cJSON_AddStringToObject(u, "name", users[i].name);
        cJSON_AddBoolToObject(u, "account", users[i].has_account);
        cJSON_AddBoolToObject(u, "paired", users[i].paired);
        cJSON_AddBoolToObject(u, "on_screen", users[i].id == fg);
        cJSON_AddStringToObject(u, "console", users[i].console);
        cJSON_AddNumberToObject(u, "since", (double)users[i].since);
        cJSON_AddItemToArray(list, u);
        if (users[i].id == used) {
            cJSON_AddStringToObject(bc, "pair_console", users[i].console);
            cJSON_AddStringToObject(bc, "pair_user", users[i].name);
            cJSON_AddNumberToObject(bc, "pair_since", (double)users[i].since);
        }
    }
    snprintf(id, sizeof id, "%08x", (unsigned)used);
    cJSON_AddStringToObject(bc, "use", used >= 0 ? id : "");
    cJSON_AddBoolToObject(bc, "paired", used >= 0);
    cJSON_AddStringToObject(bc, "pair_state", rp_pair_state());
    cJSON_AddStringToObject(bc, "pair_error", rp_pair_error());
    cJSON_AddStringToObject(bc, "controller", bc_controller());
    cJSON_AddItemToObject(o, "prefs", prefs_json());
    s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

// Where a system library's function sits in the app. A title's own dlsym finds
// nothing on 13.00, so the app asks the core, which reads the app's library
// table through the kernel; the app then calls the address itself.
static void api_dlsym(int c, const char *query)
{
    char pid[12], lib[64], sym[64];
    if (query_get(query, "pid", pid, sizeof pid) != 0 || !is_id(pid) || query_get(query, "lib", lib, sizeof lib) != 0 ||
        strncmp(lib, "libSce", 6) != 0 || query_get(query, "sym", sym, sizeof sym) != 0 || strncmp(sym, "sce", 3) != 0) {
        reply(c, "400 Bad Request", "text/plain", "pid lib sym", 11);
        return;
    }
    long addr = 0;
#ifdef __PROSPERO__
    uint32_t handle = 0;
    if (kernel_dynlib_handle(atoi(pid), lib, &handle) == 0)
        addr = (long)kernel_dynlib_dlsym(atoi(pid), handle, sym);
    LOG("dlsym %s %s in %s: 0x%lx", lib, sym, pid, addr);
#endif
    char json[48];
    snprintf(json, sizeof json, "{\"addr\":%ld}", addr > 0 ? addr : 0);
    reply(c, "200 OK", "application/json", json, strlen(json));
}

// The app polls every few seconds while it is on screen; a game in front suspends it
// (or the PS button closes it), and then it is silent. Only the app sends the native
// header. The core stays either way, for alerts and calls; the app's Quit stops it.
static _Atomic time_t app_seen;
static _Atomic int app_front = 1; // its last X-App-Front: 0 on the home screen or under the PS menu

int web_app_on_screen(void)
{
    return app_front && app_seen && time(NULL) - app_seen <= 8;
}

// The line is among the headers: body text cannot impersonate one.
static const char *header_line(const char *req, const char *body, const char *line)
{
    const char *at = strstr(req, line);
    return at && at < body ? at : NULL;
}

// Host, when sent, names this console: loopback, localhost or the address it was reached on.
static int host_ok(int c, const char *req, const char *body)
{
    const char *h = header_line(req, body, "\r\nHost: ");
    if (!h)
        return 1; // the app's IME lookup and other non-browser clients
    h += 8;
    size_t n = strcspn(h, ":\r");
    char self[16] = "127.0.0.1";
    struct sockaddr_in a;
    socklen_t alen = sizeof a;
    if (getsockname(c, (struct sockaddr *)&a, &alen) == 0 && a.sin_family == AF_INET) {
        unsigned ip = ntohl(a.sin_addr.s_addr);
        snprintf(self, sizeof self, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    }
    return (n == 9 && (strncmp(h, "127.0.0.1", 9) == 0 || strncmp(h, "localhost", 9) == 0)) ||
           (n == strlen(self) && strncmp(h, self, n) == 0);
}

// X-Core-Key: the per-boot key (core_key.h), which only the app can read.
static int key_header_ok(const char *req, const char *body)
{
    const char *h = header_line(req, body, "\r\nX-Core-Key: ");
    if (!h)
        return 0;
    h += 14;
    size_t n = strcspn(h, "\r\n");
    return h[n] == '\r' && core_key_ok(h, n);
}

// local: the peer is on this console (the native app). Chat data is only served to it.
static void handle(int c, int local)
{
    char req[16384]; // per connection: requests are served on their own threads
    size_t len = 0;
    req[0] = 0;
    char *body = NULL;
    while (len < sizeof req - 1) { // no memmem in the PS5 libc, so strstr on a NUL-terminated buffer
        if ((body = strstr(req, "\r\n\r\n"))) {
            body += 4;
            const char *cl = strstr(req, "Content-Length:");
            size_t want = cl && cl < body ? strtoul(cl + 15, NULL, 10) : 0;
            if ((size_t)(req + len - body) >= want)
                break;
        }
        ssize_t r = recv(c, req + len, sizeof req - 1 - len, 0);
        if (r <= 0)
            return;
        len += (size_t)r;
        req[len] = 0;
    }
    char method[8], target[512];
    if (!body || sscanf(req, "%7s %511s", method, target) != 2) {
        reply(c, "400 Bad Request", "text/plain", "bad request", 11);
        return;
    }
    char *query = strchr(target, '?');
    if (query)
        *query++ = 0;
    const char *path = target;
    int get = strcmp(method, "GET") == 0, post = strcmp(method, "POST") == 0;
    // A page in the console's own browser connects from 127.x too. Browsers always
    // send Host, so a name other than ours is DNS rebinding. The custom header makes
    // a cross-origin request need a preflight, which this server never grants. The key
    // keeps out what is no browser: another payload or app on the console, or the LAN.
    // /api/state stays open: it is how the app finds a core, and which one, before it has the key.
    if (!host_ok(c, req, body)) {
        reply(c, "403 Forbidden", "text/plain", "host", 4);
        return;
    }
    int open_path = strncmp(path, "/api/", 5) != 0 || (get && strcmp(path, "/api/state") == 0);
    if (!open_path && !header_line(req, body, "\r\nX-Discord-Native: 1\r\n")) {
        reply(c, "403 Forbidden", "text/plain", "native app only", 15);
        return;
    }
    if (!open_path && !key_header_ok(req, body)) {
        reply(c, "403 Forbidden", "text/plain", "key", 3);
        return;
    }
    if (local && header_line(req, body, "\r\nX-Discord-Native: 1\r\n") && key_header_ok(req, body)) {
        app_seen = time(NULL);
        app_front = !header_line(req, body, "\r\nX-App-Front: 0\r\n");
    }
    if (!local && strncmp(path, "/api/", 5) == 0 && strcmp(path, "/api/state") != 0 &&
        strcmp(path, "/api/quit") != 0) {
        reply(c, "403 Forbidden", "text/plain", "local only", 10);
        return;
    }

    if (get && strcmp(path, "/api/state") == 0) {
        core_key_keep(); // the app polls this first; a reinstall may have taken its key file
        reply_json(c, status_json());
    } else if (post && strcmp(path, "/api/browser-login") == 0) {
        if (browser_auth_request() == 0)
            reply(c, "202 Accepted", "application/json", "{\"ok\":true}", 11);
        else
            reply(c, "409 Conflict", "application/json", "{\"ok\":false}", 12);
    } else if (post && strcmp(path, "/api/browser-cancel") == 0) {
        browser_auth_cancel();
        reply(c, "200 OK", "application/json", "{\"ok\":true}", 11);
    } else if (get && strcmp(path, "/api/chat") == 0) {
        // ?v=: the version the app holds; unchanged, nothing is sent.
        char v[24] = "";
        if (query_get(query, "v", v, sizeof v) == 0 && v[0] && strtol(v, NULL, 10) == chat_tree_version())
            reply(c, "304 Not Modified", "application/json", "", 0);
        else
            reply_json(c, chat_tree_json());
    } else if (get && strcmp(path, "/api/events") == 0) {
        char since[24] = "0", wait[8] = "0";
        query_get(query, "since", since, sizeof since);
        query_get(query, "wait", wait, sizeof wait); // seconds, capped: a long poll
        int seconds = atoi(wait);
        reply_json(c, chat_events_json(strtol(since, NULL, 10), (seconds > 20 ? 20 : seconds < 0 ? 0 : seconds) * 1000));
    } else if (get && strcmp(path, "/api/messages") == 0) {
        api_messages(c, query);
    } else if (post && strcmp(path, "/api/send") == 0) {
        api_send(c, body);
    } else if (get && strcmp(path, "/api/mentions") == 0) {
        api_mentions(c);
    } else if (get && strcmp(path, "/api/image") == 0) {
        api_image(c, query);
    } else if (post && strcmp(path, "/api/ack") == 0) {
        api_ack(c, body);
    } else if (post && strcmp(path, "/api/react") == 0) {
        api_react(c, body);
    } else if (post && strcmp(path, "/api/dm") == 0) {
        api_dm(c, body);
    } else if (get && strcmp(path, "/api/voice") == 0) {
        reply_json(c, voice_json());
    } else if (post && strncmp(path, "/api/voice/", 11) == 0 && signed_in(c)) {
        if (strcmp(path, "/api/voice/join") == 0)
            api_voice_join(c, body);
        else if (strcmp(path, "/api/voice/call") == 0 || strcmp(path, "/api/voice/answer") == 0)
            api_voice_call(c, body);
        else if (strcmp(path, "/api/voice/decline") == 0)
            api_voice_decline(c, body);
        else if (strcmp(path, "/api/voice/leave") == 0)
            api_voice_leave(c);
        else if (strcmp(path, "/api/voice/set") == 0)
            api_voice_set(c, body);
        else if (strcmp(path, "/api/voice/output") == 0)
            api_voice_output(c, body);
        else if (strcmp(path, "/api/voice/broadcast") == 0)
            api_voice_broadcast(c, body);
        else
            reply(c, "404 Not Found", "text/plain", "not found", 9);
    } else if (post && (strcmp(path, "/api/remoteplay/pair") == 0 || strcmp(path, "/api/remoteplay/unpair") == 0 ||
                        strcmp(path, "/api/remoteplay/use") == 0)) {
        api_remoteplay(c, path, body);
    } else if (get && strcmp(path, "/api/sticker-packs") == 0) {
        api_sticker_packs(c);
    } else if (post && strcmp(path, "/api/prefs") == 0) {
        api_prefs(c, body);
    } else if (post && strcmp(path, "/api/reconnect") == 0) {
        gateway_retry();
        voice_retry();
        reply(c, "202 Accepted", "application/json", "{}", 2);
    } else if (post && strcmp(path, "/api/signout") == 0) {
        LOG("sign out requested");
        if (core_sign_out() != 0) {
            refuse(c, "502 Bad Gateway", "Could not reach Discord to sign out. Try again.");
        } else {
            reply(c, "200 OK", "application/json", "{\"ok\":true}", 11);
            close(c);
            core_quit(1);
        }
    } else if (post && strcmp(path, "/api/edit") == 0) {
        api_change(c, body, 0);
    } else if (post && strcmp(path, "/api/delete") == 0) {
        api_change(c, body, 1);
    } else if (get && strcmp(path, "/api/dlsym") == 0) {
        api_dlsym(c, query);
    } else if (post && strcmp(path, "/api/quit") == 0) {
        reply(c, "200 OK", "application/json", "{\"ok\":true}", 11);
        LOG("quit requested");
        close(c);
        core_quit(0);
    } else {
        reply(c, "404 Not Found", "text/plain", "not found", 9);
    }
}

// A thread per connection, MAX_CONNECTIONS at most (503 past that): an image fetch or a
// long poll does not hold the others. mbedTLS threading covers the shared DRBG.
struct conn {
    int c, local;
};

static int busy; // connections being served, guarded by busy_lock
static pthread_mutex_t busy_lock = PTHREAD_MUTEX_INITIALIZER;

static void *serve_one(void *arg)
{
    struct conn *k = arg;
    handle(k->c, k->local);
    close(k->c);
    free(k);
    pthread_mutex_lock(&busy_lock);
    busy--;
    pthread_mutex_unlock(&busy_lock);
    return NULL;
}

// Each connection on its own thread, so a slow one (an image from the CDN, a
// history page) does not hold up the app's polling. Capped: past the cap the
// request gets 503 at once, so the accept loop (and /api/state) never waits on one.
#define MAX_CONNECTIONS 16


void web_serve(int fd)
{
    for (int failures = 0;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int c = accept(fd, (struct sockaddr *)&peer, &plen);
        if (c < 0) {
            // One failure is a client that gave up; no descriptors left (EMFILE) passes as
            // requests finish. A run of anything else is a dead socket: the caller listens again.
            if (errno != EMFILE && errno != ENFILE && ++failures >= 20) {
                LOG("control: accept keeps failing (errno %d), listening again", errno);
                return;
            }
            usleep(100000);
            continue;
        }
        failures = 0;
        int local = peer.sin_family == AF_INET && ntohl(peer.sin_addr.s_addr) >> 24 == 127;
        struct timeval tv = {.tv_sec = 5};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        struct conn *k = malloc(sizeof *k);
        pthread_mutex_lock(&busy_lock);
        int room = busy < MAX_CONNECTIONS;
        if (room && k)
            busy++;
        pthread_mutex_unlock(&busy_lock);
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 512 * 1024);
        if (room && k) {
            k->c = c;
            k->local = local;
            if (pthread_create(&t, &attr, serve_one, k) == 0) {
                pthread_attr_destroy(&attr);
                continue;
            }
            pthread_mutex_lock(&busy_lock);
            busy--;
            pthread_mutex_unlock(&busy_lock);
        }
        pthread_attr_destroy(&attr);
        free(k);
        reply(c, "503 Service Unavailable", "text/plain", "busy", 4);
        close(c);
    }
}
