#pragma once
#include <stddef.h>

// Read only the active Discord token from WebKit's SQLite storage. No logging.
int browser_storage_token(const char *path, char *out, size_t cap);
// Atomic owner-only session persistence, as directory/name. Returns -1 without replacing on failure.
int session_save(const char *directory, const char *name, const char *token);
// Zeroes a secret; unlike a plain memset, never optimised away.
void storage_wipe(void *p, size_t n);
