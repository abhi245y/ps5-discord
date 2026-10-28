// SPDX-License-Identifier: AGPL-3.0-or-later
// The user's settings that live on the console, one file per console user ("key value"
// a line), so they outlast the app and the core. Each key is a small integer in a range.
#pragma once
#include <stdint.h>

#include <cJSON.h>

// The console user's settings (DATA_DIR/prefs-<user>); before the first, the defaults.
void prefs_load(const char *data_dir, int32_t user);
int prefs_get(const char *key);                  // the default when never set
int prefs_set(const char *key, int value);       // 0 when known and in range; saved at once
cJSON *prefs_json(void);                         // every key and its value
