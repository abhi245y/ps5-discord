// One-shot HTTPS requests to discord.com/api/v9.
#pragma once
#include <stddef.h>

typedef struct {
    int status;  // HTTP status, or -1 on transport failure
    char *body;  // NUL-terminated, caller frees
    size_t body_len;
} http_response;

// POST JSON to path (e.g. "/users/@me/remote-auth/login"). token may be NULL.
http_response http_post_json(const char *path, const char *json, const char *token);
