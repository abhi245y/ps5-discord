#include "web.h"

#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cJSON.h>

#include "chat.h"
#include "gateway.h"
#include "http.h"
#include "log.h"
#include "ratelimit.h"
#include "status.h"
#include "voice.h"
#include "voice_audio.h"
#include "controller_voice.h"
#include "browser_auth.h"

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#endif

extern const unsigned char web_index_html[];
extern const size_t web_index_html_len;

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
    http_response res = http_request(method, path, json, chat_token());
    const double wait = ratelimit_retry_after(&res, 5.0);
    if (wait > 0) {
        LOG("rate limited on %s %s, retrying in %.2f s", method, path, wait);
        free(res.body);
        usleep((useconds_t)(wait * 1e6) + 50000);
        res = http_request(method, path, json, chat_token());
    }
    return res;
}

static void reply_rest(int c, http_response res)
{
    const char *body = res.body && res.body[0] ? res.body : "null";
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
// Discord's CDN hosts: the console must not become a general web proxy.
static void api_image(int c, const char *query)
{
    char url[1024];
    if (query_get(query, "url", url, sizeof url) != 0) {
        reply(c, "400 Bad Request", "text/plain", "url", 3);
        return;
    }
    url_decode(url);
    const char *rest = strncmp(url, "https://", 8) == 0 ? url + 8 : NULL;
    char host[128];
    size_t hlen = rest ? strcspn(rest, "/") : 0;
    int allowed = 0;
    if (rest && hlen > 0 && hlen < sizeof host) {
        memcpy(host, rest, hlen);
        host[hlen] = 0;
        allowed = strcmp(host, "cdn.discordapp.com") == 0 || strcmp(host, "media.discordapp.net") == 0 ||
                  (strncmp(host, "images-ext-", 11) == 0 && strstr(host, ".discordapp.net") != NULL) ||
                  // Colour emoji pictures (Twemoji, as Discord draws them), that project only.
                  (strcmp(host, "cdn.jsdelivr.net") == 0 &&
                   strncmp(rest + hlen, "/gh/jdecked/twemoji@", 20) == 0 && !strstr(rest + hlen, ".."));
    }
    if (!allowed) {
        reply(c, "403 Forbidden", "text/plain", "host", 4);
        return;
    }
    http_response res = http_get(host, rest + hlen);
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
    // Byte cap only; Discord enforces its 2000 character limit itself.
    if (!channel || !is_id(channel) || !content || !*content || strlen(content) > 8000) {
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

// Adds the account's reaction: {channel, message, emoji} (a Unicode emoji).
static void api_react(int c, const char *body)
{
    cJSON *in = cJSON_Parse(body ? body : "");
    const char *channel = cJSON_GetStringValue(cJSON_GetObjectItem(in, "channel"));
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItem(in, "message"));
    const char *emoji = cJSON_GetStringValue(cJSON_GetObjectItem(in, "emoji"));
    if (!channel || !is_id(channel) || !message || !is_id(message) || !emoji || !*emoji || strlen(emoji) > 32) {
        cJSON_Delete(in);
        reply(c, "400 Bad Request", "text/plain", "react", 5);
        return;
    }
    char path[256];
    int n = snprintf(path, sizeof path, "/channels/%s/messages/%s/reactions/", channel, message);
    for (const unsigned char *e = (const unsigned char *)emoji; *e && n < (int)sizeof path - 16; e++)
        n += snprintf(path + n, sizeof path - n, "%%%02X", *e);
    snprintf(path + n, sizeof path - n, "/@me?location=Message");
    cJSON_Delete(in);
    reply_rest(c, rest("PUT", path, NULL));
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
static void api_voice_call(int c, const char *body)
{
    cJSON *in = cJSON_Parse(body ? body : "");
    const char *channel = body_channel(c, in);
    if (channel && !chat_call_active(channel)) {
        char path[96];
        snprintf(path, sizeof path, "/channels/%s/call/ring", channel);
        http_response res = rest("POST", path, "{\"recipients\":null}");
        free(res.body);
        if (res.status < 200 || res.status > 299) {
            refuse(c, "409 Conflict", "Discord would not ring that call.");
            channel = NULL;
        }
    }
    if (channel) {
        voice_join(NULL, channel);
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

// GET /api/voice: the call, plus the controller audio and the pick.
static char *voice_json(void)
{
    char *s = voice_state_json();
    cJSON *o = cJSON_Parse(s ? s : "");
    free(s);
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "controller", cv_state());
    cJSON_AddStringToObject(o, "output", cv_tv() ? "tv" : "controller");
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
    // a cross-origin request need a preflight, which this server never grants.
    if (!host_ok(c, req, body)) {
        reply(c, "403 Forbidden", "text/plain", "host", 4);
        return;
    }
    if (strncmp(path, "/api/", 5) == 0 && !(get && strcmp(path, "/api/state") == 0) &&
        !header_line(req, body, "\r\nX-Discord-Native: 1\r\n")) {
        reply(c, "403 Forbidden", "text/plain", "native app only", 15);
        return;
    }
    if (!local && strncmp(path, "/api/", 5) == 0 && strcmp(path, "/api/state") != 0 &&
        strcmp(path, "/api/quit") != 0) {
        reply(c, "403 Forbidden", "text/plain", "local only", 10);
        return;
    }

    if (get && strcmp(path, "/") == 0) {
        reply(c, "200 OK", "text/html; charset=utf-8", web_index_html, web_index_html_len - 1);
    } else if (get && strcmp(path, "/api/state") == 0) {
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
        else
            reply(c, "404 Not Found", "text/plain", "not found", 9);
    } else if (post && strcmp(path, "/api/edit") == 0) {
        api_change(c, body, 0);
    } else if (post && strcmp(path, "/api/delete") == 0) {
        api_change(c, body, 1);
    } else if (get && strcmp(path, "/api/dlsym") == 0) {
        api_dlsym(c, query);
    } else if (post && strcmp(path, "/api/quit") == 0) {
        reply(c, "200 OK", "application/json", "{\"ok\":true}", 11);
        LOG("quit requested from the web UI");
        close(c);
        _exit(0);
    } else {
        reply(c, "404 Not Found", "text/plain", "not found", 9);
    }
}

// ponytail: one connection at a time; an image fetch (~0.3 s) holds the next request.
// Threads need mbedTLS threading (shared DRBG) first.
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
// accept loop serves it itself, which also slows accepting down.
#define MAX_CONNECTIONS 8

#if CORE_FOLLOWS_APP
// Test builds: the app starts the core, so the core leaves with it. The app
// polls every few seconds; once it has been seen, a long silence means it closed.
// A game suspends the app the same way, so a wanted call keeps the core.
#define APP_GONE_SECONDS 30
static volatile time_t app_seen;

static void *follow_app(void *arg)
{
    (void)arg;
    for (;;) {
        sleep(5);
        if (app_seen && time(NULL) - app_seen > APP_GONE_SECONDS && !browser_auth_keepalive() &&
            !voice_wanted()) {
            LOG("the app closed: the core leaves with it");
            _exit(0);
        }
    }
}
#endif

void web_serve(int fd)
{
#if CORE_FOLLOWS_APP
    pthread_t follower;
    if (pthread_create(&follower, NULL, follow_app, NULL) == 0)
        pthread_detach(follower);
#endif
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int c = accept(fd, (struct sockaddr *)&peer, &plen);
        if (c < 0)
            continue;
        int local = peer.sin_family == AF_INET && ntohl(peer.sin_addr.s_addr) >> 24 == 127;
#if CORE_FOLLOWS_APP
        if (local)
            app_seen = time(NULL);
#endif
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
        handle(c, local);
        close(c);
    }
}
