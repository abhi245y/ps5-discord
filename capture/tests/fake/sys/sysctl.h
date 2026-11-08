#pragma once
#include <stddef.h>
int sysctl(const int *name, unsigned namelen, void *oldp, size_t *oldlenp, const void *newp, size_t newlen);
