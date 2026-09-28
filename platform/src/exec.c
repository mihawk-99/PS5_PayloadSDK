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
#include <stdlib.h>
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

/* What ps5_exec_allocate hands out has no fixed limit, as mmap has none. A
 * table of 128 regions ran out in Dolphin, which takes 4 KiB of code for each
 * vertex format a game draws with: Rogue Leader passed 127 regions between a
 * mission and the main menu, the next allocation failed, and Dolphin filled
 * the null block it got with breakpoints.
 *
 * A request of 64 KiB or more is a region of its own, kept in a list. A
 * smaller one is a block of whole 16 KiB pages in a shared 4 MiB arena within
 * reach of its anchor; as a region of its own it would take a 64 KiB
 * direct-memory unit and a kernel mapping. Whole pages keep a core's
 * protection changes to its own block (PPSSPP takes execute away from a block
 * before it frees it), a freed block is made read, write and execute again,
 * and a block is zeroed when it is handed out, as a fresh mapping is. An
 * arena goes back to the kernel when its last block is freed. */
#define ARENA_BYTES ((size_t)4 << 20)
#define ARENA_PAGES (ARENA_BYTES / PS5P_PAGE)

struct arena {
   struct ps5_exec_region region;
   uint8_t run[ARENA_PAGES];  /* at a block's first page, its pages; 0 elsewhere */
   uint8_t used[ARENA_PAGES]; /* 1 for every page in a block */
   unsigned used_pages;
   struct arena *next;
};

struct kept {
   struct ps5_exec_region region;
   struct kept *next;
};

static struct arena *arenas;
static struct kept *kept;
/* Where the next region near an anchor is asked for: just past the last one.
 * The near search tries 64 MiB steps, which room only 62 regions within reach
 * of an anchor unless the kernel moves a taken hint to free space nearby. */
static uintptr_t near_hint;
static pthread_mutex_t kept_lock = PTHREAD_MUTEX_INITIALIZER;

/* A region for the pointer form, placed past the last near one. */
static int
kept_alloc(struct ps5_exec_request *request, struct ps5_exec_region *region)
{
   if (request->flags & PS5_EXEC_NEAR)
      request->address = near_hint;
   const int result = ps5_exec_alloc(request, region);
   if (result == 0 && (request->flags & PS5_EXEC_NEAR))
      near_hint = (uintptr_t)region->base + region->bytes;
   return result;
}

/* The first free run of pages in the arena, taken and zeroed, or NULL. */
static void *
arena_take(struct arena *arena, unsigned pages)
{
   for (unsigned first = 0; first + pages <= ARENA_PAGES; first++) {
      unsigned free_pages = 0;
      while (free_pages < pages && !arena->used[first + free_pages])
         free_pages++;
      if (free_pages < pages) {
         first += free_pages; /* past the used page that ended the run */
         continue;
      }
      memset(&arena->used[first], 1, pages);
      arena->run[first] = (uint8_t)pages;
      arena->used_pages += pages;
      char *const block = (char *)arena->region.base + (size_t)first * PS5P_PAGE;
      memset(block, 0, (size_t)pages * PS5P_PAGE);
      return block;
   }
   return NULL;
}

void *
ps5_exec_allocate(size_t bytes, uintptr_t anchor)
{
   if (bytes == 0)
      return NULL;
   struct ps5_exec_request request = {
      .bytes = bytes,
      .anchor = anchor,
      .flags = anchor != 0 ? PS5_EXEC_NEAR : 0,
   };
   void *base = NULL;
   pthread_mutex_lock(&kept_lock);
   if (bytes < PS5P_DIRECT_UNIT) {
      const unsigned pages = (unsigned)(ps5p_round_up(bytes, PS5P_PAGE) / PS5P_PAGE);
      for (struct arena *arena = arenas; arena && !base; arena = arena->next)
         if (anchor == 0 ||
             ps5p_near_enough((uintptr_t)arena->region.base, arena->region.bytes, anchor))
            base = arena_take(arena, pages);
      if (!base) {
         struct arena *const arena = calloc(1, sizeof(*arena));
         request.bytes = ARENA_BYTES;
         if (arena && kept_alloc(&request, &arena->region) == 0) {
            arena->next = arenas;
            arenas = arena;
            base = arena_take(arena, pages);
         } else {
            free(arena);
         }
      }
   } else {
      struct kept *const node = malloc(sizeof(*node));
      if (node && kept_alloc(&request, &node->region) == 0) {
         node->next = kept;
         kept = node;
         base = node->region.base;
      } else {
         free(node);
      }
   }
   pthread_mutex_unlock(&kept_lock);
   return base;
}

int
ps5_exec_release(void *base)
{
   if (!base)
      return PS5_EXEC_BAD_REQUEST;
   int result = PS5_EXEC_BAD_REQUEST;
   pthread_mutex_lock(&kept_lock);
   for (struct kept **link = &kept; *link; link = &(*link)->next)
      if ((*link)->region.base == base) {
         struct kept *const node = *link;
         *link = node->next;
         ps5_exec_free(&node->region);
         free(node);
         result = 0;
         break;
      }
   for (struct arena **link = &arenas; result != 0 && *link; link = &(*link)->next) {
      struct arena *const arena = *link;
      const uintptr_t at = (uintptr_t)base, start = (uintptr_t)arena->region.base;
      if (at < start || at >= start + arena->region.bytes)
         continue;
      const size_t offset = at - start;
      const unsigned first = (unsigned)(offset / PS5P_PAGE);
      const unsigned pages = arena->run[first];
      if (offset % PS5P_PAGE != 0 || pages == 0)
         break; /* inside an arena, but not a block it handed out */
      arena->run[first] = 0;
      memset(&arena->used[first], 0, pages);
      arena->used_pages -= pages;
      if (arena->used_pages == 0) {
         *link = arena->next;
         ps5_exec_free(&arena->region);
         free(arena);
      } else {
         sceKernelMprotect(base, (size_t)pages * PS5P_PAGE, PROT_RWX);
      }
      result = 0;
   }
   pthread_mutex_unlock(&kept_lock);
   return result;
}
