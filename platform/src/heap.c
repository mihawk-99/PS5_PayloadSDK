/*
 * PS5 Platform - the title heap (include/ps5platform/heap.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One reserved range holds every segment dlmalloc maps (src/dlmalloc/
 * mspace.c). A segment is direct memory allocated for it and mapped at a
 * fixed address in the range, first fit; when dlmalloc returns one, it is
 * unmapped, the range is reserved again under it, and the direct memory is
 * released. dlmalloc joins a segment to the one before it when the two are
 * adjacent, so a return can span several of the segments recorded here.
 *
 * The heap is several dlmalloc mspaces, its arenas, which map their
 * segments from the one range. A thread allocates from the arena it was
 * given at its first allocation, round robin, so threads allocating at once
 * wait on each other only when they share one: with one lock over the heap,
 * RADV's shader compiles took twice as long on eight threads (measured on the
 * host, 2026-09-28), and a title compiles pipelines on several. Each block
 * records its arena (dlmalloc's FOOTERS), so a free or realloc from any
 * thread goes to the arena the block came from. Arenas map and return
 * segments under a lock of the table's own.
 */
#include "ps5platform/heap.h"

#include "placement.h"
#include "ps5platform/kernel.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

/* src/dlmalloc/mspace.c */
typedef void *mspace;
mspace create_mspace(size_t capacity, int locked);
void *mspace_malloc(mspace space, size_t bytes);
void *mspace_calloc(mspace space, size_t count, size_t bytes);
void *mspace_realloc(mspace space, void *pointer, size_t bytes);
void *mspace_memalign(mspace space, size_t alignment, size_t bytes);
size_t mspace_usable_size(const void *pointer);
void mspace_free(mspace space, void *pointer);


#define HEAP_MAX_SEGMENTS 1024
#define HEAP_UNIT PS5P_DIRECT_UNIT
#define HEAP_PROTECTION (PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE)

struct heap_segment {
   size_t offset;
   size_t bytes;
   int64_t direct_start;
};

/* Sorted by offset. */
static struct heap_segment segments[HEAP_MAX_SEGMENTS];
static unsigned segment_count;
static size_t mapped_bytes, peak_bytes;
static unsigned long long libc_fallbacks;

static uintptr_t range_base;
static size_t range_bytes;

#define HEAP_ARENAS 8

/* 0 none, 1 in progress, 2 ready, 3 refused. */
static int range_state;
static mspace arenas[HEAP_ARENAS];
static int arena_states[HEAP_ARENAS];
static unsigned arena_next;
static pthread_key_t arena_key;
static pthread_once_t arena_key_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t segment_lock = PTHREAD_MUTEX_INITIALIZER;

static bool
reserve_range(void)
{
   /* The whole pool first; a console with less space free gets less. */
   for (size_t bytes = PS5_HEAP_RANGE; bytes >= ((size_t)1 << 30); bytes /= 2) {
      void *address = NULL;
      if (ps5p_reserve_placed(bytes, PS5_HEAP_HINT, HEAP_UNIT, -1, &address) == 0) {
         range_base = (uintptr_t)address;
         range_bytes = bytes;
         return true;
      }
   }
   return false;
}

static void *
heap_map_locked(size_t bytes)
{
   bytes = ps5p_round_up(bytes, HEAP_UNIT);
   if (bytes == 0 || segment_count >= HEAP_MAX_SEGMENTS)
      return (void *)-1;

   unsigned slot = 0;
   size_t offset = 0;
   for (; slot < segment_count; slot++) {
      if (segments[slot].offset - offset >= bytes)
         break;
      offset = segments[slot].offset + segments[slot].bytes;
   }
   if (offset > range_bytes || range_bytes - offset < bytes)
      return (void *)-1;

   int64_t start = -1;
   if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, HEAP_UNIT,
                                     PS5_KERNEL_DIRECT_TYPE_CPU, &start) != 0)
      return (void *)-1;
   void *const at = (void *)(range_base + offset);
   void *mapped = at;
   if (sceKernelMapDirectMemory(&mapped, bytes, HEAP_PROTECTION, PS5_KERNEL_MAP_FIXED, start,
                                HEAP_UNIT) != 0 ||
       mapped != at) {
      sceKernelReleaseDirectMemory(start, bytes);
      return (void *)-1;
   }

   memmove(&segments[slot + 1], &segments[slot],
           (segment_count - slot) * sizeof(segments[0]));
   segments[slot] = (struct heap_segment){.offset = offset, .bytes = bytes, .direct_start = start};
   segment_count++;
   mapped_bytes += bytes;
   if (mapped_bytes > peak_bytes)
      peak_bytes = mapped_bytes;
   return at;
}

void *
ps5p_heap_map(size_t bytes)
{
   pthread_mutex_lock(&segment_lock);
   void *const at = heap_map_locked(bytes);
   pthread_mutex_unlock(&segment_lock);
   return at;
}

static int
heap_unmap_locked(void *address, size_t bytes)
{
   if ((uintptr_t)address < range_base)
      return -1;
   const size_t offset = (uintptr_t)address - range_base;
   unsigned first = 0;
   while (first < segment_count && segments[first].offset < offset)
      first++;
   if (first == segment_count || segments[first].offset != offset)
      return -1;
   /* The segments must tile the span exactly: dlmalloc returns whole
    * segments, joined or not, and treats a refusal as nothing returned. */
   unsigned last = first;
   size_t covered = 0;
   while (last < segment_count && covered < bytes &&
          segments[last].offset == offset + covered) {
      covered += segments[last].bytes;
      last++;
   }
   if (covered != bytes)
      return -1;

   for (unsigned i = first; i < last; i++) {
      void *at = (void *)(range_base + segments[i].offset);
      sceKernelMunmap(at, segments[i].bytes);
      sceKernelReserveVirtualRange(&at, segments[i].bytes, PS5_KERNEL_MAP_FIXED, PS5P_PAGE);
      sceKernelReleaseDirectMemory(segments[i].direct_start, segments[i].bytes);
      mapped_bytes -= segments[i].bytes;
   }
   memmove(&segments[first], &segments[last], (segment_count - last) * sizeof(segments[0]));
   segment_count -= last - first;
   return 0;
}

int
ps5p_heap_unmap(void *address, size_t bytes)
{
   pthread_mutex_lock(&segment_lock);
   const int result = heap_unmap_locked(address, bytes);
   pthread_mutex_unlock(&segment_lock);
   return result;
}

/* Once, by whichever thread asks first; the others wait for it. */
static bool
once(int *state, bool (*make)(void *), void *context)
{
   int seen = __atomic_load_n(state, __ATOMIC_ACQUIRE);
   if (seen == 0 &&
       __atomic_compare_exchange_n(state, &seen, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      __atomic_store_n(state, make(context) ? 2 : 3, __ATOMIC_RELEASE);
   while ((seen = __atomic_load_n(state, __ATOMIC_ACQUIRE)) == 1)
      ;
   return seen == 2;
}

static bool
make_range(void *unused)
{
   (void)unused;
   return reserve_range();
}

static bool
make_arena(void *slot)
{
   mspace *const arena = slot;
   *arena = create_mspace(0, 1);
   return *arena != NULL;
}

static mspace
arena(unsigned index)
{
   return once(&arena_states[index], make_arena, &arenas[index]) ? arenas[index] : NULL;
}

static void
make_arena_key(void)
{
   pthread_key_create(&arena_key, NULL);
}

/* The calling thread's arena: given at its first allocation (the key's
 * values are the arena's index plus one), made when first given. An arena
 * that cannot be made leaves its threads the first one. */
static mspace
title_heap(void)
{
   if (!once(&range_state, make_range, NULL))
      return NULL;
   pthread_once(&arena_key_once, make_arena_key);
   uintptr_t slot = (uintptr_t)pthread_getspecific(arena_key);
   if (slot == 0) {
      slot = __atomic_fetch_add(&arena_next, 1, __ATOMIC_RELAXED) % HEAP_ARENAS + 1;
      pthread_setspecific(arena_key, (void *)slot);
   }
   const mspace space = arena((unsigned)slot - 1);
   return space || slot == 1 ? space : arena(0);
}

/* A block's own arena frees and resizes it, whichever is named (FOOTERS). */
static mspace
any_arena(void)
{
   return arenas[0];
}

bool
ps5_heap_owns(const void *pointer)
{
   if (__atomic_load_n(&range_state, __ATOMIC_ACQUIRE) != 2)
      return false;
   return (uintptr_t)pointer - range_base < range_bytes;
}

void
ps5_heap_stats(struct ps5_heap_stats *stats)
{
   const bool ready = __atomic_load_n(&range_state, __ATOMIC_ACQUIRE) == 2;
   stats->range_base = ready ? range_base : 0;
   stats->range_bytes = ready ? range_bytes : 0;
   /* Read without the segment lock: a report, not a decision. */
   stats->mapped_bytes = mapped_bytes;
   stats->peak_bytes = peak_bytes;
   stats->segments = segment_count;
   stats->arenas = 0;
   for (unsigned i = 0; i < HEAP_ARENAS; i++)
      stats->arenas += __atomic_load_n(&arena_states[i], __ATOMIC_ACQUIRE) == 2;
   stats->libc_fallbacks = __atomic_load_n(&libc_fallbacks, __ATOMIC_RELAXED);
}

void
ps5p_heap_count_fallback(void)
{
   __atomic_fetch_add(&libc_fallbacks, 1, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------ the heap */

void *
ps5_heap_malloc(size_t bytes)
{
   const mspace space = title_heap();
   return space ? mspace_malloc(space, bytes) : NULL;
}

void *
ps5_heap_calloc(size_t count, size_t bytes)
{
   const mspace space = title_heap();
   return space ? mspace_calloc(space, count, bytes) : NULL;
}

void *
ps5_heap_memalign(size_t alignment, size_t bytes)
{
   const mspace space = title_heap();
   return space ? mspace_memalign(space, alignment, bytes) : NULL;
}

void *
ps5_heap_realloc(void *pointer, size_t bytes)
{
   return pointer ? mspace_realloc(any_arena(), pointer, bytes) : ps5_heap_malloc(bytes);
}

void
ps5_heap_free(void *pointer)
{
   if (pointer)
      mspace_free(any_arena(), pointer);
}

size_t
ps5_heap_usable_size(const void *pointer)
{
   return pointer ? mspace_usable_size(pointer) : 0;
}

