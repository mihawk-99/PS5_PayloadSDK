/*
 * PS5 Platform - dlmalloc 2.8.6 as the title heap's allocator (src/heap.c).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Built as imported, like src/regex/: malloc.c is Doug Lea's file unchanged
 * (SOURCE), configured here as mspaces only, with no sbrk, and with every
 * segment it maps coming from src/heap.c.
 */
#include <errno.h>
#include <stddef.h>

void *ps5p_heap_map(size_t bytes);
int ps5p_heap_unmap(void *address, size_t bytes);

#define ONLY_MSPACES 1
#define USE_LOCKS 1
/* Each block records its mspace, so a free or realloc from any thread goes to
 * the arena the block came from (src/heap.c). */
#define FOOTERS 1
#define HAVE_MORECORE 0
#define HAVE_MMAP 1
#define HAVE_MREMAP 0
#define MMAP(s) ps5p_heap_map(s)
#define DIRECT_MMAP(s) ps5p_heap_map(s)
#define MUNMAP(a, s) ps5p_heap_unmap((a), (s))
/* The PS5 target's __STDCPP_DEFAULT_NEW_ALIGNMENT__ and __BIGGEST_ALIGNMENT__
 * are 32: its compilers emit 32-byte-aligned AVX stores into plain new
 * objects. */
#define MALLOC_ALIGNMENT ((size_t)32)
/* Each arena maps this much at a time: small enough that arenas a title
 * barely uses hold little direct memory. */
#define DEFAULT_GRANULARITY ((size_t)16 << 20)
#define DEFAULT_MMAP_THRESHOLD ((size_t)32 << 20)
#define MALLOC_FAILURE_ACTION errno = ENOMEM
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
/* Direct memory's unit, so every size dlmalloc maps and unmaps is one the
 * kernel allocates as asked. */
#define malloc_getpagesize ((size_t)0x10000)
#include "malloc.c"
