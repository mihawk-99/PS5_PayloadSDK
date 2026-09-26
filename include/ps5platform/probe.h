/*
 * PS5 Platform - the console capability probe.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Establishes, on the console and in the calling process, the memory
 * behaviour this layer builds on: the direct-memory pool, executable code in
 * direct memory, its placement, rewriting, fault recovery, protection costs
 * and accounting. Every result is one line through the caller's log; nothing
 * is printed per frame and nothing is left mapped when it returns.
 */
#ifndef PS5PLATFORM_PROBE_H
#define PS5PLATFORM_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ps5_probe_log_fn)(void *context, const char *line);

/* The large test: 4 GiB of guest memory beside 1 GiB of executable code. */
#define PS5_PROBE_LARGE 0x1u
/* The shared-memory JIT interface, which may not be granted to a title. */
#define PS5_PROBE_JIT_API 0x2u

/* Runs every test and returns how many checks failed. */
int ps5_platform_probe(ps5_probe_log_fn log, void *context, unsigned flags);

#ifdef __cplusplus
}
#endif

#endif /* PS5PLATFORM_PROBE_H */
