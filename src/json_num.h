// Numbers from Discord's JSON as integers. A cast of a missing field (cJSON gives NaN) is
// undefined, so missing, non-numeric or out-of-range values give the fallback instead.
#pragma once
#include <stdint.h>

#include <cJSON.h>

static inline int64_t json_item_int(const cJSON *v, int64_t fallback)
{
    if (!cJSON_IsNumber(v) || !(v->valuedouble > -9.0e18 && v->valuedouble < 9.0e18))
        return fallback;
    return (int64_t)v->valuedouble;
}

static inline int64_t json_int(const cJSON *o, const char *key, int64_t fallback)
{
    return json_item_int(cJSON_GetObjectItem(o, key), fallback);
}
