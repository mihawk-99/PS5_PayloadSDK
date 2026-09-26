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
 * Segments are mapped and returned only under the mspace's lock, or by the
 * one thread creating the mspace, so the table needs no lock of its own.
 */
#include "ps5platform/heap.h"

#include "placement.h"
#include "ps5platform/kernel.h"

#include <errno.h>
#include <limits.h>
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

/* libc's allocator, through the linker's --wrap. malloc_usable_size is weak:
 * a title that does not reference it links without its wrap. */
void *__real_malloc(size_t bytes);
void *__real_calloc(size_t count, size_t bytes);
void *__real_realloc(void *pointer, size_t bytes);
void __real_free(void *pointer);
int __real_posix_memalign(void **out, size_t alignment, size_t bytes);
__attribute__((weak)) size_t __real_malloc_usable_size(const void *pointer);

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

static mspace heap;
static int heap_state; /* 0 none, 1 creating, 2 ready, 3 refused */

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

void *
ps5p_heap_map(size_t bytes)
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

int
ps5p_heap_unmap(void *address, size_t bytes)
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

static mspace
title_heap(void)
{
   int state = __atomic_load_n(&heap_state, __ATOMIC_ACQUIRE);
   if (state == 2)
      return heap;
   if (state == 0 &&
       __atomic_compare_exchange_n(&heap_state, &state, 1, false, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE)) {
      heap = reserve_range() ? create_mspace(0, 1) : NULL;
      __atomic_store_n(&heap_state, heap ? 2 : 3, __ATOMIC_RELEASE);
   }
   while ((state = __atomic_load_n(&heap_state, __ATOMIC_ACQUIRE)) == 1)
      ;
   return state == 2 ? heap : NULL;
}

bool
ps5_heap_owns(const void *pointer)
{
   if (__atomic_load_n(&heap_state, __ATOMIC_ACQUIRE) != 2)
      return false;
   return (uintptr_t)pointer - range_base < range_bytes;
}

void
ps5_heap_stats(struct ps5_heap_stats *stats)
{
   const bool ready = __atomic_load_n(&heap_state, __ATOMIC_ACQUIRE) == 2;
   stats->range_base = ready ? range_base : 0;
   stats->range_bytes = ready ? range_bytes : 0;
   /* Read without the mspace's lock: a report, not a decision. */
   stats->mapped_bytes = mapped_bytes;
   stats->peak_bytes = peak_bytes;
   stats->segments = segment_count;
   stats->libc_fallbacks = __atomic_load_n(&libc_fallbacks, __ATOMIC_RELAXED);
}

static void
count_fallback(void)
{
   __atomic_fetch_add(&libc_fallbacks, 1, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------- the wraps */

void *
__wrap_malloc(size_t bytes)
{
   const mspace space = title_heap();
   void *const pointer = space ? mspace_malloc(space, bytes) : NULL;
   if (pointer)
      return pointer;
   count_fallback();
   return __real_malloc(bytes);
}

void *
__wrap_calloc(size_t count, size_t bytes)
{
   const mspace space = title_heap();
   void *const pointer = space ? mspace_calloc(space, count, bytes) : NULL;
   if (pointer)
      return pointer;
   count_fallback();
   return __real_calloc(count, bytes);
}

void
__wrap_free(void *pointer)
{
   if (!pointer)
      return;
   if (ps5_heap_owns(pointer))
      mspace_free(heap, pointer);
   else
      __real_free(pointer);
}

size_t
__wrap_malloc_usable_size(const void *pointer)
{
   if (!pointer)
      return 0;
   if (ps5_heap_owns(pointer))
      return mspace_usable_size(pointer);
   return __real_malloc_usable_size ? __real_malloc_usable_size(pointer) : 0;
}

void *
__wrap_realloc(void *pointer, size_t bytes)
{
   if (!pointer)
      return __wrap_malloc(bytes);
   /* FreeBSD's realloc gives a zero-size request a minimum-size object. */
   if (bytes == 0)
      bytes = 1;
   if (ps5_heap_owns(pointer)) {
      void *const moved = mspace_realloc(heap, pointer, bytes);
      if (moved)
         return moved;
      /* Direct memory is exhausted: move the block to libc. */
      void *const copy = __real_malloc(bytes);
      if (!copy)
         return NULL;
      count_fallback();
      const size_t kept = mspace_usable_size(pointer);
      memcpy(copy, pointer, kept < bytes ? kept : bytes);
      mspace_free(heap, pointer);
      return copy;
   }
   void *const moved = __real_realloc(pointer, bytes);
   if (moved || !__real_malloc_usable_size)
      return moved;
   /* libc's heap is exhausted: move its block to the title heap. */
   const mspace space = title_heap();
   void *const copy = space ? mspace_malloc(space, bytes) : NULL;
   if (!copy)
      return NULL;
   const size_t kept = __real_malloc_usable_size(pointer);
   memcpy(copy, pointer, kept < bytes ? kept : bytes);
   __real_free(pointer);
   return copy;
}

void *
__wrap_reallocf(void *pointer, size_t bytes)
{
   void *const moved = __wrap_realloc(pointer, bytes);
   if (!moved)
      __wrap_free(pointer);
   return moved;
}

void *
__wrap_reallocarray(void *pointer, size_t count, size_t bytes)
{
   if (bytes != 0 && count > SIZE_MAX / bytes) {
      errno = ENOMEM;
      return NULL;
   }
   return __wrap_realloc(pointer, count * bytes);
}

int
__wrap_posix_memalign(void **out, size_t alignment, size_t bytes)
{
   if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
      return EINVAL;
   const mspace space = title_heap();
   void *const pointer = space ? mspace_memalign(space, alignment, bytes) : NULL;
   if (pointer) {
      *out = pointer;
      return 0;
   }
   count_fallback();
   return __real_posix_memalign(out, alignment, bytes);
}

void *
__wrap_aligned_alloc(size_t alignment, size_t bytes)
{
   void *pointer = NULL;
   const int result =
      __wrap_posix_memalign(&pointer, alignment < sizeof(void *) ? sizeof(void *) : alignment, bytes);
   if (result != 0) {
      errno = result;
      return NULL;
   }
   return pointer;
}

void *
__wrap_memalign(size_t alignment, size_t bytes)
{
   return __wrap_aligned_alloc(alignment, bytes);
}

/* libc's getdelim grows the caller's buffer with libc's realloc, which must
 * never see a title-heap block: this one grows it with the wrap. */
ssize_t
__wrap_getdelim(char **line, size_t *capacity, int delimiter, FILE *stream)
{
   if (!line || !capacity || !stream) {
      errno = EINVAL;
      return -1;
   }
   if (!*line)
      *capacity = 0;
   size_t length = 0;
   for (;;) {
      const int c = fgetc(stream);
      if (c == EOF) {
         if (length == 0 || ferror(stream))
            return -1;
         break;
      }
      if (length + 2 > *capacity) {
         size_t grown = *capacity < 64 ? 128 : *capacity * 2;
         if (grown > (size_t)SSIZE_MAX) {
            errno = EOVERFLOW;
            return -1;
         }
         char *const moved = __wrap_realloc(*line, grown);
         if (!moved)
            return -1;
         *line = moved;
         *capacity = grown;
      }
      (*line)[length++] = (char)c;
      if (c == (unsigned char)delimiter)
         break;
   }
   (*line)[length] = '\0';
   return (ssize_t)length;
}

ssize_t
__wrap_getline(char **line, size_t *capacity, FILE *stream)
{
   return __wrap_getdelim(line, capacity, '\n', stream);
}
