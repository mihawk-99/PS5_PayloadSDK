/*
 * PS5 Platform - where mappings go (src/placement.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "placement.h"

#include "ps5platform/kernel.h"

/* Placed-anywhere candidates: the hint, then the view area in 4 GiB steps. */
#define PS5P_AREA_STEP ((uintptr_t)0x100000000ull)
#define PS5P_AREA_CANDIDATES 32
/* Near-anchor candidates, on each side, in 64 MiB steps. */
#define PS5P_NEAR_GRANULE ((uintptr_t)0x4000000ull)
/* Nothing is placed in the first 16 MiB of the address space. */
#define PS5P_LOWEST ((uintptr_t)0x1000000ull)

static uintptr_t
candidate(uintptr_t hint, unsigned attempt)
{
   if (attempt == 0 && hint != 0)
      return hint;
   return PS5P_VIEW_AREA + (uintptr_t)(attempt - (hint != 0 ? 1u : 0u)) * PS5P_AREA_STEP;
}

int
ps5p_map_placed(int64_t direct_start, size_t bytes, uintptr_t hint, int protection, int no_place,
                void **address)
{
   int32_t last = no_place;
   for (unsigned attempt = 0; attempt <= PS5P_AREA_CANDIDATES; attempt++) {
      const uintptr_t at = candidate(hint, attempt);
      if (!ps5p_outside_gpu_window(at, bytes))
         continue;
      void *placed = (void *)at;
      const int32_t result = sceKernelMapDirectMemory(&placed, bytes, protection, 0, direct_start,
                                                      PS5P_DIRECT_UNIT);
      if (result != 0) {
         last = result;
         continue;
      }
      if (ps5p_outside_gpu_window((uintptr_t)placed, bytes)) {
         *address = placed;
         return 0;
      }
      sceKernelMunmap(placed, bytes);
      last = no_place;
   }
   return last;
}

int
ps5p_reserve_placed(size_t bytes, uintptr_t hint, size_t alignment, int no_place, void **address)
{
   int32_t last = no_place;
   for (unsigned attempt = 0; attempt <= PS5P_AREA_CANDIDATES; attempt++) {
      const uintptr_t at = candidate(hint, attempt);
      if (!ps5p_outside_gpu_window(at, bytes))
         continue;
      void *placed = (void *)at;
      const int32_t result = sceKernelReserveVirtualRange(&placed, bytes, 0, alignment);
      if (result != 0) {
         last = result;
         continue;
      }
      if (ps5p_outside_gpu_window((uintptr_t)placed, bytes)) {
         *address = placed;
         return 0;
      }
      sceKernelMunmap(placed, bytes);
      last = no_place;
   }
   return last;
}

static bool
near_enough(uintptr_t base, size_t bytes, uintptr_t anchor)
{
   if (base < PS5P_LOWEST || !ps5p_outside_gpu_window(base, bytes))
      return false;
   const uintptr_t low = anchor > PS5P_NEAR_REACH ? anchor - PS5P_NEAR_REACH : 0;
   return base >= low && base + bytes <= anchor + PS5P_NEAR_REACH;
}

static bool
try_reserve(uintptr_t at, size_t bytes, uintptr_t anchor, void **address)
{
   void *placed = (void *)at;
   if (sceKernelReserveVirtualRange(&placed, bytes, 0, PS5P_DIRECT_UNIT) != 0)
      return false;
   if (near_enough((uintptr_t)placed, bytes, anchor)) {
      *address = placed;
      return true;
   }
   sceKernelMunmap(placed, bytes);
   return false;
}

int
ps5p_reserve_near(size_t bytes, uintptr_t anchor, uintptr_t hint, int no_place, void **address)
{
   if (bytes >= PS5P_NEAR_REACH)
      return no_place;
   if (hint != 0 && near_enough(hint, bytes, anchor) && try_reserve(hint, bytes, anchor, address))
      return 0;
   const uintptr_t origin = anchor / PS5P_NEAR_GRANULE * PS5P_NEAR_GRANULE;
   const unsigned steps = (unsigned)(PS5P_NEAR_REACH / PS5P_NEAR_GRANULE);
   /* Above the anchor first, then below, nearest first on each side. */
   for (unsigned step = 1; step <= steps; step++) {
      const uintptr_t above = origin + step * PS5P_NEAR_GRANULE;
      if (near_enough(above, bytes, anchor) && try_reserve(above, bytes, anchor, address))
         return 0;
      const uintptr_t distance = step * PS5P_NEAR_GRANULE + ps5p_round_up(bytes, PS5P_NEAR_GRANULE);
      if (origin > distance) {
         const uintptr_t below = origin - distance;
         if (near_enough(below, bytes, anchor) && try_reserve(below, bytes, anchor, address))
            return 0;
      }
   }
   return no_place;
}
