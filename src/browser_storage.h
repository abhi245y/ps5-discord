#pragma once
#include <stddef.h>

// Read only the active Discord token from WebKit's SQLite storage. No logging.
int browser_storage_token(const char *path, char *out, size_t cap);
// Atomic owner-only session persistence, as directory/name, encrypted to this console (see
// browser_storage.c). Returns -1 without replacing on failure; never writes the token in the clear.
int session_save(const char *directory, const char *name, const char *token);
// The session saved as directory/name: 0, or 1 when it was still in the clear (an older
// build's file; save it again), -1 when missing, damaged or not this console's.
int session_load(const char *directory, const char *name, char *out, size_t cap);
// Zeroes a secret; unlike a plain memset, never optimised away.
void storage_wipe(void *p, size_t n);
