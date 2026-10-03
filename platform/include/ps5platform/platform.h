/*
 * PS5 Platform - everything, and the live counters for a memory report.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PS5PLATFORM_PLATFORM_H
#define PS5PLATFORM_PLATFORM_H

#include "ps5platform/context.h"
#include "ps5platform/elevation.h"
#include "ps5platform/exec.h"
#include "ps5platform/fp.h"
#include "ps5platform/heap.h"
#include "ps5platform/kernel.h"
#include "ps5platform/klog.h"
#include "ps5platform/libc.h"
#include "ps5platform/shm.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One line for a memory report, in the form the Vulkan driver's direct_live
 * takes: "platform exec=<regions>/<MiB>MiB shm=<objects>/<MiB>MiB
 * views=<views> ranges=<ranges>/<MiB>MiB". Returns the line's length. */
int ps5_platform_report(char *line, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* PS5PLATFORM_PLATFORM_H */
