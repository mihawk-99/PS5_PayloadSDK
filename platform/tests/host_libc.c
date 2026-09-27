/*
 * PS5 Platform - libc's allocator as the heap's wraps reach it (src/heap.c).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A title links the heap with --wrap, which binds __real_malloc and the rest
 * to the console's libc. The host tests call the wraps directly, so these
 * bind the same names to the host's libc.
 */
#define _GNU_SOURCE 1

#include <malloc.h>
#include <locale.h>
#include <stdlib.h>

void *__real_malloc(size_t bytes);
void *__real_calloc(size_t count, size_t bytes);
void *__real_realloc(void *pointer, size_t bytes);
void __real_free(void *pointer);
int __real_posix_memalign(void **out, size_t alignment, size_t bytes);
size_t __real_malloc_usable_size(const void *pointer);

void *__real_malloc(size_t bytes) { return malloc(bytes); }
void *__real_calloc(size_t count, size_t bytes) { return calloc(count, bytes); }
void *__real_realloc(void *pointer, size_t bytes) { return realloc(pointer, bytes); }
void __real_free(void *pointer) { free(pointer); }
int __real_posix_memalign(void **out, size_t alignment, size_t bytes)
{
   return posix_memalign(out, alignment, bytes);
}
size_t __real_malloc_usable_size(const void *pointer) { return malloc_usable_size((void *)pointer); }

/* localeconv() as the console's: its decimal point reads empty while strtod
 * reads '.' (host_empty_decimal_point set), else the host's. */
struct lconv *__real_localeconv(void);
int host_empty_decimal_point;

struct lconv *
__wrap_localeconv(void)
{
   static struct lconv console;
   struct lconv *const host = __real_localeconv();
   if (!host_empty_decimal_point)
      return host;
   console = *host;
   console.decimal_point = (char *)"";
   console.thousands_sep = (char *)"";
   return &console;
}
