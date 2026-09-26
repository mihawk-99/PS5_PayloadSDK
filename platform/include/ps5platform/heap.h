/*
 * PS5 Platform - the title heap: malloc and its family in direct memory.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console's libc serves malloc from a private heap that runs out long
 * before the title does: the Vulkan CTS exhausted it building its first
 * shader (an 8 KiB operator new refused), and the RetroArch title's cores did
 * the same with a few hundred MB of flexible memory still free. Direct memory
 * is a separate pool of 12 GiB (docs/PROBE.md).
 *
 * The title heap serves the title's own allocations from direct memory:
 * dlmalloc 2.8.6 (src/dlmalloc, MIT) as one locked mspace whose segments are
 * direct memory mapped CPU read-write inside one reserved range, so a pointer
 * is the heap's exactly when it lies in that range. libc's private heap is
 * left to the system libraries, which allocate from it themselves.
 *
 * A title links it with these flags, which route every reference its own
 * objects make (C, C++ through libc++'s operator new, the Vulkan driver) to
 * the heap:
 *
 *   --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free
 *   --wrap=posix_memalign --wrap=aligned_alloc --wrap=memalign
 *   --wrap=malloc_usable_size --wrap=reallocf --wrap=reallocarray
 *   --wrap=getline --wrap=getdelim
 *
 * The last two matter because libc's own getline and getdelim reallocate the
 * caller's buffer with libc's allocator. Pointers libc allocated itself
 * (strdup, asprintf, realpath, scandir, ...) stay libc's: free and realloc
 * send them back to it. When direct memory is exhausted, or the range cannot
 * be reserved, allocations fall back to libc.
 */
#ifndef PS5PLATFORM_HEAP_H
#define PS5PLATFORM_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where the heap's range is asked for, and how large it is: the whole direct
 * pool and room for its segments to move. From 0x10_0000_0000, 16 GiB is
 * reserved at the hint itself (docs/PROBE.md); guest-memory arenas take
 * 0x10_0000_0000 first, and the Vulkan driver's device memory starts at
 * 0x40_0000_0000. */
#define PS5_HEAP_HINT ((uintptr_t)0x2000000000ull)
#define PS5_HEAP_RANGE ((size_t)16 << 30)

struct ps5_heap_stats {
   /* The reserved range; zero before the first allocation or when it was
    * refused. */
   uintptr_t range_base;
   size_t range_bytes;
   /* Direct memory mapped for the heap now, and the most it has held. */
   size_t mapped_bytes;
   size_t peak_bytes;
   unsigned segments;
   /* Allocations the heap could not serve and libc did. */
   unsigned long long libc_fallbacks;
};

void ps5_heap_stats(struct ps5_heap_stats *stats);

/* Whether a pointer is the heap's. */
bool ps5_heap_owns(const void *pointer);

#ifdef __cplusplus
}
#endif

#endif /* PS5PLATFORM_HEAP_H */
