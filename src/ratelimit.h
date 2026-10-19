#pragma once

#include "http.h"

// Seconds to wait before retrying a REST call Discord rate-limited (429 with
// retry_after), or 0 when it is not worth retrying (not a 429, or too long).
double ratelimit_retry_after(const http_response *res, double longest);
