#include "web.h"

#include <sys/time.h>
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
#include "status.h"

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
    reply_rest(c, http_request("GET", path, NULL, chat_token()));
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
    reply_rest(c, http_request("GET", "/users/@me/mentions?limit=25&roles=true&everyone=true", NULL, chat_token()));
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
                  (strncmp(host, "images-ext-", 11) == 0 && strstr(host, ".discordapp.net") != NULL);
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
    reply_rest(c, http_request("POST", path, "{\"token\":null}", chat_token()));
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
    reply_rest(c, http_request("POST", path, json, chat_token()));
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
    reply_rest(c, http_request("PUT", path, NULL, chat_token()));
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
    reply_rest(c, http_request("POST", "/users/@me/channels", json, chat_token()));
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
    if (sscanf(req, "%7s %511s", method, target) != 2) {
        reply(c, "400 Bad Request", "text/plain", "bad request", 11);
        return;
    }
    char *query = strchr(target, '?');
    if (query)
        *query++ = 0;
    const char *path = target;
    int get = strcmp(method, "GET") == 0, post = strcmp(method, "POST") == 0;
    if (!local && strncmp(path, "/api/", 5) == 0 && strcmp(path, "/api/state") != 0 &&
        strcmp(path, "/api/quit") != 0) {
        reply(c, "403 Forbidden", "text/plain", "local only", 10);
        return;
    }

    if (get && strcmp(path, "/") == 0) {
        reply(c, "200 OK", "text/html; charset=utf-8", web_index_html, web_index_html_len - 1);
    } else if (get && strcmp(path, "/api/state") == 0) {
        reply_json(c, status_json());
    } else if (get && strcmp(path, "/api/chat") == 0) {
        reply_json(c, chat_tree_json());
    } else if (get && strcmp(path, "/api/events") == 0) {
        char since[24] = "0";
        query_get(query, "since", since, sizeof since);
        reply_json(c, chat_events_json(strtol(since, NULL, 10)));
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

void web_serve(int fd)
{
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int c = accept(fd, (struct sockaddr *)&peer, &plen);
        if (c < 0)
            continue;
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
        handle(c, local);
        close(c);
    }
}
