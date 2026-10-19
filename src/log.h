// Logging: klog on the console, stderr on the host.
#pragma once

#ifdef __PROSPERO__
#include <ps5/klog.h>
#define LOG(fmt, ...) klog_printf("[discord] " fmt "\n", ##__VA_ARGS__)
#else
#include <stdio.h>
#define LOG(fmt, ...) fprintf(stderr, "[discord] " fmt "\n", ##__VA_ARGS__)
#endif
