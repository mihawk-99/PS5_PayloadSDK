/*
 * PS5 Platform - the host's stand-in kernel, for tests (tests/host_kernel.c).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PS5PLATFORM_HOST_KERNEL_H
#define PS5PLATFORM_HOST_KERNEL_H

#include <stdbool.h>

enum {
   HOST_CALL_NONE,
   HOST_CALL_ALLOCATE,
   HOST_CALL_MAP,
   HOST_CALL_PROTECT,
   HOST_CALL_RESERVE,
};

/* The nth next call of that kind fails; with nth -1, every call fails until
 * host_fail(HOST_CALL_NONE, 0). */
void host_fail(int call, int nth);
/* Direct-memory allocations outstanding. */
long long host_direct_allocations(void);

#endif /* PS5PLATFORM_HOST_KERNEL_H */
