#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "props.h"
#include "tls.h"

#define API_HOST "discord.com"
#define API_BASE "/api/v9"

http_response http_request(const char *method, const char *path, const char *json, const char *token)
{
    http_response res = {.status = -1};
    tls_conn c;
    if (tls_connect(&c, API_HOST, "443") != 0) {
        tls_close(&c);
        return res;
    }
    char *props = props_header();
    char auth[300] = "";
    if (token)
        snprintf(auth, sizeof auth, "Authorization: %s\r\n", token);
    // ponytail: HTTP/1.0 so the reply is never chunked and ends at close; keep-alive when REST grows.
    char head[2048], body_headers[96] = "";
    if (json)
        snprintf(body_headers, sizeof body_headers,
                 "Content-Type: application/json\r\nContent-Length: %zu\r\n", strlen(json));
    int n = snprintf(head, sizeof head,
                     "%s %s%s HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s\r\n%s"
                     "Origin: https://discord.com\r\nX-Discord-Locale: en-US\r\n"
                     "X-Super-Properties: %s\r\n%s\r\n",
                     method, API_BASE, path, API_HOST, USER_AGENT, body_headers, props ? props : "", auth);
    free(props);
    if (n <= 0 || (size_t)n >= sizeof head || tls_write_all(&c, head, (size_t)n) != 0 ||
        (json && tls_write_all(&c, json, strlen(json)) != 0)) {
        tls_close(&c);
        return res;
    }

    size_t len = 0, cap = 16384;
    char *buf = malloc(cap);
    for (int r; buf; len += (size_t)r) {
        if (cap - len < 4096) {
            char *p = realloc(buf, cap *= 2);
            if (!p) {
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

    char *body = strstr(buf, "\r\n\r\n");
    if (sscanf(buf, "HTTP/%*s %d", &res.status) != 1 || !body) {
        LOG("http: bad response to %s", path);
        free(buf);
        res.status = -1;
        return res;
    }
    body += 4;
    res.body_len = len - (size_t)(body - buf);
    memmove(buf, body, res.body_len + 1);
    res.body = buf;
    return res;
}
