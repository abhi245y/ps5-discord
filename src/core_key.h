// SPDX-License-Identifier: AGPL-3.0-or-later
// The per-boot key the app shows to the control API (X-Core-Key) and as the first
// bytes on the audio link. It is written to the app's install folder, which the app
// reads as /app0/control.key (a title's sandbox has no /data), so only the app has it:
// a page in the console's browser or another payload cannot drive the core, listen
// to a call or knock the app off it.
#pragma once

#include <stddef.h>

#define CORE_KEY_LEN 64 // hex characters

// From 32 random bytes: kept in memory and written to dir/control.key (0644). 0 on success;
// the key is set either way, so a failed write only locks the app out, never opens the core.
int core_key_make(const char *dir, const unsigned char raw[32]);
// Writes the file again if it is gone (the app was reinstalled while the core ran).
void core_key_keep(void);
// s holds n bytes (not NUL-terminated): 1 when they are the key. Constant time.
int core_key_ok(const char *s, size_t n);
