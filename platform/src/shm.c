/*
 * PS5 Platform - shared-memory objects on direct memory, and virtual ranges
 * (include/ps5platform/shm.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An object is a direct-memory allocation; a view maps part of it, at a hint
 * or at a fixed address inside a reservation. Unmapping a view inside an
 * arena gives the range back to the kernel for a moment, so it is reserved
 * again at once (PS5_SHM_KEEP_RESERVED), as a POSIX arena overlays a
 * PROT_NONE mapping.
 */
#include "ps5platform/shm.h"

#include "placement.h"
#include "ps5platform/kernel.h"

#include <stdatomic.h>
#include <string.h>

static atomic_uint_fast64_t objects, object_bytes, views, view_bytes, ranges, range_bytes;

int
ps5_shm_create(size_t bytes, struct ps5_shm *shm)
{
   if (shm)
      memset(shm, 0, sizeof(*shm));
   if (!shm || bytes == 0)
      return PS5_SHM_BAD_REQUEST;
   const size_t rounded = ps5p_round_up(bytes, PS5P_DIRECT_UNIT);
   int64_t start = -1;
   const int32_t result = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), rounded,
                                                        PS5P_DIRECT_UNIT, PS5_KERNEL_DIRECT_TYPE_CPU,
                                                        &start);
   if (result != 0)
      return result;
   shm->direct_start = start;
   shm->bytes = rounded;
   atomic_fetch_add(&objects, 1);
   atomic_fetch_add(&object_bytes, rounded);
   return 0;
}

void
ps5_shm_destroy(struct ps5_shm *shm)
{
   if (!shm || shm->bytes == 0)
      return;
   sceKernelReleaseDirectMemory(shm->direct_start, shm->bytes);
   atomic_fetch_sub(&objects, 1);
   atomic_fetch_sub(&object_bytes, shm->bytes);
   memset(shm, 0, sizeof(*shm));
}

static int
kernel_protection(int protection)
{
   return (protection & PS5_SHM_READ ? PS5_KERNEL_PROT_CPU_READ : 0) |
          (protection & PS5_SHM_WRITE ? PS5_KERNEL_PROT_CPU_WRITE : 0) |
          (protection & PS5_SHM_EXEC ? PS5_KERNEL_PROT_CPU_EXEC : 0);
}

int
ps5_shm_map(const struct ps5_shm *shm, size_t offset, size_t bytes, void *address, int protection,
            unsigned flags, void **view)
{
   if (view)
      *view = NULL;
   if (!shm || !view || bytes == 0 || offset % PS5P_DIRECT_UNIT != 0 ||
       bytes % PS5P_PAGE != 0 || offset + bytes > shm->bytes ||
       ((flags & PS5_SHM_FIXED) && (uintptr_t)address % PS5P_PAGE != 0))
      return PS5_SHM_BAD_REQUEST;
   /* Mapped without execute; execute, if asked for, comes after. */
   const int wanted = kernel_protection(protection);
   const int at_map = wanted & ~PS5_KERNEL_PROT_CPU_EXEC;
   void *mapped = NULL;
   int32_t result;
   if (flags & PS5_SHM_FIXED) {
      mapped = address;
      result = sceKernelMapDirectMemory(&mapped, bytes, at_map, PS5_KERNEL_MAP_FIXED,
                                        shm->direct_start + (int64_t)offset, PS5P_PAGE);
      if (result == 0 && mapped != address) {
         sceKernelMunmap(mapped, bytes);
         result = PS5_SHM_NO_PLACE;
      }
   } else {
      result = ps5p_map_placed(shm->direct_start + (int64_t)offset, bytes, (uintptr_t)address,
                               at_map, PS5_SHM_NO_PLACE, &mapped);
   }
   if (result != 0)
      return result;
   if (wanted != at_map) {
      result = sceKernelMprotect(mapped, bytes, wanted);
      if (result != 0) {
         sceKernelMunmap(mapped, bytes);
         return result;
      }
   }
   *view = mapped;
   atomic_fetch_add(&views, 1);
   atomic_fetch_add(&view_bytes, bytes);
   return 0;
}

int
ps5_shm_unmap(void *view, size_t bytes, unsigned flags)
{
   if (!view || bytes == 0)
      return PS5_SHM_BAD_REQUEST;
   int32_t result = sceKernelMunmap(view, bytes);
   if (result != 0)
      return result;
   atomic_fetch_sub(&views, 1);
   atomic_fetch_sub(&view_bytes, bytes);
   if (flags & PS5_SHM_KEEP_RESERVED) {
      void *at = view;
      result = sceKernelReserveVirtualRange(&at, bytes, PS5_KERNEL_MAP_FIXED, PS5P_PAGE);
      if (result == 0 && at != view) {
         sceKernelMunmap(at, bytes);
         result = PS5_SHM_NO_PLACE;
      }
   }
   return result;
}

int
ps5_vrange_reserve(size_t bytes, void *hint, size_t alignment, void **base)
{
   if (base)
      *base = NULL;
   if (!base || bytes == 0)
      return PS5_SHM_BAD_REQUEST;
   const size_t rounded = ps5p_round_up(bytes, PS5P_PAGE);
   const int result = ps5p_reserve_placed(rounded, (uintptr_t)hint,
                                          alignment ? alignment : PS5P_DIRECT_UNIT,
                                          PS5_SHM_NO_PLACE, base);
   if (result == 0) {
      atomic_fetch_add(&ranges, 1);
      atomic_fetch_add(&range_bytes, rounded);
   }
   return result;
}

int
ps5_vrange_release(void *base, size_t bytes)
{
   if (!base || bytes == 0)
      return PS5_SHM_BAD_REQUEST;
   const size_t rounded = ps5p_round_up(bytes, PS5P_PAGE);
   const int32_t result = sceKernelMunmap(base, rounded);
   if (result == 0) {
      atomic_fetch_sub(&ranges, 1);
      atomic_fetch_sub(&range_bytes, rounded);
   }
   return result;
}

void
ps5_shm_live(struct ps5_shm_stats *stats)
{
   if (!stats)
      return;
   stats->objects = atomic_load(&objects);
   stats->object_bytes = atomic_load(&object_bytes);
   stats->views = atomic_load(&views);
   stats->view_bytes = atomic_load(&view_bytes);
   stats->ranges = atomic_load(&ranges);
   stats->range_bytes = atomic_load(&range_bytes);
}
