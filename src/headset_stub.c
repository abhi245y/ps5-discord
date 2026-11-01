// SPDX-License-Identifier: AGPL-3.0-or-later
// The host build has no USB Bluetooth adapter.
#include "headset.h"

void hs_init(const char *data_dir, const char *firmware_dir)
{
    (void)data_dir;
    (void)firmware_dir;
}

int hs_power(int on)
{
    (void)on;
    return -1;
}

void hs_shutdown(void)
{
}

int hs_scan(void)
{
    return -1;
}

int hs_connect(const char *addr)
{
    (void)addr;
    return -1;
}

int hs_disconnect(void)
{
    return -1;
}

int hs_forget(const char *addr)
{
    (void)addr;
    return -1;
}

cJSON *hs_state_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "on", 0);
    cJSON_AddStringToObject(o, "state", "unavailable");
    return o;
}
