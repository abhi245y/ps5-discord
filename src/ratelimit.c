#include "ratelimit.h"

#include <cJSON.h>

double ratelimit_retry_after(const http_response *res, double longest)
{
    if (!res || res->status != 429 || !res->body)
        return 0;
    cJSON *body = cJSON_Parse(res->body);
    const double wait = cJSON_GetNumberValue(cJSON_GetObjectItem(body, "retry_after"));
    cJSON_Delete(body);
    return wait > 0 && wait <= longest ? wait : 0; // NaN (no field) fails both tests
}
