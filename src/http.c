#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "props.h"
#include "tls.h"

#define API_HOST "discord.com"
#define API_BASE "/api/v9"
#define HTTP_MAX_REPLY (16 << 20)

// Sends head (and body) over a fresh TLS connection to host and reads the
// whole reply (HTTP/1.0: no chunking, ends at close).
static http_response exchange(const char *host, const char *head, size_t head_len, const char *body)
{
    http_response res = {.status = -1};
    tls_conn c;
    if (tls_connect(&c, host, "443") != 0) {
        tls_close(&c);
        return res;
    }
    if (tls_write_all(&c, head, head_len) != 0 || (body && tls_write_all(&c, body, strlen(body)) != 0)) {
        tls_close(&c);
        return res;
    }

    size_t len = 0, cap = 16384;
    char *buf = malloc(cap);
    for (int r; buf; len += (size_t)r) {
        if (cap - len < 4096) {
            char *p = cap < HTTP_MAX_REPLY ? realloc(buf, cap *= 2) : NULL; // no reply grows the core without end
            if (!p) {
                LOG("http: reply from %s over %d MiB or out of memory", host, HTTP_MAX_REPLY >> 20);
                free(buf);
                buf = NULL;
                break;
            }
            buf = p;
        }
        if ((r = tls_read(&c, buf + len, cap - len - 1)) <= 0)
            break;
    }
    tls_close(&c);
    if (!buf)
        return res;
    buf[len] = 0;
    res = http_parse_reply(buf, len);
    if (res.status < 0)
        LOG("http: bad response from %s", host);
    return res;
}

http_response http_parse_reply(char *buf, size_t len)
{
    http_response res = {.status = -1};
    char *start = strstr(buf, "\r\n\r\n");
    if (sscanf(buf, "HTTP/%*s %d", &res.status) != 1 || !start) {
        free(buf);
        res.status = -1;
        return res;
    }
    start += 4;
    res.body_len = len - (size_t)(start - buf);
    memmove(buf, start, res.body_len + 1);
    res.body = buf;
    return res;
}

http_response http_request(const char *method, const char *path, const char *json, const char *token)
{
    char *props = props_header();
    char auth[300] = "";
    if (token)
        snprintf(auth, sizeof auth, "Authorization: %s\r\n", token);
    // ponytail: HTTP/1.0 so the reply is never chunked and ends at close; keep-alive when REST grows.
    char head[2048], body_headers[96] = "";
    if (json)
        snprintf(body_headers, sizeof body_headers,
                 "Content-Type: application/json\r\nContent-Length: %zu\r\n", strlen(json));
    else if (strcmp(method, "GET") != 0) // PUT/DELETE without a body still need a length
        snprintf(body_headers, sizeof body_headers, "Content-Length: 0\r\n");
    int n = snprintf(head, sizeof head,
                     "%s %s%s HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s\r\n%s"
                     "Origin: https://discord.com\r\nX-Discord-Locale: en-US\r\n"
                     "X-Super-Properties: %s\r\n%s\r\n",
                     method, API_BASE, path, API_HOST, USER_AGENT, body_headers, props ? props : "", auth);
    free(props);
    if (n <= 0 || (size_t)n >= sizeof head)
        return (http_response){.status = -1};
    return exchange(API_HOST, head, (size_t)n, json);
}

http_response http_get(const char *host, const char *path)
{
    char head[2048];
    int n = snprintf(head, sizeof head, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s\r\n\r\n", path, host,
                     USER_AGENT);
    if (n <= 0 || (size_t)n >= sizeof head)
        return (http_response){.status = -1};
    return exchange(host, head, (size_t)n, NULL);
}

int http_split_url(const char *url, char host[256], char path[3072])
{
    if (strncmp(url, "https://", 8) != 0)
        return -1;
    const char *h = url + 8, *slash = strchr(h, '/');
    size_t n = slash ? (size_t)(slash - h) : strlen(h);
    if (n == 0 || n >= 256 || strcspn(h, ":@?#") < n || strlen(slash ? slash : "/") >= 3072)
        return -1;
    memcpy(host, h, n);
    host[n] = 0;
    snprintf(path, 3072, "%s", slash ? slash : "/");
    return 0;
}

// One GET: the head is read into buf, then the body goes to sink. 3xx: Location in loc.
static int get_once(const char *host, const char *path, unsigned long long limit, http_sink sink, void *user,
                    char *loc, size_t loc_cap)
{
    char head[4096]; // GitHub's redirect carries a signed query of about 1 KB
    int n = snprintf(head, sizeof head, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\n\r\n", path,
                     host, USER_AGENT);
    if (n <= 0 || (size_t)n >= sizeof head)
        return -1;
    tls_conn c;
    if (tls_connect(&c, host, "443") != 0 || tls_write_all(&c, head, (size_t)n) != 0) {
        tls_close(&c);
        return -1;
    }
    char buf[16384];
    size_t len = 0;
    char *end = NULL;
    int status = -1;
    unsigned long long got = 0;
    for (int r; (r = tls_read(&c, buf + len, sizeof buf - 1 - len)) > 0;) {
        len += (size_t)r;
        buf[len] = 0;
        if (!end) {
            if (!(end = strstr(buf, "\r\n\r\n"))) {
                if (len == sizeof buf - 1)
                    break; // a head this long is no reply of ours
                continue;
            }
            end += 4;
            if (sscanf(buf, "HTTP/%*s %d", &status) != 1)
                break;
            if (status >= 300 && status < 400) {
                const char *l = strstr(buf, "\r\nLocation: ");
                if (!l)
                    l = strstr(buf, "\r\nlocation: ");
                size_t ln = l ? strcspn(l + 12, "\r\n") : 0;
                if (!l || ln >= loc_cap)
                    status = -1;
                else
                    snprintf(loc, loc_cap, "%.*s", (int)ln, l + 12);
                break;
            }
            if (status != 200)
                break;
            size_t body = len - (size_t)(end - buf);
            memmove(buf, end, body);
            len = body;
        }
        if ((got += len) > limit || (len && !sink(user, buf, len))) {
            status = -1;
            break;
        }
        len = 0;
    }
    tls_close(&c);
    return end ? status : -1;
}

int http_get_url(const char *url, int (*allowed)(const char *host), unsigned long long limit, http_sink sink,
                 void *user)
{
    char host[256] = "", path[3072], loc[3072];
    snprintf(loc, sizeof loc, "%s", url);
    for (int hops = 0; hops < 4; hops++) {
        if (http_split_url(loc, host, path) != 0 || !allowed(host)) {
            LOG("http: refused %s", host);
            return -1;
        }
        int status = get_once(host, path, limit, sink, user, loc, sizeof loc);
        if (status < 300 || status >= 400)
            return status;
    }
    return -1;
}
