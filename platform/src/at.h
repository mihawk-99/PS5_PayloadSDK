/*
 * PS5 Platform - paths relative to directory descriptors (internal).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PS5PLATFORM_AT_H
#define PS5PLATFORM_AT_H

#include <stddef.h>

/* The path of name relative to directory (AT_FDCWD, or a descriptor
 * ps5_openat or ps5_opendir recorded) in out; 0, or -1 with errno set. */
int ps5p_resolve_at(int directory, const char *name, char *out, size_t size);

#endif /* PS5PLATFORM_AT_H */
