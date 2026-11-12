// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../src/prefs.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
void log_printf(const char *format, ...)
{
    (void)format;
}
int main(void)
{
    char directory[] = "/tmp/controller-prefs-test-XXXXXX";
    assert(mkdtemp(directory));
    prefs_load(directory, 1);
    assert(prefs_get("controller_audio") == 0);
    assert(prefs_set("controller_audio", 1) == 0);
    assert(prefs_set("controller_audio", 2) == -1);
    assert(prefs_set("controller_audio", -1) == -1);
    prefs_load(directory, 2);
    assert(prefs_get("controller_audio") == 0);
    prefs_load(directory, 1);
    assert(prefs_get("controller_audio") == 1);
    cJSON *json = prefs_json();
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(json, "controller_audio")) == 1);
    cJSON_Delete(json);
    assert(prefs_set("controller_audio", 0) == 0);
    prefs_load(directory, 1);
    assert(prefs_get("controller_audio") == 0);
    char path[256];
    snprintf(path, sizeof path, "%s/prefs-00000001", directory);
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
    puts("Controller preference: default off, validation, per-user persistence, JSON and disable "
         "reload passed");
    return 0;
}
