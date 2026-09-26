/*
 * PS5 Platform - the live counters as one report line (include/ps5platform/platform.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ps5platform/platform.h"

#include <stdio.h>

int
ps5_platform_report(char *line, size_t size)
{
   uint64_t regions = 0, bytes = 0;
   ps5_exec_live(&regions, &bytes);
   struct ps5_shm_stats shm;
   ps5_shm_live(&shm);
   return snprintf(line, size,
                   "platform exec=%llu/%lluMiB shm=%llu/%lluMiB views=%llu ranges=%llu/%lluMiB",
                   (unsigned long long)regions, (unsigned long long)(bytes >> 20),
                   (unsigned long long)shm.objects, (unsigned long long)(shm.object_bytes >> 20),
                   (unsigned long long)shm.views, (unsigned long long)shm.ranges,
                   (unsigned long long)(shm.range_bytes >> 20));
}
