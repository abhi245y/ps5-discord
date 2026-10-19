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
