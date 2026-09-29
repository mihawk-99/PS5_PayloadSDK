/*
 * PS5 Platform - host unit tests.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The library's own code against tests/host_kernel.c, which models the
 * console's measured behaviour. What only the console can show (that the
 * console executes such code, faults, costs) is the probe's (docs/PROBE.md).
 */
#define _GNU_SOURCE 1

#include "host_kernel.h"

/* host_libc.c: localeconv() reports an empty decimal point, as the console's. */
extern int host_empty_decimal_point;
#include "ps5platform/exec.h"
#include "ps5platform/fp.h"
#include "ps5platform/heap.h"
#include "ps5platform/klog.h"
#include "ps5platform/kernel.h"
#include "ps5platform/libc.h"
#include "ps5platform/platform.h"
#include "ps5platform/probe.h"
#include "ps5platform/shm.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <langinfo.h>
#include <pwd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <string.h>
#include <sys/times.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static unsigned checks, failures;

static void
check(bool passed, const char *what)
{
   checks++;
   if (!passed) {
      failures++;
      printf("  FAIL %s\n", what);
      fflush(stdout);
   }
}

static void
write_return(uint8_t *at, uint32_t value)
{
   at[0] = 0xb8;
   memcpy(at + 1, &value, 4);
   at[5] = 0xc3;
}

static uint32_t
call(const void *at)
{
   return ((uint32_t(*)(void))(uintptr_t)at)();
}

static bool
outside_window(const void *base, size_t bytes)
{
   const uintptr_t at = (uintptr_t)base;
   return at >= 0x300000000ull || at + bytes <= 0x200000000ull;
}

static bool
nothing_live(void)
{
   uint64_t regions = 1, bytes = 1;
   ps5_exec_live(&regions, &bytes);
   struct ps5_shm_stats stats;
   ps5_shm_live(&stats);
   unsigned stacks = 0, cached = 0;
   ps5_thread_stacks(&stacks, &cached);
   return regions == 0 && bytes == 0 && stats.objects == 0 && stats.views == 0 &&
          stats.ranges == 0 && stats.committed_bytes == 0 &&
          host_direct_allocations() == (long long)(stacks + cached);
}

/* The host's protection of the page at the address, from /proc/self/maps:
 * "rw-", "---", or "" when nothing is mapped there. */
static const char *
host_protection(const void *address)
{
   static char protection[4];
   protection[0] = 0;
   FILE *maps = fopen("/proc/self/maps", "r");
   if (!maps)
      return protection;
   unsigned long low, high;
   char bits[5];
   while (fscanf(maps, "%lx-%lx %4s%*[^\n]", &low, &high, bits) == 3) {
      if ((uintptr_t)address >= low && (uintptr_t)address < high) {
         memcpy(protection, bits, 3);
         protection[3] = 0;
         break;
      }
   }
   fclose(maps);
   return protection;
}

/* ---- executable regions ----------------------------------------------------- */

static void
test_model(void)
{
   /* The host kernel, as the console: no address means the GPU window, and
    * execute at map time is refused. */
   int64_t start = -1;
   check(sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), 0x10000, 0x10000, 12,
                                       &start) == 0,
         "model: a direct allocation");
   void *at = NULL;
   check(sceKernelMapDirectMemory(&at, 0x10000, 0x3, 0, start, 0x10000) == 0 &&
            !outside_window(at, 0x10000),
         "model: a mapping with no address lands in the GPU window");
   sceKernelMunmap(at, 0x10000);
   void *exec = (void *)0x500000000ull;
   check(sceKernelMapDirectMemory(&exec, 0x10000, 0x5, 0, start, 0x10000) == (int32_t)0x80020016,
         "model: execute at map time is refused");
   sceKernelReleaseDirectMemory(start, 0x10000);
}

static void
test_exec_anywhere(void)
{
   struct ps5_exec_request request = {.bytes = 100000};
   struct ps5_exec_region region;
   check(ps5_exec_alloc(&request, &region) == 0, "exec: a region anywhere");
   check(region.bytes == 0x20000 && region.write_view == region.base,
         "exec: rounded to 64 KiB, one view");
   check(outside_window(region.base, region.bytes), "exec: outside the GPU window");
   uint64_t regions = 0, bytes = 0;
   ps5_exec_live(&regions, &bytes);
   check(regions == 1 && bytes == 0x20000, "exec: counted live");
   write_return(region.base, 41);
   check(call(region.base) == 41, "exec: code runs");
   write_return(region.base, 42);
   check(call(region.base) == 42, "exec: rewritten in place, read-write-execute");
   ps5_exec_free(&region);
   check(region.bytes == 0 && nothing_live(), "exec: freed, nothing live");
}

static void
test_exec_near(void)
{
   const uintptr_t anchor = (uintptr_t)(void *)&test_exec_near;
   struct ps5_exec_request request = {.bytes = 64 << 20, .anchor = anchor, .flags = PS5_EXEC_NEAR};
   struct ps5_exec_region region;
   check(ps5_exec_alloc(&request, &region) == 0, "near: a 64 MiB region near the anchor");
   const uintptr_t base = (uintptr_t)region.base;
   check(base + region.bytes <= anchor + 0x7c000000ull && base + 0x7c000000ull >= anchor,
         "near: every byte within reach of the anchor");
   check(outside_window(region.base, region.bytes), "near: outside the GPU window");
   write_return((uint8_t *)region.base + region.bytes - 64, 7);
   check(call((uint8_t *)region.base + region.bytes - 64) == 7, "near: code at its end runs");
   /* A second one does not overlap the first. */
   struct ps5_exec_region second;
   check(ps5_exec_alloc(&request, &second) == 0 &&
            ((uintptr_t)second.base >= base + region.bytes ||
             (uintptr_t)second.base + second.bytes <= base),
         "near: a second region elsewhere near the anchor");
   ps5_exec_free(&second);
   ps5_exec_free(&region);
   request.bytes = (size_t)3 << 30;
   check(ps5_exec_alloc(&request, &region) == PS5_EXEC_NO_PLACE && nothing_live(),
         "near: 3 GiB cannot be near anything, and nothing is left");
}

/* The pointer-only form the cores' allocators use: Dolphin's near and far
 * caches, 128 and 64 MiB, near the core's code and within a 32-bit jump of
 * each other; freed by address; an unknown address refused. */
static void
test_exec_pointer(void)
{
   const uintptr_t anchor = (uintptr_t)(void *)&test_exec_pointer;
   uint8_t *const near = ps5_exec_allocate((size_t)128 << 20, anchor);
   uint8_t *const far = ps5_exec_allocate((size_t)64 << 20, anchor);
   check(near && far, "pointer: 128 and 64 MiB near the anchor");
   const uintptr_t low = (uintptr_t)(near < far ? near : far);
   const uintptr_t high = near < far ? (uintptr_t)far + ((size_t)64 << 20)
                                     : (uintptr_t)near + ((size_t)128 << 20);
   check(near && far && high - low < 0x80000000ull,
         "pointer: the two lie within one 32-bit displacement of each other");
   check(near && outside_window(near, (size_t)128 << 20) && far &&
            outside_window(far, (size_t)64 << 20),
         "pointer: outside the GPU window");
   if (near) {
      write_return(near + 4096, 9);
      check(call(near + 4096) == 9, "pointer: code runs, read-write-execute");
   }
   uint64_t regions = 0;
   ps5_exec_live(&regions, NULL);
   check(regions == 2, "pointer: two regions live");
   check(ps5_exec_release(near + 4096) == PS5_EXEC_BAD_REQUEST,
         "pointer: an address inside a region is not one it returned");
   check(ps5_exec_release(near) == 0 && ps5_exec_release(far) == 0 && nothing_live(),
         "pointer: both freed by address, nothing live");
   check(ps5_exec_release(near) == PS5_EXEC_BAD_REQUEST, "pointer: freed twice is refused");
   uint8_t *const anywhere = ps5_exec_allocate(4096, 0);
   check(anywhere && outside_window(anywhere, 0x10000), "pointer: with no anchor, anywhere");
   check(ps5_exec_release(anywhere) == 0 && nothing_live(), "pointer: and freed");
}

/* No limit on how many are live. Dolphin takes 4 KiB of code for each vertex
 * format a game draws with, and a table of 128 regions ran out between a
 * mission and the main menu. Small blocks share arenas, whole 16 KiB pages
 * each; large requests are regions of their own. */
static void
test_exec_many(void)
{
   enum { BLOCKS = 1000, REGIONS = 200 };
   static uint8_t *block[BLOCKS];
   static uint8_t *region[REGIONS];
   const uintptr_t anchor = (uintptr_t)(void *)&test_exec_many;
   unsigned handed = 0, placed = 0, zeroed = 0, runs = 0;
   for (unsigned i = 0; i < BLOCKS; i++) {
      block[i] = ps5_exec_allocate(4096, anchor);
      if (!block[i])
         continue;
      handed++;
      const uintptr_t at = (uintptr_t)block[i];
      placed += at % 0x4000 == 0 && at + 0x7c000000ull >= anchor &&
                at + 0x4000 <= anchor + 0x7c000000ull && outside_window(block[i], 0x4000);
      bool clear = true;
      for (unsigned b = 0; b < 0x4000; b++)
         clear &= block[i][b] == 0;
      zeroed += clear;
      write_return(block[i], i);
      runs += call(block[i]) == i;
   }
   check(handed == BLOCKS, "many: 1,000 blocks of 4 KiB, past the old limit of 128");
   check(placed == BLOCKS, "many: each on 16 KiB pages of its own, within reach of the anchor");
   check(zeroed == BLOCKS && runs == BLOCKS, "many: each zeroed, and its code runs");
   unsigned intact = 0;
   for (unsigned i = 0; i < BLOCKS; i++)
      intact += block[i] && call(block[i]) == i;
   check(intact == BLOCKS, "many: no block overlaps another");
   uint64_t regions = 0;
   ps5_exec_live(&regions, NULL);
   check(regions <= (BLOCKS * 0x4000) / (4 << 20) + 1, "many: in a few shared arenas, not a region each");
   check(ps5_exec_release(block[10] + 4096) == PS5_EXEC_BAD_REQUEST,
         "many: an address inside a block is not one it returned");
   check(ps5_exec_release(block[10]) == 0 && ps5_exec_release(block[10]) == PS5_EXEC_BAD_REQUEST,
         "many: a block freed once, and refused the second time");
   block[10] = NULL;
   unsigned large = 0;
   for (unsigned i = 0; i < REGIONS; i++) {
      region[i] = ps5_exec_allocate(0x10000, anchor);
      if (region[i]) {
         write_return(region[i] + 0x10000 - 64, i);
         large += call(region[i] + 0x10000 - 64) == i;
      }
   }
   check(large == REGIONS, "many: 200 regions of 64 KiB, each of its own, past the old limit");
   unsigned released = 0;
   for (unsigned i = 0; i < REGIONS; i++)
      released += ps5_exec_release(region[i]) == 0;
   for (unsigned i = 0; i < BLOCKS; i++)
      released += block[i] && ps5_exec_release(block[i]) == 0;
   check(released == REGIONS + BLOCKS - 1 && nothing_live(), "many: all freed, nothing live");

   /* A core that takes execute away from its block before freeing it (PPSSPP
    * does) leaves the page runnable for the next block handed out there. */
   uint8_t *const first = ps5_exec_allocate(4096, anchor);
   uint8_t *const second = ps5_exec_allocate(4096, anchor);
   sceKernelMprotect(first, 0x4000, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE);
   check(first && second && ps5_exec_release(first) == 0, "many: a read-write block freed");
   uint8_t *const reused = ps5_exec_allocate(8000, anchor);
   check(reused == first, "many: its page handed out again");
   if (reused == first && reused) {
      write_return(reused, 77);
      check(call(reused) == 77, "many: and its code runs");
   }
   /* An anchor out of reach of the arena gets one of its own. */
   const uintptr_t far_anchor = anchor + ((uintptr_t)8 << 30);
   uint8_t *const far = ps5_exec_allocate(4096, far_anchor);
   check(far && (uintptr_t)far + 0x7c000000ull >= far_anchor &&
            (uintptr_t)far + 0x4000 <= far_anchor + 0x7c000000ull,
         "many: a far anchor's block is within its reach");
   check(ps5_exec_release(reused) == 0 && ps5_exec_release(second) == 0 &&
            ps5_exec_release(far) == 0 && nothing_live(),
         "many: and all of it freed");
}

static void
test_exec_fixed(void)
{
   void *range = NULL;
   check(ps5_vrange_reserve(8 << 20, NULL, 0x10000, &range) == 0, "fixed: a reserved range");
   struct ps5_exec_request request = {
      .bytes = 1 << 20, .address = (uintptr_t)range + (2 << 20), .flags = PS5_EXEC_FIXED};
   struct ps5_exec_region region;
   check(ps5_exec_alloc(&request, &region) == 0 && region.base == (void *)request.address,
         "fixed: mapped exactly there, inside the range");
   write_return(region.base, 9);
   check(call(region.base) == 9, "fixed: code runs");
   ps5_exec_free(&region);
   request.address += 0x4000;
   check(ps5_exec_alloc(&request, &region) == PS5_EXEC_BAD_REQUEST,
         "fixed: an address off the 64 KiB unit is refused");
   ps5_vrange_release(range, 8 << 20);
   check(nothing_live(), "fixed: nothing live");
}

/* PS5_EXEC_AT: exactly at the address when the range is free, and never over
 * what is already there -- how LRPS2 tries candidate places for its code area. */
static void
test_exec_at(void)
{
   void *range = NULL;
   check(ps5_vrange_reserve(8 << 20, NULL, 0x10000, &range) == 0, "at: a free place found");
   ps5_vrange_release(range, 8 << 20);
   struct ps5_exec_request request = {
      .bytes = 4 << 20, .address = (uintptr_t)range, .flags = PS5_EXEC_AT};
   struct ps5_exec_region region;
   check(ps5_exec_alloc(&request, &region) == 0 && region.base == range,
         "at: mapped exactly there while the range is free");
   write_return(region.base, 12);
   struct ps5_exec_region second;
   check(ps5_exec_alloc(&request, &second) == PS5_EXEC_NO_PLACE,
         "at: the same address again, taken, is refused");
   check(call(region.base) == 12, "at: and what was there still runs, not replaced");
   ps5_exec_free(&region);
   request.address = 0;
   check(ps5_exec_alloc(&request, &second) == PS5_EXEC_BAD_REQUEST, "at: no address is refused");
   request.address = 0x200010000ull;
   check(ps5_exec_alloc(&request, &second) == PS5_EXEC_NO_PLACE,
         "at: an address in the GPU window is never given out");
   request.address = (uintptr_t)range;
   request.flags = PS5_EXEC_AT | PS5_EXEC_NEAR;
   check(ps5_exec_alloc(&request, &second) == PS5_EXEC_BAD_REQUEST,
         "at: with another placement is refused");
   check(nothing_live(), "at: nothing live");
}

static void
test_exec_dual(void)
{
   struct ps5_exec_request request = {.bytes = 1 << 20, .flags = PS5_EXEC_DUAL_VIEW};
   struct ps5_exec_region region;
   check(ps5_exec_alloc(&request, &region) == 0 && region.write_view != region.base,
         "dual: two views");
   check(outside_window(region.write_view, region.bytes), "dual: the write view is placed too");
   for (uint32_t i = 0; i < 100; i++) {
      write_return((uint8_t *)region.write_view + i * 64, i);
      if (call((uint8_t *)region.base + i * 64) != i) {
         check(false, "dual: written through one view, run through the other");
         break;
      }
   }
   ps5_exec_free(&region);
   request.flags = PS5_EXEC_DUAL_VIEW | PS5_EXEC_TOGGLED;
   check(ps5_exec_alloc(&request, &region) == PS5_EXEC_BAD_REQUEST, "dual: toggled too is refused");
   check(nothing_live(), "dual: nothing live");
}

static void
test_exec_toggled(void)
{
   struct ps5_exec_request request = {.bytes = 4 * 0x4000, .flags = PS5_EXEC_TOGGLED};
   struct ps5_exec_region region;
   check(ps5_exec_alloc(&request, &region) == 0, "toggled: a region");
   write_return(region.base, 3);
   check(ps5_exec_protect(&region, 0, 6, false) == 0 && call(region.base) == 3,
         "toggled: made executable, runs");
   check(ps5_exec_protect(&region, 0x4000 + 10, 20, true) == 0, "toggled: one page writable");
   write_return((uint8_t *)region.base + 0x4000 + 10, 4);
   check(ps5_exec_protect(&region, 0x4000 + 10, 20, false) == 0 &&
            call((uint8_t *)region.base + 0x4000 + 10) == 4 && call(region.base) == 3,
         "toggled: that page rewritten, both run");
   ps5_exec_free(&region);
   check(nothing_live(), "toggled: nothing live");
}

static void
test_exec_unwinding(void)
{
   static const struct {
      int call;
      unsigned flags;
      const char *what;
   } cases[] = {
      {HOST_CALL_ALLOCATE, 0, "unwind: the allocation fails"},
      {HOST_CALL_MAP, 0, "unwind: the map fails"},
      {HOST_CALL_PROTECT, 0, "unwind: the protection fails"},
      {HOST_CALL_RESERVE, PS5_EXEC_NEAR, "unwind: the reservation near the anchor fails"},
      {HOST_CALL_MAP, PS5_EXEC_NEAR, "unwind: the map into the reservation fails"},
      {HOST_CALL_PROTECT, PS5_EXEC_DUAL_VIEW, "unwind: the dual view's protection fails"},
   };
   for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      struct ps5_exec_request request = {
         .bytes = 1 << 20, .anchor = (uintptr_t)(void *)&test_exec_unwinding, .flags = cases[i].flags};
      struct ps5_exec_region region;
      host_fail(cases[i].call, 1);
      const int result = ps5_exec_alloc(&request, &region);
      host_fail(HOST_CALL_NONE, 0);
      /* A near reservation that fails is retried at the next candidate, so it
       * may still succeed; what matters is that a failure leaves nothing. */
      if (result == 0)
         ps5_exec_free(&region);
      check(nothing_live() && region.bytes == 0, cases[i].what);
   }
   /* The dual view's second map, the second map call of the allocation. */
   struct ps5_exec_request request = {.bytes = 1 << 20, .flags = PS5_EXEC_DUAL_VIEW};
   struct ps5_exec_region region;
   host_fail(HOST_CALL_MAP, 2);
   int result = ps5_exec_alloc(&request, &region);
   host_fail(HOST_CALL_NONE, 0);
   if (result == 0)
      ps5_exec_free(&region);
   check(nothing_live(), "unwind: the dual view's second map fails");
}

struct churn {
   unsigned cycles;
   unsigned failed;
   bool blocks; /* through ps5_exec_allocate, small and large in turn */
};

static void *
churn(void *argument)
{
   struct churn *const c = argument;
   for (unsigned i = 0; c->blocks && i < c->cycles; i++) {
      const size_t bytes = i % 5 == 4 ? 0x20000 : 4096 * (1 + i % 4);
      uint8_t *const at = ps5_exec_allocate(bytes, (uintptr_t)(void *)&churn);
      if (!at) {
         c->failed++;
         continue;
      }
      write_return(at, i);
      c->failed += call(at) != i;
      c->failed += ps5_exec_release(at) != 0;
   }
   for (unsigned i = 0; !c->blocks && i < c->cycles; i++) {
      struct ps5_exec_request request = {.bytes = 0x10000 * (1 + i % 4)};
      struct ps5_exec_region region;
      if (ps5_exec_alloc(&request, &region) != 0) {
         c->failed++;
         continue;
      }
      write_return(region.base, i);
      c->failed += call(region.base) != i;
      ps5_exec_free(&region);
   }
   return NULL;
}

static void
test_exec_threads(void)
{
   enum { THREADS = 8 };
   pthread_t threads[THREADS];
   struct churn work[THREADS];
   for (unsigned t = 0; t < THREADS; t++) {
      work[t] = (struct churn){.cycles = 200};
      pthread_create(&threads[t], NULL, churn, &work[t]);
   }
   unsigned failed = 0;
   for (unsigned t = 0; t < THREADS; t++) {
      pthread_join(threads[t], NULL);
      failed += work[t].failed;
   }
   check(failed == 0 && nothing_live(), "threads: 1,600 allocations on eight threads, counted back to zero");
   for (unsigned t = 0; t < THREADS; t++) {
      work[t] = (struct churn){.cycles = 400, .blocks = true};
      pthread_create(&threads[t], NULL, churn, &work[t]);
   }
   failed = 0;
   for (unsigned t = 0; t < THREADS; t++) {
      pthread_join(threads[t], NULL);
      failed += work[t].failed;
   }
   check(failed == 0 && nothing_live(),
         "threads: 3,200 blocks and regions by pointer on eight threads, counted back to zero");
}

/* ---- shared memory ---------------------------------------------------------- */

static void
test_shm(void)
{
   struct ps5_shm shm;
   check(ps5_shm_create(3 << 20, &shm) == 0 && shm.bytes == 3 << 20, "shm: an object");
   void *a = NULL, *b = NULL;
   check(ps5_shm_map(&shm, 0, 3 << 20, NULL, PS5_SHM_READ | PS5_SHM_WRITE, 0, &a) == 0 &&
            outside_window(a, 3 << 20),
         "shm: a view, placed");
   check(((volatile uint8_t *)a)[0] == 0 && ((volatile uint8_t *)a)[(3 << 20) - 1] == 0,
         "shm: a new object reads zero");
   check(ps5_shm_map(&shm, 1 << 20, 1 << 20, NULL, PS5_SHM_READ | PS5_SHM_WRITE, 0, &b) == 0 &&
            a != b,
         "shm: a second view of its middle");
   ((volatile uint8_t *)a)[(1 << 20) + 5] = 0x5a;
   check(((volatile uint8_t *)b)[5] == 0x5a, "shm: written through one view, read through the other");
   /* A view at a 16 KiB page offset, off the 64 KiB unit (PPSSPP's VRAM view
    * starts at 80 KiB); one at an offset off the page is refused. */
   void *paged = NULL;
   check(ps5_shm_map(&shm, 0x14000, 0x4000, NULL, PS5_SHM_READ | PS5_SHM_WRITE, 0, &paged) == 0,
         "shm: a view at an offset of 80 KiB");
   ((volatile uint8_t *)a)[0x14000 + 9] = 0x77;
   check(paged && ((volatile uint8_t *)paged)[9] == 0x77, "shm: and it is that page of the object");
   ps5_shm_unmap(paged, 0x4000, 0);
   check(ps5_shm_map(&shm, 0x1000, 0x4000, NULL, PS5_SHM_READ | PS5_SHM_WRITE, 0, &paged) ==
            PS5_SHM_BAD_REQUEST,
         "shm: an offset off the 16 KiB page is refused");
   /* An arena: a reserved range, a view mapped into it, a hole kept reserved. */
   void *arena = NULL;
   check(ps5_vrange_reserve(16 << 20, NULL, 0, &arena) == 0, "shm: an arena reserved");
   void *mirror = NULL;
   check(ps5_shm_map(&shm, 0, 1 << 20, (uint8_t *)arena + (4 << 20), PS5_SHM_READ | PS5_SHM_WRITE,
                     PS5_SHM_FIXED, &mirror) == 0 &&
            mirror == (uint8_t *)arena + (4 << 20),
         "shm: a mirror at a fixed place in the arena");
   ((volatile uint8_t *)mirror)[7] = 0x33;
   check(((volatile uint8_t *)a)[7] == 0x33, "shm: the mirror is the object");
   check(ps5_shm_unmap(mirror, 1 << 20, PS5_SHM_KEEP_RESERVED) == 0, "shm: the mirror unmapped");
   /* Kept reserved: a mapping that must not replace anything cannot go there. */
   void *probe = mmap(mirror, 0x10000, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                      -1, 0);
   check(probe == MAP_FAILED && errno == EEXIST, "shm: the hole stays reserved");
   check(ps5_shm_map(&shm, 0, 1 << 20, mirror, PS5_SHM_READ | PS5_SHM_WRITE, PS5_SHM_FIXED,
                     &mirror) == 0 &&
            ((volatile uint8_t *)mirror)[7] == 0x33,
         "shm: mapped there again");
   /* Execute through a view: granted after the map. */
   void *code = NULL;
   check(ps5_shm_map(&shm, 2 << 20, 0x10000, NULL,
                     PS5_SHM_READ | PS5_SHM_WRITE | PS5_SHM_EXEC, 0, &code) == 0,
         "shm: a read-write-execute view");
   write_return(code, 17);
   check(call(code) == 17, "shm: code in it runs");
   struct ps5_shm_stats stats;
   ps5_shm_live(&stats);
   check(stats.objects == 1 && stats.views == 4 && stats.ranges == 1, "shm: counted live");
   char line[160];
   ps5_platform_report(line, sizeof(line));
   check(strstr(line, "shm=1/3MiB views=4 ranges=1/16MiB") != NULL, "report: the live line");
   ps5_shm_unmap(code, 0x10000, 0);
   ps5_shm_unmap(mirror, 1 << 20, 0);
   ps5_vrange_release(arena, 16 << 20);
   ps5_shm_unmap(b, 1 << 20, 0);
   ps5_shm_unmap(a, 3 << 20, 0);
   ps5_shm_destroy(&shm);
   check(nothing_live(), "shm: nothing live");
   check(ps5_shm_map(&shm, 0, 0x4000, NULL, 0x3, 0, &a) == PS5_SHM_BAD_REQUEST,
         "shm: a view of a destroyed object is refused");
}

/* Committed memory in a reserved range: 64 KiB units of direct memory, backed
 * at their first commit, given back when decommitted whole. */
static void
test_vrange_commit(void)
{
   uint8_t *base = NULL;
   check(ps5_vrange_reserve(1 << 20, NULL, 0x10000, (void **)&base) == 0, "commit: a range");
   struct ps5_shm_stats stats;
   check(ps5_vrange_commit(base + 0x4000, 0x8000, PS5_SHM_READ | PS5_SHM_WRITE) == 0 &&
            base[0x4000] == 0 && base[0xbfff] == 0,
         "commit: two pages, reading zero");
   ps5_shm_live(&stats);
   check(stats.committed_bytes == 0x10000, "commit: backed by one unit");
   check(!strcmp(host_protection(base + 0x4000), "rw-") &&
            !strcmp(host_protection(base + 0x8000), "rw-") &&
            !strcmp(host_protection(base), "---") && !strcmp(host_protection(base + 0xc000), "---"),
         "commit: the unit's other pages have no access");
   base[0x4000] = 0x11;
   check(ps5_vrange_commit(base + 0x4000, 0x8000, PS5_SHM_READ) == 0 && base[0x4000] == 0x11 &&
            !strcmp(host_protection(base + 0x4000), "r--"),
         "commit: again, read-only, contents kept");
   check(ps5_vrange_commit(base + 0x10000 - 0x100, 0x20200, PS5_SHM_READ | PS5_SHM_WRITE) == 0,
         "commit: a range across units");
   ps5_shm_live(&stats);
   check(stats.committed_bytes == 4 * 0x10000, "commit: four units backed");
   memset(base + 0x20000, 0x5a, 0x10000);
   check(ps5_vrange_decommit(base + 0x20000, 0x10000) == 0, "decommit: a whole unit");
   ps5_shm_live(&stats);
   check(stats.committed_bytes == 3 * 0x10000 && !strcmp(host_protection(base + 0x20000), "---"),
         "decommit: given back, reserved again");
   check(ps5_vrange_commit(base + 0x20000, 0x10000, PS5_SHM_READ | PS5_SHM_WRITE) == 0 &&
            base[0x20000] == 0 && base[0x2ffff] == 0,
         "decommit: zero at its next commit");
   base[0x10100] = 0xaa;
   check(ps5_vrange_decommit(base + 0x10000, 0x4000) == 0 && base[0x10100] == 0 &&
            !strcmp(host_protection(base + 0x10000), "rw-"),
         "decommit: part of a unit is zeroed, and kept");
   ps5_shm_live(&stats);
   check(stats.committed_bytes == 4 * 0x10000, "decommit: the partial unit stays backed");
   check(ps5_vrange_commit(base + 0x80000, 0x10000, PS5_SHM_READ | PS5_SHM_WRITE | PS5_SHM_EXEC) == 0,
         "commit: read-write-execute");
   write_return(base + 0x80000, 23);
   check(call(base + 0x80000) == 23, "commit: code in it runs");
   check(ps5_vrange_release(base, 1 << 20) == 0, "commit: the range released");
   ps5_shm_live(&stats);
   check(stats.committed_bytes == 0 && nothing_live(), "commit: its units went with it");
   check(ps5_vrange_reserve_at(base, 1 << 20) == 0 && ps5_vrange_reserve_at(base, 0x10000) == PS5_SHM_NO_PLACE,
         "reserve_at: exactly there, once");
   check(ps5_vrange_reserve_at((void *)0x200000000ull, 0x10000) == PS5_SHM_NO_PLACE,
         "reserve_at: never in the GPU window");
   check(ps5_vrange_reserve_at(base + 0x4000, 0x10000) == PS5_SHM_BAD_REQUEST,
         "reserve_at: a 64 KiB multiple");
   ps5_vrange_release(base, 1 << 20);
   check(nothing_live(), "reserve_at: nothing live");
}

/* memfd_create: an object ftruncate sizes, two shared views of it. */
static void
test_memfd(void)
{
   const int fd = ps5_memfd_create("test", PS5_MFD_CLOEXEC);
   check(fd >= 0, "memfd: created");
   check((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "memfd: closed on exec");
   check(ftruncate(fd, 3 * 0x4000) == 0, "memfd: sized");
   uint8_t *const a = mmap(NULL, 3 * 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   uint8_t *const b = mmap(NULL, 3 * 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   check(a != MAP_FAILED && b != MAP_FAILED && a != b, "memfd: two views");
   check(b[0x4000 + 3] == 0, "memfd: starts zeroed");
   a[0x4000 + 3] = 0xab;
   check(b[0x4000 + 3] == 0xab, "memfd: written through one view, read through the other");
   munmap(b, 3 * 0x4000);
   munmap(a, 3 * 0x4000);
   close(fd);
   const int plain = ps5_memfd_create("test", 0);
   check(plain >= 0 && (fcntl(plain, F_GETFD) & FD_CLOEXEC) == 0, "memfd: without the flag, kept on exec");
   close(plain);
   errno = 0;
   check(ps5_memfd_create("test", 0x2u) == -1 && errno == EINVAL, "memfd: sealing is refused");
}

/* ---- libc --------------------------------------------------------------------- */

/* accept4, getpagesizes, in6addr_any and thread affinity (src/libc.c). */
static void
test_libc_system(void)
{
   size_t sizes[2] = {0, 0};
   check(ps5_getpagesizes(NULL, 0) == 1 && ps5_getpagesizes(sizes, 2) == 1 && sizes[0] == 0x4000 &&
            sizes[1] == 0,
         "getpagesizes: the 16 KiB page");
   errno = 0;
   check(ps5_getpagesizes(NULL, 1) == -1 && errno == EINVAL, "getpagesizes: no array for a count");
   static const uint8_t zero[16];
   check(!memcmp(&ps5_in6addr_any, zero, sizeof(zero)), "in6addr_any: the wildcard");

   const int listener = socket(AF_INET, SOCK_STREAM, 0);
   struct sockaddr_in at = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
   socklen_t length = sizeof(at);
   check(listener >= 0 && bind(listener, (struct sockaddr *)&at, sizeof(at)) == 0 &&
            listen(listener, 1) == 0 && getsockname(listener, (struct sockaddr *)&at, &length) == 0,
         "accept4: a listener");
   const int client = socket(AF_INET, SOCK_STREAM, 0);
   check(connect(client, (struct sockaddr *)&at, sizeof(at)) == 0, "accept4: connected");
   const int accepted = ps5_accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
   check(accepted >= 0 && (fcntl(accepted, F_GETFD) & FD_CLOEXEC) &&
            (fcntl(accepted, F_GETFL) & O_NONBLOCK),
         "accept4: close-on-exec and non-blocking");
   errno = 0;
   check(ps5_accept4(listener, NULL, NULL, 0x4) == -1 && errno == EINVAL, "accept4: unknown flags");
   close(accepted);
   close(client);
   close(listener);

   uint8_t set[16];
   memset(set, 0xff, sizeof(set));
   uint64_t mask = 0;
   check(ps5_pthread_getaffinity_np(pthread_self(), sizeof(set), set) == 0 &&
            (memcpy(&mask, set, 8), mask != 0) && !memcmp(set + 8, zero, 8),
         "affinity: read as a set, CPUs past 63 clear");
   check(ps5_pthread_setaffinity_np(pthread_self(), sizeof(set), set) == 0, "affinity: set back");
   check(ps5_sysconf(_SC_NPROCESSORS_ONLN) == __builtin_popcountll(mask) &&
            ps5_sysconf(_SC_NPROCESSORS_CONF) == __builtin_popcountll(mask) &&
            ps5_sysconf(_SC_PAGESIZE) == sysconf(_SC_PAGESIZE),
         "sysconf: the CPUs in the affinity mask; the rest as sysconf says");
   uint8_t none[16] = {0};
   check(ps5_pthread_setaffinity_np(pthread_self(), sizeof(none), none) == EINVAL,
         "affinity: no CPU is refused");
   uint8_t far[16] = {0};
   far[0] = 1;
   far[9] = 1;
   check(ps5_pthread_setaffinity_np(pthread_self(), sizeof(far), far) == EINVAL,
         "affinity: CPUs past 63 are refused");
}

static void
test_libc(void)
{
   const time_t when = 1758844800; /* 2025-09-26 00:00:00 UTC */
   struct tm ours, theirs;
   check(ps5_gmtime_r(&when, &ours) == &ours && gmtime_r(&when, &theirs) &&
            ours.tm_year == theirs.tm_year && ours.tm_yday == theirs.tm_yday &&
            ours.tm_hour == theirs.tm_hour,
         "libc: gmtime_r");
   unsigned below = 1;
   for (int i = 0; i < 10000; i++)
      below &= ps5_arc4random_uniform(37) < 37;
   check(below, "libc: arc4random_uniform stays below its bound");
   unsigned char buffer[37] = {0};
   ps5_arc4random_buf(buffer, sizeof(buffer));
   unsigned nonzero = 0;
   for (size_t i = 0; i < sizeof(buffer); i++)
      nonzero += buffer[i] != 0;
   check(nonzero > 20, "libc: arc4random_buf fills its buffer");
   struct statvfs space;
   check(ps5_statvfs("/", &space) == 0 && space.f_bavail * space.f_frsize == (16ull << 30),
         "libc: statvfs answers 16 GiB free");
   check(ps5_statvfs("/no/such/path", &space) == -1, "libc: statvfs of a missing path fails");
   const char *text = "Commodore 64 disk.D64";
   check(ps5_strcasestr(text, ".d64") == text + 17 && ps5_strcasestr(text, "") == text &&
             ps5_strcasestr(text, "c128") == NULL && ps5_strcasestr("", "a") == NULL,
         "libc: strcasestr finds a needle regardless of case");
   char copy[16] = "xxxxxxxxxxxxxxx";
   check(ps5_memccpy(copy, "3ds:cia", ':', 7) == copy + 4 && memcmp(copy, "3ds:x", 5) == 0 &&
             ps5_memccpy(copy, "abc", 'z', 3) == NULL && memcmp(copy, "abc:x", 5) == 0,
         "libc: memccpy stops after the byte and says where");
   struct tms cpu;
   const clock_t start = ps5_times(&cpu);
   check(start != (clock_t)-1 && cpu.tms_stime == 0 && cpu.tms_cutime == 0,
         "libc: times answers in clock ticks");
   check(ps5_getpwuid(0) == NULL, "libc: getpwuid finds no user");
   check(ps5_gethostbyaddr("\x7f\0\0\1", 4, 2) == NULL, "libc: gethostbyaddr finds no host");
   check(ps5_gethostbyname("localhost") == NULL, "libc: gethostbyname finds no host");
   char interface_name[16];
   check(ps5_if_nametoindex("lo0") == 0 && ps5_if_indextoname(1, interface_name) == NULL,
         "libc: interface lookups find nothing, as if_nameindex does");
   char temporary[] = "/tmp/ps5-platform-tmpfile-XXXXXX";
   check(mkdtemp(temporary) != NULL, "libc: a directory for tmpfile");
   setenv("TMPDIR", temporary, 1);
   FILE *const scratch = ps5_tmpfile();
   char readback[8] = "";
   check(scratch && fputs("azahar", scratch) >= 0 && fseek(scratch, 0, SEEK_SET) == 0 &&
             fgets(readback, sizeof(readback), scratch) && !strcmp(readback, "azahar"),
         "libc: tmpfile reads back what was written");
   if (scratch)
      fclose(scratch);
   unsetenv("TMPDIR");
   check(rmdir(temporary) == 0, "libc: tmpfile left no file behind");
   char stem[] = "/tmp/ps5-platform-mkstemp-XXXXXX";
   const int made = ps5_mkstemp(stem);
   check(made >= 0 && strcmp(stem + strlen(stem) - 6, "XXXXXX") != 0, "libc: mkstemp names and opens");
   check(!ps5_isatty(made) && errno == ENOTTY, "libc: isatty finds no terminal");
   char target[32];
   check(ps5_readlink(stem, target, sizeof(target)) == -1 && errno == EINVAL,
         "libc: readlink finds a file is no link");
   check(ps5_readlink("/no/such/path", target, sizeof(target)) == -1 && errno == ENOENT,
         "libc: readlink of a missing path");
   check(ps5_link(stem, "/tmp/ps5-platform-link") == -1 && ps5_symlink(stem, "/tmp/ps5-platform-link") == -1,
         "libc: link and symlink are refused");
   check(ps5_fchown(made, 0, 0) == -1 && errno == EPERM, "libc: fchown is not permitted");
   close(made);
   unlink(stem);
   char host[16] = "x", service[16] = "y";
   check(ps5_getnameinfo("", 16, host, sizeof(host), service, sizeof(service), 0) == EAI_FAIL &&
             !host[0] && !service[0],
         "libc: getnameinfo is refused as getaddrinfo is");
   struct passwd entry, *found = &entry;
   char entry_buffer[256];
   check(ps5_getpwuid_r(getuid(), &entry, entry_buffer, sizeof(entry_buffer), &found) == 0 && found == NULL,
         "libc: getpwuid_r finds no user");
   {
      char name[] = "/tmp/ps5-platform-fallocate-XXXXXX";
      const int fd = mkstemp(name);
      unlink(name);
      const bool written = fd >= 0 && pwrite(fd, "abc", 3, 0) == 3;
      struct stat grown = {0};
      char head[3] = {0}, tail = 1;
      const bool ok = written && ps5_posix_fallocate(fd, 2, 40000) == 0 && fstat(fd, &grown) == 0 &&
                      grown.st_size == 40002 && pread(fd, head, 3, 0) == 3 && !memcmp(head, "abc", 3) &&
                      pread(fd, &tail, 1, 40001) == 1 && tail == 0 && ps5_posix_fallocate(fd, 0, 10) == 0 &&
                      fstat(fd, &grown) == 0 && grown.st_size == 40002;
      check(ok, "libc: posix_fallocate writes zeros past the end and leaves the rest");
      if (fd >= 0)
         close(fd);
   }
   {
      char folder[] = "/tmp/ps5-platform-access-XXXXXX";
      char file[64];
      const bool made = mkdtemp(folder) != NULL;
      snprintf(file, sizeof(file), "%s/file", folder);
      const int fd = made ? open(file, O_CREAT | O_WRONLY | O_CLOEXEC, 0644) : -1;
      if (fd >= 0)
         close(fd);
      check(fd >= 0 && ps5_access(file, F_OK) == 0 && ps5_access(file, R_OK | W_OK) == 0 &&
               ps5_access(folder, F_OK) == 0 && ps5_access(folder, R_OK | W_OK | X_OK) == 0,
            "libc: access finds a file and a folder, readable and writable");
      errno = 0;
      const bool missing = ps5_access("/no/such/path", F_OK) == -1 && errno == ENOENT;
      errno = 0;
      const bool not_executable = ps5_access(file, X_OK) == -1 && errno == EACCES;
      errno = 0;
      const bool bad_mode = ps5_access(file, 0x100) == -1 && errno == EINVAL;
      check(missing && not_executable && bad_mode, "libc: access fails as access() does");
      if (getuid() != 0 && chmod(file, 0444) == 0) {
         errno = 0;
         check(ps5_access(file, W_OK) == -1 && errno == EACCES && ps5_access(file, R_OK) == 0,
               "libc: access asks the file system whether a file can be written");
      }
      unlink(file);
      rmdir(folder);
   }
   char path[64];
   check(ps5_join_path("/app0", "saves", path, sizeof(path)) == 0 && !strcmp(path, "/app0/saves"),
         "libc: a path joined");
   check(ps5_join_path("/app0/", "x", path, sizeof(path)) == 0 && !strcmp(path, "/app0/x"),
         "libc: no doubled slash");
   check(ps5_join_path("/app0", "a-very-long-name-that-does-not-fit-in-the-buffer-at-all", path,
                       16) == -1 &&
            errno == ENAMETOOLONG,
         "libc: too long is refused");
}

static void
test_directories(void)
{
   char root[] = "/tmp/ps5-platform-test-XXXXXX";
   check(mkdtemp(root) != NULL, "dir: a scratch directory");
   char path[256];
   const char *const names[] = {"alpha", "beta", "gamma"};
   for (int i = 0; i < 3; i++) {
      snprintf(path, sizeof(path), "%s/%s", root, names[i]);
      close(open(path, O_CREAT | O_WRONLY, 0644));
   }
   DIR *const stream = ps5_opendir(root);
   check(stream != NULL, "dir: opendir");
   unsigned seen = 0;
   for (struct dirent *entry; stream && (entry = ps5_readdir(stream));)
      for (int i = 0; i < 3; i++)
         seen |= !strcmp(entry->d_name, names[i]) ? 1u << i : 0;
   check(seen == 7, "dir: readdir sees every entry");
   /* The *at family against the stream's descriptor. */
   const int fd = stream ? ps5_dirfd(stream) : -1;
   struct stat status;
   check(ps5_fstatat(fd, "beta", &status, 0) == 0 && S_ISREG(status.st_mode),
         "dir: fstatat relative to the stream's descriptor");
   check(ps5_mkdirat(fd, "sub", 0755) == 0, "dir: mkdirat");
   const int sub = ps5_openat(fd, "sub", O_RDONLY | O_DIRECTORY);
   check(sub >= 0, "dir: openat of the new directory");
   const int file = ps5_openat(sub, "leaf", O_CREAT | O_WRONLY, 0600);
   check(file >= 0, "dir: openat relative to a descriptor openat returned");
   close(file);
   check(ps5_renameat(sub, "leaf", fd, "leaf2") == 0, "dir: renameat across descriptors");
   check(ps5_fchmodat(fd, "leaf2", 0644, 0) == 0 && ps5_fstatat(fd, "leaf2", &status, 0) == 0 &&
            (status.st_mode & 0777) == 0644,
         "dir: fchmodat");
   const struct timespec times[2] = {{1000000000, 0}, {1000000000, 500000000}};
   check(ps5_utimensat(fd, "leaf2", times, 0) == 0 && ps5_fstatat(fd, "leaf2", &status, 0) == 0 &&
            status.st_mtime == 1000000000,
         "dir: utimensat relative to a descriptor");
   const struct timespec omit[2] = {{0, UTIME_OMIT}, {0, 0}};
   check(ps5_utimensat(fd, "leaf2", omit, 0) == -1 && errno == ENOSYS,
         "dir: utimensat refuses to omit one time");
   check(ps5_unlinkat(fd, "leaf2", 0) == 0 && ps5_unlinkat(fd, "sub", AT_REMOVEDIR) == 0,
         "dir: unlinkat of a file and a directory");
   close(sub);
   /* An unknown descriptor is refused, not resolved against a stale path. */
   const int other = open(root, O_RDONLY | O_DIRECTORY);
   check(ps5_fstatat(other, "alpha", &status, 0) == -1 && errno == ENOSYS,
         "dir: a descriptor nobody recorded is refused");
   close(other);
   ps5_rewinddir(stream);
   unsigned again = 0;
   for (struct dirent *entry; stream && (entry = ps5_readdir(stream));)
      again += entry->d_name[0] != '.';
   check(again == 3, "dir: rewinddir reads it again");
   check(ps5_closedir(stream) == 0, "dir: closedir");
   /* The working directory, walked up from inside the scratch tree. */
   char before[512], expected[512], found[512];
   snprintf(path, sizeof(path), "%s/sub2", root);
   mkdir(path, 0755);
   check(getcwd(before, sizeof(before)) != NULL && chdir(path) == 0 &&
             getcwd(expected, sizeof(expected)) != NULL,
         "dir: into a directory to find");
   check(ps5_getcwd(found, sizeof(found)) == found && !strcmp(found, expected),
         "dir: getcwd walks up to the same path getcwd gives");
   char *allocated = ps5_getcwd(NULL, 0);
   check(allocated && !strcmp(allocated, expected), "dir: getcwd allocates when given no buffer");
   free(allocated);
   check(ps5_getcwd(found, 4) == NULL && errno == ERANGE, "dir: getcwd reports a short buffer");
   check(chdir("/") == 0 && ps5_getcwd(found, sizeof(found)) && !strcmp(found, "/"),
         "dir: getcwd of the root");
   check(chdir(before) == 0, "dir: back to where the test started");
   rmdir(path);
   for (int i = 0; i < 3; i++) {
      snprintf(path, sizeof(path), "%s/%s", root, names[i]);
      unlink(path);
   }
   rmdir(root);
}

/* The file probe (src/probe_files.c) in a directory of its own: every pass
 * reads back what it wrote, and nothing is left behind. The thread probe
 * (src/probe_threads.c) reports both threads' stacks. */
struct probe_lines {
   unsigned passes;
   unsigned lines;
};

static void
probe_line(void *context, const char *line)
{
   struct probe_lines *const seen = context;
   seen->lines++;
   if (strstr(line, "check PASS") != NULL)
      seen->passes++;
}

static void
test_probe_files(void)
{
   char directory[] = "/tmp/ps5-platform-files-XXXXXX";
   check(mkdtemp(directory) != NULL, "files: a directory of its own");
   struct probe_lines seen = {0};
   check(ps5_platform_probe_files(probe_line, &seen, directory) == 0,
         "files: the probe reports no failure");
   check(seen.passes == 6, "files: every chunk size and stdio buffer reads back what it wrote");
   check(rmdir(directory) == 0, "files: the probe leaves its directory empty");
}

static void
test_probe_threads(void)
{
   struct probe_lines seen = {0};
   check(ps5_platform_probe_threads(probe_line, &seen) == 0, "threads: the probe reports no failure");
   check(seen.passes == 2, "threads: both threads report their stacks");
}


/* The POSIX gaps (src/posix.c) and regular expressions (src/regex.c). */
static int
compare_offset(void *thunk, const void *a, const void *b)
{
   const int offset = *(const int *)thunk;
   return (*(const int *)a + offset) % 10 - (*(const int *)b + offset) % 10;
}

static int
compare_nested(void *thunk, const void *a, const void *b)
{
   /* A comparator that sorts again must get its own thunk back. */
   int inner[3] = {3, 1, 2};
   int zero = 0;
   ps5_qsort_r(inner, 3, sizeof(int), &zero, compare_offset);
   (*(int *)thunk)++;
   return *(const int *)a - *(const int *)b;
}

static void *
report_stack(void *out)
{
   pthread_attr_t running;
   size_t size = 0;
   if (pthread_getattr_np(pthread_self(), &running) == 0) {
      pthread_attr_getstacksize(&running, &size);
      pthread_attr_destroy(&running);
   }
   *(size_t *)out = size;
   return NULL;
}

static int destructor_order[4];
static int destructor_count;

static void
record_destructor(void *object)
{
   destructor_order[destructor_count++] = *(int *)object;
}

static void *
register_destructors(void *unused)
{
   (void)unused;
   static int first = 1, second = 2;
   ps5___cxa_thread_atexit_impl(record_destructor, &first, NULL);
   ps5___cxa_thread_atexit_impl(record_destructor, &second, NULL);
   return NULL;
}

/* src/cxa.c: a thread's thread_local destructors, last registered first. */
static void
test_thread_destructors(void)
{
   pthread_t thread;
   check(pthread_create(&thread, NULL, register_destructors, NULL) == 0 && pthread_join(thread, NULL) == 0 &&
            destructor_count == 2 && destructor_order[0] == 2 && destructor_order[1] == 1,
         "thread_local destructors run at thread exit, last first");
}

/* src/threads.c, linked with --wrap=pthread_create as consumers link it. */
static int deep_done, deep_release;

/* Uses 1.5 MiB of its stack, waits until released, then counts itself. */
static void *
deep_stack(void *unused)
{
   (void)unused;
   volatile unsigned char frame[1536 * 1024];
   for (size_t i = 0; i < sizeof(frame); i += 4096)
      frame[i] = (unsigned char)i;
   while (!__atomic_load_n(&deep_release, __ATOMIC_ACQUIRE))
      usleep(1000);
   __atomic_fetch_add(&deep_done, 1, __ATOMIC_ACQ_REL);
   return (void *)(uintptr_t)frame[4096];
}

static unsigned
read_mxcsr(void)
{
   unsigned mxcsr;
   __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
   return mxcsr;
}

static void
write_mxcsr(unsigned mxcsr)
{
   __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}

static void *
report_mxcsr(void *out)
{
   *(unsigned *)out = read_mxcsr();
   return NULL;
}

static void
test_fp_environment(void)
{
   /* The console's start: flush-to-zero and denormals-are-zero. */
   write_mxcsr(0x9fe0);
   ps5_fp_ieee();
   const unsigned ieee = read_mxcsr() & ~0x3fu; /* without the exception flags */
   check(ieee == 0x1f80, "fp: ps5_fp_ieee sets MXCSR to 0x1f80");
   volatile double tiny = 0x1p-1022;
   check(tiny / 2 != 0.0, "fp: a denormal quotient is kept");

   /* A creator with flush-to-zero set hands it to its threads, whichever way
    * their stacks come. */
   write_mxcsr(0x9fc0);
   unsigned seen = 0;
   pthread_t thread;
   const bool plain = pthread_create(&thread, NULL, report_mxcsr, &seen) == 0 && pthread_join(thread, NULL) == 0;
   check(plain && seen == 0x9fc0, "fp: a thread on the platform's stack starts with its creator's MXCSR");
   pthread_attr_t large;
   pthread_attr_init(&large);
   pthread_attr_setstacksize(&large, (size_t)8 << 20);
   seen = 0;
   const bool own = pthread_create(&thread, &large, report_mxcsr, &seen) == 0 && pthread_join(thread, NULL) == 0;
   pthread_attr_destroy(&large);
   check(own && seen == 0x9fc0, "fp: a thread on a stack of its own starts with its creator's MXCSR");
   ps5_fp_ieee();
}

static void
test_thread_stacks(void)
{
   size_t size = 0;
   pthread_t thread;
   check(pthread_create(&thread, NULL, report_stack, &size) == 0 && pthread_join(thread, NULL) == 0 &&
            size >= PS5_THREAD_STACK_BYTES,
         "a thread created without attributes gets the main thread's stack");
   pthread_attr_t small;
   pthread_attr_init(&small);
   pthread_attr_setstacksize(&small, 65536);
   pthread_attr_setdetachstate(&small, PTHREAD_CREATE_JOINABLE);
   size = 0;
   check(pthread_create(&thread, &small, report_stack, &size) == 0 && pthread_join(thread, NULL) == 0 &&
            size >= PS5_THREAD_STACK_BYTES,
         "a thread asking for 64 KiB gets the main thread's stack");
   pthread_attr_setstacksize(&small, (size_t)8 << 20);
   size = 0;
   check(pthread_create(&thread, &small, report_stack, &size) == 0 && pthread_join(thread, NULL) == 0 &&
            size >= ((size_t)8 << 20),
         "a thread asking for more keeps what it asked for");
   pthread_attr_destroy(&small);

   /* Many threads at once, each using most of its stack, half joined and half
    * detached: every stack comes from direct memory and goes back. */
   enum { THREADS = 96 };
   const long long direct_before = host_direct_allocations();
   pthread_t threads[THREADS];
   bool created = true;
   __atomic_store_n(&deep_done, 0, __ATOMIC_RELEASE);
   __atomic_store_n(&deep_release, 0, __ATOMIC_RELEASE);
   for (int i = 0; i < THREADS; i++)
      created &= pthread_create(&threads[i], NULL, deep_stack, NULL) == 0;
   check(created, "thread stacks: 96 threads start at once");
   unsigned live = 0, cached = 0;
   ps5_thread_stacks(&live, &cached);
   check(live >= THREADS, "thread stacks: each running thread's stack is direct memory");
   __atomic_store_n(&deep_release, 1, __ATOMIC_RELEASE);
   bool joined = true;
   for (int i = 0; i < THREADS; i++) {
      if (i % 2)
         joined &= pthread_detach(threads[i]) == 0;
      else
         joined &= pthread_join(threads[i], NULL) == 0;
   }
   for (int i = 0; i < 400 && __atomic_load_n(&deep_done, __ATOMIC_ACQUIRE) < THREADS; i++)
      usleep(5000);
   for (int i = 0; i < 400 && host_direct_allocations() > direct_before + 32; i++)
      usleep(5000);
   check(joined && __atomic_load_n(&deep_done, __ATOMIC_ACQUIRE) == THREADS,
         "thread stacks: every thread ran 1.5 MiB deep, joined or detached");
   check(host_direct_allocations() <= direct_before + 32,
         "thread stacks: ended threads' stacks are freed, a few kept for reuse");
   pthread_attr_t detached;
   pthread_attr_init(&detached);
   pthread_attr_setdetachstate(&detached, PTHREAD_CREATE_DETACHED);
   pthread_t one;
   check(pthread_create(&one, &detached, deep_stack, NULL) == 0,
         "thread stacks: a thread created detached starts");
   pthread_attr_destroy(&detached);
}

static void
test_posix(void)
{
   int values[6] = {5, 3, 9, 1, 7, 2};
   int offset = 0;
   ps5_qsort_r(values, 6, sizeof(int), &offset, compare_offset);
   check(values[0] == 1 && values[1] == 2 && values[5] == 9, "qsort_r sorts with its thunk");
   int calls = 0;
   int nested[4] = {4, 2, 3, 1};
   ps5_qsort_r(nested, 4, sizeof(int), &calls, compare_nested);
   check(nested[0] == 1 && nested[3] == 4 && calls > 0, "qsort_r nests");

   char path[] = "/tmp/ps5platform-XXXXXX.log";
   const int fd = ps5_mkstemps(path, 4);
   check(fd >= 0 && strstr(path, "XXXXXX") == NULL && strcmp(path + strlen(path) - 4, ".log") == 0,
         "mkstemps names a new file and keeps the suffix");
   struct stat status;
   check(fd >= 0 && fstat(fd, &status) == 0, "mkstemps's file exists");
   if (fd >= 0) {
      close(fd);
      unlink(path);
   }
   char short_template[] = "XXXXX";
   check(ps5_mkstemps(short_template, 0) == -1 && errno == EINVAL, "mkstemps refuses a short template");

   errno = 0;
   check(ps5_popen("true", "r") == NULL && errno == ENOSYS, "popen fails with ENOSYS");
   char *buffer = NULL;
   size_t size = 1;
   FILE *const memory_stream = ps5_open_memstream(&buffer, &size);
   check(memory_stream != NULL && buffer != NULL && size == 0 && buffer[0] == '\0', "open_memstream starts empty");
   if (memory_stream) {
      fputs("hello", memory_stream);
      fflush(memory_stream);
      check(size == 5 && strcmp(buffer, "hello") == 0, "fflush publishes a memory stream's text");
      /* Far more than a pipe holds: the reader drains while the writer writes. */
      enum { lines = 200000 };
      for (int i = 0; i < lines; i++)
         fprintf(memory_stream, "%07d\n", i);
      check(fclose(memory_stream) == 0, "fclose closes a memory stream");
      bool intact = size == 5 + (size_t)lines * 8 && buffer[size] == '\0';
      for (int i = 0; intact && i < lines; i += 997) {
         char line[9];
         snprintf(line, sizeof(line), "%07d\n", i);
         intact = memcmp(buffer + 5 + (size_t)i * 8, line, 8) == 0;
      }
      check(intact, "fclose publishes all 1.6 MB written through a memory stream, in order and terminated");
      free(buffer);
   }

   char names[5][32];
   memset(names, 'x', sizeof(names));
   check(ps5___xuname(32, names) == 0 && strcmp(names[0], "PlayStation") == 0 && strlen(names[3]) < 32,
         "uname fills every field and terminates it");

   char memory[8];
   check(ps5___memset_chk(memory, 7, sizeof(memory), sizeof(memory)) == memory && memory[7] == 7,
         "__memset_chk sets a fitting range");

   void *const c_locale = ps5_newlocale(LC_ALL_MASK, "C", NULL);
   check(c_locale != NULL && ps5_newlocale(LC_ALL_MASK, "de_DE", NULL) == NULL,
         "newlocale gives the C locale only");
   char *number_end = NULL;
   const char *number = "3.25e1x";
   check(ps5_strtod_l(number, &number_end, c_locale) == 32.5 && number_end == number + 6,
         "strtod_l parses with '.'");
   check(ps5_strtof_l("0.5", NULL, c_locale) == 0.5f, "strtof_l parses with '.'");
   host_empty_decimal_point = 1;
   number_end = NULL;
   check(ps5_strtod_l(number, &number_end, c_locale) == 32.5 && number_end == number + 6 &&
            ps5_strtof_l("0.100000001", NULL, c_locale) == 0.100000001f,
         "strtod_l and strtof_l parse with '.' where localeconv's decimal point is empty (the console's)");
   host_empty_decimal_point = 0;
   check(ps5_dladdr((const void *)test_posix, NULL) == 0, "dladdr reports nothing");

   char *end = NULL;
   check(ps5_strtoll_l("-42z", &end, 10, c_locale) == -42 && *end == 'z', "strtoll_l");
   check(strcmp(ps5_nl_langinfo(RADIXCHAR), ".") == 0 && strcmp(ps5_nl_langinfo(THOUSEP), "") == 0 &&
            strcmp(ps5_nl_langinfo(CODESET), "US-ASCII") == 0 &&
            strcmp(ps5_nl_langinfo_l(MON_12, c_locale), "December") == 0 &&
            strcmp(ps5_nl_langinfo(-1), "") == 0,
         "nl_langinfo answers for the C locale");
   char formatted[32];
   check(ps5_snprintf_l(formatted, sizeof(formatted), c_locale, "%d:%.1f", 7, 2.5) == 5 &&
            strcmp(formatted, "7:2.5") == 0,
         "snprintf_l");
   int scanned = 0;
   check(ps5_sscanf_l("x=13", c_locale, "x=%d", &scanned) == 1 && scanned == 13, "sscanf_l");
   const char *narrow = "abc";
   wchar_t wide[8];
   mbstate_t state;
   memset(&state, 0, sizeof(state));
   check(ps5_mbsnrtowcs_l(wide, &narrow, 2, 8, &state, c_locale) == 2 && wide[0] == L'a' &&
            wide[1] == L'b' && narrow != NULL && *narrow == 'c',
         "mbsnrtowcs_l stops at its byte bound");
   const wchar_t *wide_in = L"xyz";
   char bytes[8];
   memset(&state, 0, sizeof(state));
   check(ps5_wcsnrtombs_l(bytes, &wide_in, 4, sizeof(bytes), &state, c_locale) == 3 &&
            memcmp(bytes, "xyz", 4) == 0 && wide_in == NULL,
         "wcsnrtombs_l converts through the terminator");
   check(ps5_catopen("messages", 0) == (void *)-1 &&
            strcmp(ps5_catgets((void *)-1, 1, 1, "own"), "own") == 0,
         "catalogues: none open, catgets gives the caller's string");
   void *frames[8];
   check(ps5_backtrace(frames, 8) > 1, "backtrace walks the calling thread");

   ps5_freelocale(c_locale);

   struct ps5_regex re;
   memset(&re, 0, sizeof(re));
   check(ps5_regcomp(&re, "^deqp-v[kK]$", PS5_REG_EXTENDED | PS5_REG_NOSUB) == 0, "regcomp: an ERE");
   check(ps5_regexec(&re, "deqp-vk", 0, NULL, 0) == 0, "regexec: a match");
   check(ps5_regexec(&re, "deqp-vks", 0, NULL, 0) == PS5_REG_NOMATCH, "regexec: no match");
   ps5_regfree(&re);

   memset(&re, 0, sizeof(re));
   struct ps5_regmatch match[3];
   check(ps5_regcomp(&re, "(a+)(b)c", PS5_REG_EXTENDED) == 0 && re.re_nsub == 2, "regcomp: two groups");
   check(ps5_regexec(&re, "xxaabc", 3, match, 0) == 0 && match[0].rm_so == 2 && match[0].rm_eo == 6 &&
            match[1].rm_so == 2 && match[1].rm_eo == 4 && match[2].rm_so == 4,
         "regexec: the groups' offsets");
   match[0].rm_so = 1;
   match[0].rm_eo = 5;
   check(ps5_regexec(&re, "aaabcz", 3, match, PS5_REG_STARTEND) == 0 && match[0].rm_so == 1 &&
            match[0].rm_eo == 5,
         "regexec: REG_STARTEND offsets stay relative to the string");
   ps5_regfree(&re);

   memset(&re, 0, sizeof(re));
   check(ps5_regcomp(&re, "hello", PS5_REG_ICASE | PS5_REG_NOSUB) == 0 &&
            ps5_regexec(&re, "Say HELLO", 0, NULL, 0) == 0,
         "regcomp: REG_ICASE, a basic expression");
   ps5_regfree(&re);
   memset(&re, 0, sizeof(re));
   check(ps5_regcomp(&re, "a.c", PS5_REG_NOSPEC | PS5_REG_NOSUB) == 0 &&
            ps5_regexec(&re, "abc", 0, NULL, 0) == PS5_REG_NOMATCH &&
            ps5_regexec(&re, "a.c", 0, NULL, 0) == 0,
         "regcomp: REG_NOSPEC is literal");
   ps5_regfree(&re);

   memset(&re, 0, sizeof(re));
   const int bad = ps5_regcomp(&re, "(unclosed", PS5_REG_EXTENDED);
   char message[64];
   check(bad == PS5_REG_EPAREN && ps5_regerror(bad, &re, message, sizeof(message)) > 1 &&
            strcmp(message, "Missing ')'") == 0,
         "regcomp: an unclosed group, and its message");
   ps5_regfree(&re);
}

/* ------------------------------------------------------------ title heap */

void *__wrap_malloc(size_t bytes);
void *__wrap_calloc(size_t count, size_t bytes);
void *__wrap_realloc(void *pointer, size_t bytes);
void *__wrap_reallocf(void *pointer, size_t bytes);
void *__wrap_reallocarray(void *pointer, size_t count, size_t bytes);
void __wrap_free(void *pointer);
int __wrap_posix_memalign(void **out, size_t alignment, size_t bytes);
void *__wrap_aligned_alloc(size_t alignment, size_t bytes);
size_t __wrap_malloc_usable_size(const void *pointer);
ssize_t __wrap_getline(char **line, size_t *capacity, FILE *stream);

static void *
heap_worker(void *opaque)
{
   unsigned seed = (unsigned)(uintptr_t)opaque;
   void *held[64] = {0};
   size_t sizes[64] = {0};
   bool intact = true;
   for (unsigned i = 0; i < 20000; i++) {
      const unsigned slot = (unsigned)rand_r(&seed) % 64;
      if (held[slot]) {
         const unsigned char *bytes = held[slot];
         intact &= bytes[0] == (unsigned char)slot && bytes[sizes[slot] - 1] == (unsigned char)slot;
         __wrap_free(held[slot]);
         held[slot] = NULL;
      } else {
         sizes[slot] = 1 + (size_t)rand_r(&seed) % 70000;
         held[slot] = __wrap_malloc(sizes[slot]);
         intact &= held[slot] != NULL && ps5_heap_owns(held[slot]);
         if (held[slot]) {
            memset(held[slot], (int)slot, sizes[slot]);
         }
      }
   }
   for (unsigned slot = 0; slot < 64; slot++)
      __wrap_free(held[slot]);
   return intact ? opaque : NULL;
}

/* One thread's blocks, for others to resize and free. */
static void **handed_blocks;

static void *
heap_giver(void *opaque)
{
   void **given = opaque;
   handed_blocks = given;
   for (unsigned i = 0; i < 4096; i++) {
      given[i] = __wrap_malloc(16 + i % 300);
      if (given[i])
         ((unsigned char *)given[i])[0] = (unsigned char)i;
   }
   return NULL;
}

static void *
heap_taker(void *opaque)
{
   const unsigned first = (unsigned)(uintptr_t)opaque;
   bool intact = true;
   for (unsigned i = first; i < 4096; i += 4) {
      unsigned char *grown = __wrap_realloc(handed_blocks[i], 1000 + i);
      intact &= grown && grown[0] == (unsigned char)i && ps5_heap_owns(grown);
      __wrap_free(grown);
   }
   return intact ? (void *)1 : NULL;
}

static void
test_heap(void)
{
   /* Small blocks: the heap's, in its range, 32-byte aligned, and one
    * segment of direct memory for all of them. */
   const long long direct_before = host_direct_allocations();
   char *small = __wrap_malloc(24);
   struct ps5_heap_stats stats;
   ps5_heap_stats(&stats);
   check(small && ps5_heap_owns(small), "heap: a small block is the heap's");
   check(stats.range_base >= 0x300000000ull && stats.range_bytes == PS5_HEAP_RANGE,
         "heap: the whole range is reserved outside the GPU window");
   check((uintptr_t)small % 32 == 0, "heap: blocks are 32-byte aligned");
   check(stats.segments == 1 && host_direct_allocations() == direct_before + 1,
         "heap: the first block maps one segment");
   strcpy(small, "title heap");
   char *grown = __wrap_realloc(small, 100000);
   check(grown && ps5_heap_owns(grown) && strcmp(grown, "title heap") == 0,
         "heap: realloc keeps the contents");
   check(__wrap_malloc_usable_size(grown) >= 100000, "heap: usable size covers the request");
   char *zero = __wrap_realloc(grown, 0);
   check(zero != NULL && ps5_heap_owns(zero), "heap: realloc to zero bytes gives a minimum object");
   __wrap_free(zero);
   int *cleared = __wrap_calloc(1000, sizeof(int));
   bool all_zero = cleared != NULL;
   for (int i = 0; cleared && i < 1000; i++)
      all_zero &= cleared[i] == 0;
   check(all_zero && ps5_heap_owns(cleared), "heap: calloc clears");
   __wrap_free(cleared);
   check(__wrap_reallocarray(NULL, SIZE_MAX / 2, 4) == NULL && errno == ENOMEM,
         "heap: reallocarray refuses an overflowing count");

   /* Alignment. */
   bool aligned = true;
   for (size_t alignment = 8; alignment <= 0x10000; alignment *= 2) {
      void *pointer = NULL;
      aligned &= __wrap_posix_memalign(&pointer, alignment, 100) == 0 && pointer &&
                 (uintptr_t)pointer % alignment == 0 && ps5_heap_owns(pointer);
      __wrap_free(pointer);
      void *c11 = __wrap_aligned_alloc(alignment, alignment * 2);
      aligned &= c11 && (uintptr_t)c11 % alignment == 0;
      __wrap_free(c11);
   }
   void *unaligned = NULL;
   check(aligned, "heap: posix_memalign and aligned_alloc honour 8 B to 64 KiB");
   check(__wrap_posix_memalign(&unaligned, 24, 100) == EINVAL, "heap: a non-power-of-two alignment is EINVAL");

   /* A block past the mapping threshold has a segment of its own, returned
    * with the block. */
   ps5_heap_stats(&stats);
   const unsigned segments = stats.segments;
   const size_t mapped = stats.mapped_bytes;
   unsigned char *large = __wrap_malloc((size_t)100 << 20);
   check(large && ps5_heap_owns(large), "heap: a 100 MiB block is the heap's");
   if (large) {
      large[0] = 1;
      large[((size_t)100 << 20) - 1] = 2;
   }
   ps5_heap_stats(&stats);
   check(stats.segments == segments + 1 && stats.mapped_bytes >= mapped + ((size_t)100 << 20),
         "heap: a large block maps its own segment");
   __wrap_free(large);
   ps5_heap_stats(&stats);
   check(stats.segments == segments && stats.mapped_bytes == mapped,
         "heap: freeing it returns the segment's direct memory");
   check(stats.peak_bytes >= mapped + ((size_t)100 << 20), "heap: the peak remembers it");

   /* libc's blocks stay libc's. */
   char *foreign = strdup("libc's own");
   check(foreign && !ps5_heap_owns(foreign), "heap: a libc block is not the heap's");
   char *foreign_grown = __wrap_realloc(foreign, 4096);
   check(foreign_grown && !ps5_heap_owns(foreign_grown) && strcmp(foreign_grown, "libc's own") == 0,
         "heap: realloc keeps a libc block in libc");
   check(__wrap_malloc_usable_size(foreign_grown) >= 4096, "heap: usable size asks libc for its block");
   __wrap_free(foreign_grown);

   /* When direct memory refuses, libc serves the request. */
   ps5_heap_stats(&stats);
   const unsigned long long fallbacks = stats.libc_fallbacks;
   host_fail(HOST_CALL_ALLOCATE, -1);
   void *refused = __wrap_malloc((size_t)64 << 20);
   host_fail(HOST_CALL_NONE, 0);
   ps5_heap_stats(&stats);
   check(refused && !ps5_heap_owns(refused) && stats.libc_fallbacks == fallbacks + 1,
         "heap: a refused segment falls back to libc");
   __wrap_free(refused);
   char *kept = __wrap_malloc(64);
   check(kept && ps5_heap_owns(kept), "heap: the next request is the heap's again");
   __wrap_free(kept);

   /* getline grows a heap buffer with the heap. */
   char text[] = "first line\nsecond, longer line that has to grow the buffer past its start\n";
   FILE *stream = fmemopen(text, strlen(text), "r");
   char *line = __wrap_malloc(4);
   size_t capacity = 4;
   const ssize_t first = __wrap_getline(&line, &capacity, stream);
   check(first == 11 && strcmp(line, "first line\n") == 0 && ps5_heap_owns(line),
         "heap: getline reads a line into a heap buffer");
   const ssize_t second = __wrap_getline(&line, &capacity, stream);
   check(second == (ssize_t)strlen(text) - 11 && line[second - 1] == '\n',
         "heap: getline grows the buffer");
   check(__wrap_getline(&line, &capacity, stream) == -1, "heap: getline ends at end of file");
   fclose(stream);
   __wrap_free(line);

   /* Threads. */
   pthread_t threads[8];
   for (uintptr_t i = 0; i < 8; i++)
      pthread_create(&threads[i], NULL, heap_worker, (void *)(i + 1));
   bool threads_intact = true;
   for (uintptr_t i = 0; i < 8; i++) {
      void *result = NULL;
      pthread_join(threads[i], &result);
      threads_intact &= result == (void *)(i + 1);
   }
   check(threads_intact, "heap: eight threads allocating and freeing keep every block intact");
   ps5_heap_stats(&stats);
   check(stats.arenas > 1 && stats.arenas <= 8, "heap: allocating threads have arenas of their own");

   /* Blocks one thread allocated, freed and resized by others. */
   enum { handed = 4096 };
   static void *given[handed];
   pthread_t giver;
   pthread_create(&giver, NULL, heap_giver, given);
   pthread_join(giver, NULL);
   bool handed_intact = true;
   for (unsigned i = 0; i < handed; i++)
      handed_intact &= given[i] && ps5_heap_owns(given[i]) && ((unsigned char *)given[i])[0] == (unsigned char)i;
   pthread_t takers[4];
   for (uintptr_t t = 0; t < 4; t++)
      pthread_create(&takers[t], NULL, heap_taker, (void *)t);
   for (uintptr_t t = 0; t < 4; t++) {
      void *result = NULL;
      pthread_join(takers[t], &result);
      handed_intact &= result != NULL;
   }
   check(handed_intact, "heap: blocks go back to their own arena from other threads");
   /* A large calloc is its own mapping (dlmalloc's threshold is 32 MiB),
    * from direct memory that may hold what it held before: cleared all the
    * same, written, freed and asked for again. */
   for (int round = 0; round < 2; round++) {
      uint8_t *const large = __wrap_calloc(1, (size_t)40 << 20);
      check(large && large[0] == 0 && large[((size_t)40 << 20) - 1] == 0 && large[(size_t)17 << 20] == 0,
            "heap: a large calloc reads zero");
      if (large)
         memset(large, 0x77, (size_t)40 << 20);
      __wrap_free(large);
   }
}

static void
test_klog(void)
{
   const int saved = dup(STDERR_FILENO);
   check(ps5_klog_capture_stderr("[t] ") == 0, "klog: standard error is captured");
   check(ps5_klog_capture_stderr("[again] ") == 0, "klog: a second capture does nothing");
   fprintf(stderr, "radv/ps5: first\nsecond ");
   fprintf(stderr, "line\n");
   char text[8192] = "";
   for (int i = 0; i < 200 && !strstr(text, "second line"); i++) {
      usleep(5000);
      host_klog_text(text, sizeof(text));
   }
   check(strstr(text, "[t] radv/ps5: first\n") != NULL && strstr(text, "[t] second line\n") != NULL,
         "klog: each line of standard error is one klog record with the prefix");
   dup2(saved, STDERR_FILENO);
   close(saved);
}

int
main(void)
{
   printf("%s\n", "test_model");
   fflush(stdout);
   test_model();
   printf("%s\n", "test_probe_files");
   fflush(stdout);
   test_probe_files();
   printf("%s\n", "test_probe_threads");
   fflush(stdout);
   test_probe_threads();
   printf("%s\n", "test_exec_anywhere");
   fflush(stdout);
   test_exec_anywhere();
   printf("%s\n", "test_exec_near");
   fflush(stdout);
   test_exec_near();
   test_exec_pointer();
   test_exec_many();
   test_exec_at();
   printf("%s\n", "test_exec_fixed");
   fflush(stdout);
   test_exec_fixed();
   printf("%s\n", "test_exec_dual");
   fflush(stdout);
   test_exec_dual();
   printf("%s\n", "test_exec_toggled");
   fflush(stdout);
   test_exec_toggled();
   printf("%s\n", "test_exec_unwinding");
   fflush(stdout);
   test_exec_unwinding();
   printf("%s\n", "test_exec_threads");
   fflush(stdout);
   test_exec_threads();
   printf("%s\n", "test_shm");
   fflush(stdout);
   test_shm();
   printf("%s\n", "test_vrange_commit");
   test_vrange_commit();
   printf("%s\n", "test_libc_system");
   test_libc_system();
   printf("%s\n", "test_memfd");
   fflush(stdout);
   test_memfd();
   printf("%s\n", "test_posix");
   fflush(stdout);
   test_posix();
   printf("%s\n", "test_thread_destructors");
   fflush(stdout);
   test_thread_destructors();
   printf("%s\n", "test_thread_stacks");
   fflush(stdout);
   test_thread_stacks();
   printf("%s\n", "test_fp_environment");
   fflush(stdout);
   test_fp_environment();
   printf("%s\n", "test_libc");
   fflush(stdout);
   test_libc();
   printf("%s\n", "test_directories");
   fflush(stdout);
   test_directories();
   printf("%s\n", "test_klog");
   fflush(stdout);
   test_klog();
   printf("%s\n", "test_heap");
   fflush(stdout);
   test_heap();
   printf("ps5-platform host tests: %u of %u checks passed\n", checks - failures, checks);
   return failures != 0;
}
