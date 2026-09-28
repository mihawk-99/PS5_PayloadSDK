/*
 * PS5 Platform - the title heap's wraps of libc's allocator
 * (include/ps5platform/heap.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Apart from src/heap.c, so that a title with an allocator of its own can use
 * the heap (ps5_heap_malloc and the rest) without linking these.
 */
#include "ps5platform/heap.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

void ps5p_heap_count_fallback(void);

/* libc's allocator, through the linker's --wrap. malloc_usable_size is weak:
 * a title that does not reference it links without its wrap. */
void *__real_malloc(size_t bytes);
void *__real_calloc(size_t count, size_t bytes);
void *__real_realloc(void *pointer, size_t bytes);
void __real_free(void *pointer);
int __real_posix_memalign(void **out, size_t alignment, size_t bytes);
__attribute__((weak)) size_t __real_malloc_usable_size(const void *pointer);

void *
__wrap_malloc(size_t bytes)
{
   void *const pointer = ps5_heap_malloc(bytes);
   if (pointer)
      return pointer;
   ps5p_heap_count_fallback();
   return __real_malloc(bytes);
}

void *
__wrap_calloc(size_t count, size_t bytes)
{
   void *const pointer = ps5_heap_calloc(count, bytes);
   if (pointer)
      return pointer;
   ps5p_heap_count_fallback();
   return __real_calloc(count, bytes);
}

void
__wrap_free(void *pointer)
{
   if (!pointer)
      return;
   if (ps5_heap_owns(pointer))
      ps5_heap_free(pointer);
   else
      __real_free(pointer);
}

size_t
__wrap_malloc_usable_size(const void *pointer)
{
   if (!pointer)
      return 0;
   if (ps5_heap_owns(pointer))
      return ps5_heap_usable_size(pointer);
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
      void *const moved = ps5_heap_realloc(pointer, bytes);
      if (moved)
         return moved;
      /* Direct memory is exhausted: move the block to libc. */
      void *const copy = __real_malloc(bytes);
      if (!copy)
         return NULL;
      ps5p_heap_count_fallback();
      const size_t kept = ps5_heap_usable_size(pointer);
      memcpy(copy, pointer, kept < bytes ? kept : bytes);
      ps5_heap_free(pointer);
      return copy;
   }
   void *const moved = __real_realloc(pointer, bytes);
   if (moved || !__real_malloc_usable_size)
      return moved;
   /* libc's heap is exhausted: move its block to the title heap. */
   void *const copy = ps5_heap_malloc(bytes);
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
   void *const pointer = ps5_heap_memalign(alignment, bytes);
   if (pointer) {
      *out = pointer;
      return 0;
   }
   ps5p_heap_count_fallback();
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
