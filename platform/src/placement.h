/*
 * PS5 Platform - where mappings go (internal).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console gives a mapping asked for with no address a place in the Vulkan
 * driver's GPU window (docs/PROBE.md), which the driver needs for itself, so
 * nothing here maps without an address: views go at a hint in the area above
 * the window, and anything that lands in the window anyway is unmapped and
 * placed again further on.
 */
#ifndef PS5PLATFORM_PLACEMENT_H
#define PS5PLATFORM_PLACEMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PS5P_GPU_WINDOW_LOW ((uintptr_t)0x200000000ull)
#define PS5P_GPU_WINDOW_HIGH ((uintptr_t)0x300000000ull)
/* Where placed-anywhere views start: above the window. */
#define PS5P_VIEW_AREA ((uintptr_t)0x400000000ull)
/* How far code near an anchor may be from it: a signed 32-bit displacement,
 * less 64 MiB for the anchor's own image. */
#define PS5P_NEAR_REACH ((uintptr_t)0x7c000000ull)
#define PS5P_DIRECT_UNIT ((size_t)0x10000)
#define PS5P_PAGE ((size_t)0x4000)

static inline size_t
ps5p_round_up(size_t value, size_t unit)
{
   return (value + unit - 1) / unit * unit;
}

static inline bool
ps5p_outside_gpu_window(uintptr_t base, size_t bytes)
{
   return base >= PS5P_GPU_WINDOW_HIGH || base + bytes <= PS5P_GPU_WINDOW_LOW;
}

/* Maps [direct_start, +bytes) with the protection at the hint (or in the view
 * area without one), never in the GPU window. Returns 0 and the address, or
 * the kernel's result, or a PS5_*_NO_PLACE code from no_place. */
int ps5p_map_placed(int64_t direct_start, size_t bytes, uintptr_t hint, int protection, int no_place,
                    void **address);

/* Reserves bytes of address space at the hint (or in the view area), outside
 * the GPU window. */
int ps5p_reserve_placed(size_t bytes, uintptr_t hint, size_t alignment, int no_place, void **address);

/* Reserves bytes of address space whose every byte is within PS5P_NEAR_REACH of
 * the anchor, outside the GPU window; the hint, when given and suitable, first. */
int ps5p_reserve_near(size_t bytes, uintptr_t anchor, uintptr_t hint, int no_place, void **address);

#endif /* PS5PLATFORM_PLACEMENT_H */
