/*
 * PS5 Platform - executable code in direct memory (include/ps5platform/exec.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A region is one direct-memory allocation, mapped read-write at its place
 * and then given execute (execute at map time is refused), and for
 * PS5_EXEC_DUAL_VIEW mapped a second time, read-write, as the view code is
 * written through. Every step that fails undoes the ones before it.
 */
#include "ps5platform/exec.h"

#include "placement.h"
#include "ps5platform/kernel.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#define PROT_RW (PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE)
#define PROT_RX (PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_EXEC)
#define PROT_RWX (PROT_RW | PS5_KERNEL_PROT_CPU_EXEC)

static atomic_uint_fast64_t live_regions;
static atomic_uint_fast64_t live_bytes;

/* Maps the allocation where the request says, read-write. */
static int
place(const struct ps5_exec_request *request, int64_t start, size_t bytes, void **base)
{
   if (request->flags & PS5_EXEC_FIXED) {
      void *at = (void *)request->address;
      const int32_t result =
         sceKernelMapDirectMemory(&at, bytes, PROT_RW, PS5_KERNEL_MAP_FIXED, start, PS5P_DIRECT_UNIT);
      if (result != 0)
         return result;
      if ((uintptr_t)at != request->address) {
         sceKernelMunmap(at, bytes);
         return PS5_EXEC_NO_PLACE;
      }
      *base = at;
      return 0;
   }
   if (request->flags & PS5_EXEC_AT) {
      /* A reservation made where it is asked for only if the range is free:
       * the kernel moves a hint it cannot place, and a moved one is refused. */
      void *reserved = (void *)request->address;
      int32_t result = sceKernelReserveVirtualRange(&reserved, bytes, 0, PS5P_DIRECT_UNIT);
      if (result != 0)
         return result;
      if ((uintptr_t)reserved != request->address) {
         sceKernelMunmap(reserved, bytes);
         return PS5_EXEC_NO_PLACE;
      }
      void *at = reserved;
      result =
         sceKernelMapDirectMemory(&at, bytes, PROT_RW, PS5_KERNEL_MAP_FIXED, start, PS5P_DIRECT_UNIT);
      if (result != 0 || at != reserved) {
         if (result == 0)
            sceKernelMunmap(at, bytes);
         sceKernelMunmap(reserved, bytes);
         return result != 0 ? result : PS5_EXEC_NO_PLACE;
      }
      *base = at;
      return 0;
   }
   if (request->flags & PS5_EXEC_NEAR) {
      void *reserved = NULL;
      const int result =
         ps5p_reserve_near(bytes, request->anchor, request->address, PS5_EXEC_NO_PLACE, &reserved);
      if (result != 0)
         return result;
      void *at = reserved;
      const int32_t mapped =
         sceKernelMapDirectMemory(&at, bytes, PROT_RW, PS5_KERNEL_MAP_FIXED, start, PS5P_DIRECT_UNIT);
      if (mapped != 0 || at != reserved) {
         if (mapped == 0)
            sceKernelMunmap(at, bytes);
         sceKernelMunmap(reserved, bytes);
         return mapped != 0 ? mapped : PS5_EXEC_NO_PLACE;
      }
      *base = at;
      return 0;
   }
   return ps5p_map_placed(start, bytes, request->address, PROT_RW, PS5_EXEC_NO_PLACE, base);
}

int
ps5_exec_alloc(const struct ps5_exec_request *request, struct ps5_exec_region *region)
{
   if (region)
      memset(region, 0, sizeof(*region));
   const unsigned placement = request ? request->flags & (PS5_EXEC_NEAR | PS5_EXEC_FIXED | PS5_EXEC_AT)
                                      : 0;
   const bool exact = placement & (PS5_EXEC_FIXED | PS5_EXEC_AT);
   if (!request || !region || request->bytes == 0 || (placement & (placement - 1)) != 0 ||
       ((request->flags & PS5_EXEC_DUAL_VIEW) && (request->flags & PS5_EXEC_TOGGLED)) ||
       (exact && (request->address == 0 || request->address % PS5P_DIRECT_UNIT != 0)))
      return PS5_EXEC_BAD_REQUEST;
   const size_t bytes = ps5p_round_up(request->bytes, PS5P_DIRECT_UNIT);
   /* An exact address in the GPU window is never one to give out. */
   if (exact && !ps5p_outside_gpu_window(request->address, bytes))
      return PS5_EXEC_NO_PLACE;
   int64_t start = -1;
   int32_t result = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes,
                                                  PS5P_DIRECT_UNIT, PS5_KERNEL_DIRECT_TYPE_CPU, &start);
   if (result != 0)
      return result;
   void *base = NULL;
   result = place(request, start, bytes, &base);
   if (result != 0) {
      sceKernelReleaseDirectMemory(start, bytes);
      return result;
   }
   const int protection = (request->flags & PS5_EXEC_TOGGLED)     ? PROT_RW
                          : (request->flags & PS5_EXEC_DUAL_VIEW) ? PROT_RX
                                                                  : PROT_RWX;
   if (protection != PROT_RW) {
      result = sceKernelMprotect(base, bytes, protection);
      if (result != 0) {
         sceKernelMunmap(base, bytes);
         sceKernelReleaseDirectMemory(start, bytes);
         return result;
      }
   }
   void *write_view = base;
   if (request->flags & PS5_EXEC_DUAL_VIEW) {
      result = ps5p_map_placed(start, bytes, 0, PROT_RW, PS5_EXEC_NO_PLACE, &write_view);
      if (result != 0) {
         sceKernelMunmap(base, bytes);
         sceKernelReleaseDirectMemory(start, bytes);
         return result;
      }
   }
   region->base = base;
   region->write_view = write_view;
   region->bytes = bytes;
   region->direct_start = start;
   region->flags = request->flags;
   atomic_fetch_add(&live_regions, 1);
   atomic_fetch_add(&live_bytes, bytes);
   return 0;
}

int
ps5_exec_protect(const struct ps5_exec_region *region, size_t offset, size_t bytes, bool writable)
{
   if (!region || !(region->flags & PS5_EXEC_TOGGLED) || offset >= region->bytes || bytes == 0)
      return PS5_EXEC_BAD_REQUEST;
   const size_t first = offset / PS5P_PAGE * PS5P_PAGE;
   size_t end = ps5p_round_up(offset + bytes, PS5P_PAGE);
   if (end > region->bytes)
      end = region->bytes;
   return sceKernelMprotect((const char *)region->base + first, end - first,
                            writable ? PROT_RW : PROT_RX);
}

void
ps5_exec_free(struct ps5_exec_region *region)
{
   if (!region || region->bytes == 0)
      return;
   if (region->write_view && region->write_view != region->base)
      sceKernelMunmap(region->write_view, region->bytes);
   sceKernelMunmap(region->base, region->bytes);
   sceKernelReleaseDirectMemory(region->direct_start, region->bytes);
   atomic_fetch_sub(&live_regions, 1);
   atomic_fetch_sub(&live_bytes, region->bytes);
   memset(region, 0, sizeof(*region));
}

void
ps5_exec_live(uint64_t *regions, uint64_t *bytes)
{
   if (regions)
      *regions = atomic_load(&live_regions);
   if (bytes)
      *bytes = atomic_load(&live_bytes);
}

/* ---- the pointer-only form -------------------------------------------------- */

/* The regions ps5_exec_allocate handed out: a JIT holds a few (Dolphin's near
 * and far caches and its trampolines, LRPS2's recompilers' areas). */
#define KEPT_REGIONS 128
static struct ps5_exec_region kept[KEPT_REGIONS];
static pthread_mutex_t kept_lock = PTHREAD_MUTEX_INITIALIZER;

void *
ps5_exec_allocate(size_t bytes, uintptr_t anchor)
{
   pthread_mutex_lock(&kept_lock);
   struct ps5_exec_region *slot = NULL;
   for (unsigned i = 0; i < KEPT_REGIONS && !slot; i++)
      if (kept[i].bytes == 0)
         slot = &kept[i];
   void *base = NULL;
   if (slot) {
      const struct ps5_exec_request request = {
         .bytes = bytes,
         .anchor = anchor,
         .flags = anchor != 0 ? PS5_EXEC_NEAR : 0,
      };
      if (ps5_exec_alloc(&request, slot) == 0)
         base = slot->base;
   }
   pthread_mutex_unlock(&kept_lock);
   return base;
}

int
ps5_exec_release(void *base)
{
   if (!base)
      return PS5_EXEC_BAD_REQUEST;
   pthread_mutex_lock(&kept_lock);
   int result = PS5_EXEC_BAD_REQUEST;
   for (unsigned i = 0; i < KEPT_REGIONS; i++)
      if (kept[i].bytes != 0 && kept[i].base == base) {
         ps5_exec_free(&kept[i]);
         result = 0;
         break;
      }
   pthread_mutex_unlock(&kept_lock);
   return result;
}
