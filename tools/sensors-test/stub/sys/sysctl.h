#pragma once
#include <stddef.h>
int sysctlbyname(const char *name, void *old, size_t *oldlen, void *new_, size_t newlen);
