// One-shot HTTPS requests to discord.com/api/v9.
#pragma once
#include <stddef.h>

typedef struct {
    int status;  // HTTP status, or -1 on transport failure
    char *body;  // NUL-terminated (binary bodies too: use body_len), caller frees
    size_t body_len;
} http_response;

// Request to path under /api/v9 (e.g. "/channels/1/messages?limit=50").
// json is the request body or NULL; token may be NULL.
http_response http_request(const char *method, const char *path, const char *json, const char *token);
#define http_post_json(path, json, token) http_request("POST", path, json, token)

// GET https://host/path with no Discord headers (CDN images). Body may be binary.
http_response http_get(const char *host, const char *path);
// A whole HTTP/1.0 reply (len bytes, buf[len] == 0) into status and body; takes buf (freed
// when it is not a reply: status -1, no header end).
http_response http_parse_reply(char *buf, size_t len);

// GET an https URL, following up to 3 redirects to a host allowed(host) accepts, and hand
// the body to sink in pieces (sink returns 0 to stop). Refuses a body over limit bytes.
// Returns the final HTTP status, or -1 (transport, a refused redirect, the limit, sink).
typedef int (*http_sink)(void *user, const void *data, size_t len);
int http_get_url(const char *url, int (*allowed)(const char *host), unsigned long long limit, http_sink sink,
                 void *user);
// Splits https://host/path (host < 256, path < 3072 bytes). 0 on success.
int http_split_url(const char *url, char host[256], char path[3072]);
