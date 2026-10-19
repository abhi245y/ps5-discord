#include "log.h"

#include <stdarg.h>
#include <stdio.h>

#include "status.h"

#ifdef __PROSPERO__
#include <ps5/klog.h>
#endif

void log_printf(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
#ifdef __PROSPERO__
    klog_printf("[discord] %s\n", line);
#else
    fprintf(stderr, "[discord] %s\n", line);
#endif
    status_log(line);
}
