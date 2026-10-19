// Client identity sent in IDENTIFY and, base64'd, as X-Super-Properties on REST.
// Mirrors the web client (build 641004, docs/gateway.md). Bump with the web build.
#pragma once
#include <cJSON.h>

#define CLIENT_BUILD 641004
#define BROWSER_VERSION "152.0.0.0"
#define USER_AGENT "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" \
                   BROWSER_VERSION " Safari/537.36"

cJSON *props_create(void);
// Base64 of the properties JSON for the X-Super-Properties header; caller frees.
char *props_header(void);
