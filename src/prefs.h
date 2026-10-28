// SPDX-License-Identifier: AGPL-3.0-or-later
// The user's settings that live on the console (DATA_DIR/prefs, one "key value" a line),
// so they outlast the app and the core. Each key is a small integer in a fixed range.
#pragma once
#include <cJSON.h>

void prefs_init(const char *data_dir);
int prefs_get(const char *key);                  // the default when never set
int prefs_set(const char *key, int value);       // 0 when known and in range; saved at once
cJSON *prefs_json(void);                         // every key and its value
