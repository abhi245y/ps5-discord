#include "props.h"

#include <stdlib.h>
#include <string.h>

#include <mbedtls/base64.h>

cJSON *props_create(void)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "os", "Linux");
    cJSON_AddStringToObject(p, "browser", "Chrome");
    cJSON_AddStringToObject(p, "device", "");
    cJSON_AddStringToObject(p, "system_locale", "en-US");
    cJSON_AddFalseToObject(p, "has_client_mods");
    cJSON_AddStringToObject(p, "browser_user_agent", USER_AGENT);
    cJSON_AddStringToObject(p, "browser_version", BROWSER_VERSION);
    cJSON_AddStringToObject(p, "os_version", "");
    cJSON_AddStringToObject(p, "referrer", "");
    cJSON_AddStringToObject(p, "referring_domain", "");
    cJSON_AddStringToObject(p, "release_channel", "stable");
    cJSON_AddNumberToObject(p, "client_build_number", CLIENT_BUILD);
    cJSON_AddNullToObject(p, "client_event_source");
    return p;
}

char *props_header(void)
{
    cJSON *p = props_create();
    char *json = cJSON_PrintUnformatted(p);
    cJSON_Delete(p);
    if (!json)
        return NULL;
    size_t len = strlen(json), cap = 4 * (len / 3 + 1) + 1, olen;
    char *out = malloc(cap);
    if (out && mbedtls_base64_encode((unsigned char *)out, cap, &olen, (unsigned char *)json, len) != 0) {
        free(out);
        out = NULL;
    }
    free(json);
    return out;
}
