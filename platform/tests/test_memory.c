#include "ps5platform/memory.h"
#include "ps5platform/kernel.h"
#include <assert.h>
#include <stdio.h>
#define UNIT 0x4000
static const unsigned holes[][2] = {{1,3}, {5,6}, {9,15}};
static int mode;
int64_t sceKernelGetDirectMemorySize(void) { return mode == 3 ? -1 : 16 * UNIT; }
int32_t sceKernelAvailableFlexibleMemorySize(size_t *n) { *n = 42; return 0; }
int32_t sceKernelAvailableDirectMemorySize(int64_t lo, int64_t hi, size_t a, int64_t *at, size_t *n) {
   (void)a; *at = 0; *n = 0;
   if (mode == 1) return 0;
   if (mode == 2) { *at = hi; *n = UNIT; return 0; }
   if (mode == 4) return -1;
   for (unsigned i=0; i<3; i++) {
      int64_t first = holes[i][0] * UNIT, last = holes[i][1] * UNIT;
      if (first < lo) first = lo;
      if (last > hi) last = hi;
      if (last > first && (uint64_t)(last-first) > *n) { *at=first; *n=last-first; }
   }
   return mode == 5 && !*n ? (int32_t)UINT32_C(0x8002000c) : 0;
}
int main(void) {
   struct ps5_memory_stats s;
   assert(ps5_memory_query(&s));
   assert(s.total_bytes == 16*UNIT && s.free_bytes == 9*UNIT);
   assert(s.largest_bytes == 6*UNIT && s.free_ranges == 3 && s.flexible_bytes == 42);
   mode=5; assert(ps5_memory_query(&s) && s.free_bytes==9*UNIT && s.free_ranges==3);
   mode=1; assert(ps5_memory_query(&s) && s.free_bytes==0 && s.free_ranges==0);
   mode=2; assert(!ps5_memory_query(&s));
   mode=3; assert(!ps5_memory_query(&s));
   mode=4; assert(!ps5_memory_query(&s));
   assert(!ps5_memory_query(NULL));
   puts("memory: fragmented, exhausted, invalid and unavailable snapshots pass");
}
