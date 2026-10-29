#include "ratelimit.h"

#include <cJSON.h>

double ratelimit_retry_after(const http_response *res, double longest)
{
    if (!res || res->status != 429 || !res->body)
        return 0;
    cJSON *body = cJSON_Parse(res->body);
    // Seconds with a fraction (0.5): a double, not json_int; NaN when missing fails the test below.
    const double wait = cJSON_GetNumberValue(cJSON_GetObjectItem(body, "retry_after"));
    cJSON_Delete(body);
    return wait > 0 && wait <= longest ? wait : 0; // NaN (no field) fails both tests
}
