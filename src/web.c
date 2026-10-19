#include "web.h"

#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

static void handle(int c)
{
    char req[2048];
    size_t len = 0;
    req[0] = 0;
    while (len < sizeof req - 1 && !strstr(req, "\r\n\r\n")) { // no memmem in the PS5 libc
        ssize_t r = recv(c, req + len, sizeof req - 1 - len, 0);
        if (r <= 0)
            return;
        len += (size_t)r;
        req[len] = 0;
    }
    char method[8], path[256];
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        reply(c, "400 Bad Request", "text/plain", "bad request", 11);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        reply(c, "200 OK", "text/html; charset=utf-8", web_index_html, web_index_html_len - 1);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/state") == 0) {
        char *j = status_json();
        reply(c, "200 OK", "application/json", j ? j : "{}", j ? strlen(j) : 2);
        free(j);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/quit") == 0) {
        reply(c, "200 OK", "application/json", "{\"ok\":true}", 11);
        LOG("quit requested from the web UI");
        close(c);
        _exit(0);
    } else {
        reply(c, "404 Not Found", "text/plain", "not found", 9);
    }
}

void web_serve(int fd)
{
    for (;;) {
        int c = accept(fd, NULL, NULL);
        if (c < 0)
            continue;
        struct timeval tv = {.tv_sec = 5};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        handle(c);
        close(c);
    }
}
