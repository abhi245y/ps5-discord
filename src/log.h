// Logging: klog on the console, stderr on the host, and the web UI's log view.
#pragma once

void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(fmt, ...) log_printf(fmt, ##__VA_ARGS__)
