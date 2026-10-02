#include "ps5platform/memory.h"
#include "ps5platform/kernel.h"
#include <string.h>

bool
ps5_memory_query(struct ps5_memory_stats *stats)
{
   if (!stats)
      return false;
   memset(stats, 0, sizeof(*stats));
   const int64_t total = sceKernelGetDirectMemorySize();
   if (total <= 0)
      return false;
   stats->total_bytes = (uint64_t)total;
   size_t flexible = 0;
   if (sceKernelAvailableFlexibleMemorySize(&flexible) == 0)
      stats->flexible_bytes = flexible;
   /* Ask for the largest hole, then search both sides. Process the smaller
    * side first: the stack is bounded by log2(pool/page), not holes.
    * No malloc: this is also used after an allocation failed. */
   struct span { int64_t low, high; } pending[64], current = {0, total};
   unsigned count = 0, queries = 0;
   for (;;) {
      if (current.high - current.low >= (int64_t)PS5_KERNEL_PAGE_SIZE) {
         int64_t start = 0;
         size_t bytes = 0;
         if (++queries > 8192)
            return false;
         const int32_t result = sceKernelAvailableDirectMemorySize(current.low,
            current.high, PS5_KERNEL_PAGE_SIZE, &start, &bytes);
         /* The console returns SCE_KERNEL_ERROR_ENOMEM for an occupied
          * search interval; this is an empty branch, not a failed query. */
         if ((uint32_t)result == UINT32_C(0x8002000c))
            bytes = 0;
         else if (result != 0)
            return false;
         if (bytes) {
            if (start < current.low || start >= current.high ||
                bytes > (uint64_t)(current.high - start))
               return false;
            stats->free_bytes += bytes;
            if (bytes > stats->largest_bytes)
               stats->largest_bytes = bytes;
            stats->free_ranges++;
            struct span left = {current.low, start};
            struct span right = {start + (int64_t)bytes, current.high};
            /* Process the smaller side first, so every nested pending
             * branch came from at most half its parent's range. */
            if (left.high - left.low > right.high - right.low) {
               struct span swap = left; left = right; right = swap;
            }
            if (right.high - right.low >= (int64_t)PS5_KERNEL_PAGE_SIZE) {
               if (count == 64)
                  return false;
               pending[count++] = right;
            }
            current = left;
            continue;
         }
      }
      if (!count)
         return true;
      current = pending[--count];
   }
}
