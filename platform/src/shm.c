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

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static atomic_uint_fast64_t objects, object_bytes, views, view_bytes, ranges, range_bytes,
   committed_bytes;

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
   /* The object is allocated in 64 KiB units; a view of it needs only the
    * kernel's 16 KiB page (PPSSPP maps VRAM at an offset of 80 KiB). */
   if (!shm || !view || bytes == 0 || offset % PS5P_PAGE != 0 ||
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
ps5_vrange_reserve_at(void *address, size_t bytes)
{
   const uintptr_t at = (uintptr_t)address;
   if (!address || bytes == 0 || at % PS5P_DIRECT_UNIT != 0)
      return PS5_SHM_BAD_REQUEST;
   const size_t rounded = ps5p_round_up(bytes, PS5P_PAGE);
   if (!ps5p_outside_gpu_window(at, rounded))
      return PS5_SHM_NO_PLACE;
   void *placed = address;
   const int32_t result = sceKernelReserveVirtualRange(&placed, rounded, 0, PS5P_DIRECT_UNIT);
   if (result != 0)
      return result;
   if (placed != address) {
      sceKernelMunmap(placed, rounded);
      return PS5_SHM_NO_PLACE;
   }
   atomic_fetch_add(&ranges, 1);
   atomic_fetch_add(&range_bytes, rounded);
   return 0;
}

/* ---- committed units ------------------------------------------------------------
 * Each backed 64 KiB unit, by address, in an open-addressed table: its direct
 * memory, and the protection its last commit gave it. */
struct unit {
   uintptr_t address; /* UNIT_EMPTY, UNIT_REMOVED, or the unit's address */
   int64_t direct_start;
   int protection;
};
#define UNIT_EMPTY ((uintptr_t)0)
#define UNIT_REMOVED ((uintptr_t)1) /* a unit's address is a multiple of 64 KiB */

static pthread_mutex_t units_lock = PTHREAD_MUTEX_INITIALIZER;
static struct unit *units;
static size_t unit_capacity, unit_used; /* used: live entries and tombstones */

static size_t
unit_slot(uintptr_t address, size_t capacity)
{
   const uint64_t key = (uint64_t)(address >> 16) * 0x9e3779b97f4a7c15ull;
   return (size_t)(key ^ (key >> 29)) & (capacity - 1);
}

static struct unit *
unit_find(uintptr_t address)
{
   if (!units)
      return NULL;
   for (size_t slot = unit_slot(address, unit_capacity);; slot = (slot + 1) & (unit_capacity - 1)) {
      if (units[slot].address == address)
         return &units[slot];
      if (units[slot].address == UNIT_EMPTY)
         return NULL;
   }
}

/* Room for one more entry: at most half the table is used. */
static bool
unit_room(void)
{
   if (units && (unit_used + 1) * 2 <= unit_capacity)
      return true;
   size_t capacity = unit_capacity ? unit_capacity : 1024;
   size_t live = 0;
   for (size_t slot = 0; slot < unit_capacity; slot++)
      live += units[slot].address > UNIT_REMOVED;
   while ((live + 1) * 4 > capacity)
      capacity *= 2;
   struct unit *const table = calloc(capacity, sizeof(*table));
   if (!table)
      return false;
   for (size_t slot = 0; slot < unit_capacity; slot++) {
      if (units[slot].address <= UNIT_REMOVED)
         continue;
      size_t to = unit_slot(units[slot].address, capacity);
      while (table[to].address != UNIT_EMPTY)
         to = (to + 1) & (capacity - 1);
      table[to] = units[slot];
   }
   free(units);
   units = table;
   unit_capacity = capacity;
   unit_used = live;
   return true;
}

static void
unit_insert(uintptr_t address, int64_t direct_start, int protection)
{
   size_t slot = unit_slot(address, unit_capacity);
   while (units[slot].address > UNIT_REMOVED)
      slot = (slot + 1) & (unit_capacity - 1);
   unit_used += units[slot].address == UNIT_EMPTY;
   units[slot] = (struct unit){address, direct_start, protection};
}

/* Gives a unit's memory back and leaves its range reserved, or not. */
static int32_t
unit_release(struct unit *unit, bool keep_reserved)
{
   void *const at = (void *)unit->address;
   int32_t result = sceKernelMunmap(at, PS5P_DIRECT_UNIT);
   if (result != 0)
      return result;
   if (keep_reserved) {
      void *placed = at;
      result = sceKernelReserveVirtualRange(&placed, PS5P_DIRECT_UNIT, PS5_KERNEL_MAP_FIXED,
                                            PS5P_DIRECT_UNIT);
      if (result == 0 && placed != at) {
         sceKernelMunmap(placed, PS5P_DIRECT_UNIT);
         result = PS5_SHM_NO_PLACE;
      }
   }
   sceKernelReleaseDirectMemory(unit->direct_start, PS5P_DIRECT_UNIT);
   unit->address = UNIT_REMOVED;
   atomic_fetch_sub(&committed_bytes, PS5P_DIRECT_UNIT);
   return result;
}

int
ps5_vrange_commit(void *address, size_t bytes, int protection)
{
   if (!address || bytes == 0)
      return PS5_SHM_BAD_REQUEST;
   const uintptr_t begin = (uintptr_t)address, end = begin + bytes;
   const uintptr_t page_low = begin & ~(uintptr_t)(PS5P_PAGE - 1);
   const uintptr_t page_high = ps5p_round_up(end, PS5P_PAGE);
   const uintptr_t unit_low = begin & ~(uintptr_t)(PS5P_DIRECT_UNIT - 1);
   const uintptr_t unit_high = ps5p_round_up(end, PS5P_DIRECT_UNIT);
   /* Mapped without execute; execute, if asked for, comes with the protection
    * change after (execute at map time is refused). */
   const int wanted = kernel_protection(protection);
   const int at_map = PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE;
   int32_t result = 0;
   pthread_mutex_lock(&units_lock);
   for (uintptr_t at = unit_low; at < unit_high && result == 0; at += PS5P_DIRECT_UNIT) {
      struct unit *const unit = unit_find(at);
      if (unit) {
         unit->protection = wanted;
         continue;
      }
      if (!unit_room()) {
         result = PS5_SHM_NO_PLACE;
         break;
      }
      int64_t start = -1;
      result = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), PS5P_DIRECT_UNIT,
                                             PS5P_DIRECT_UNIT, PS5_KERNEL_DIRECT_TYPE_CPU, &start);
      if (result != 0)
         break;
      void *mapped = (void *)at;
      result = sceKernelMapDirectMemory(&mapped, PS5P_DIRECT_UNIT, at_map, PS5_KERNEL_MAP_FIXED,
                                        start, PS5P_PAGE);
      if (result == 0 && mapped != (void *)at) {
         sceKernelMunmap(mapped, PS5P_DIRECT_UNIT);
         result = PS5_SHM_NO_PLACE;
      }
      if (result != 0) {
         sceKernelReleaseDirectMemory(start, PS5P_DIRECT_UNIT);
         break;
      }
      unit_insert(at, start, wanted);
      atomic_fetch_add(&committed_bytes, PS5P_DIRECT_UNIT);
      /* The unit's pages outside the range stay without access. */
      if (at < page_low)
         result = sceKernelMprotect(mapped, page_low - at, 0);
      if (result == 0 && at + PS5P_DIRECT_UNIT > page_high)
         result = sceKernelMprotect((void *)page_high, at + PS5P_DIRECT_UNIT - page_high, 0);
   }
   if (result == 0)
      result = sceKernelMprotect((void *)page_low, page_high - page_low, wanted);
   pthread_mutex_unlock(&units_lock);
   return result;
}

int
ps5_vrange_decommit(void *address, size_t bytes)
{
   if (!address || bytes == 0)
      return PS5_SHM_BAD_REQUEST;
   const uintptr_t begin = (uintptr_t)address, end = begin + bytes;
   const uintptr_t unit_low = begin & ~(uintptr_t)(PS5P_DIRECT_UNIT - 1);
   const uintptr_t unit_high = ps5p_round_up(end, PS5P_DIRECT_UNIT);
   int32_t result = 0;
   pthread_mutex_lock(&units_lock);
   for (uintptr_t at = unit_low; at < unit_high && result == 0; at += PS5P_DIRECT_UNIT) {
      struct unit *const unit = unit_find(at);
      if (!unit)
         continue;
      if (at >= begin && at + PS5P_DIRECT_UNIT <= end) {
         result = unit_release(unit, true);
         continue;
      }
      /* Partly inside: its part is zeroed, through pages made writable for
       * the moment. */
      const uintptr_t low = at > begin ? at : begin;
      const uintptr_t high = at + PS5P_DIRECT_UNIT < end ? at + PS5P_DIRECT_UNIT : end;
      const uintptr_t page_low = low & ~(uintptr_t)(PS5P_PAGE - 1);
      const uintptr_t page_high = ps5p_round_up(high, PS5P_PAGE);
      result = sceKernelMprotect((void *)page_low, page_high - page_low,
                                 PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE);
      if (result == 0) {
         memset((void *)low, 0, high - low);
         result = sceKernelMprotect((void *)page_low, page_high - page_low, unit->protection);
      }
   }
   pthread_mutex_unlock(&units_lock);
   return result;
}

int
ps5_vrange_release(void *base, size_t bytes)
{
   if (!base || bytes == 0)
      return PS5_SHM_BAD_REQUEST;
   const size_t rounded = ps5p_round_up(bytes, PS5P_PAGE);
   const uintptr_t low = (uintptr_t)base, high = low + rounded;
   pthread_mutex_lock(&units_lock);
   for (size_t slot = 0; slot < unit_capacity; slot++)
      if (units[slot].address > UNIT_REMOVED && units[slot].address >= low &&
          units[slot].address < high)
         unit_release(&units[slot], false);
   pthread_mutex_unlock(&units_lock);
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
   stats->committed_bytes = atomic_load(&committed_bytes);
}
