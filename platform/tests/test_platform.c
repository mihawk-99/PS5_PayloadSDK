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
#include "ps5platform/exec.h"
#include "ps5platform/kernel.h"
#include "ps5platform/libc.h"
#include "ps5platform/platform.h"
#include "ps5platform/shm.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
   return regions == 0 && bytes == 0 && stats.objects == 0 && stats.views == 0 &&
          stats.ranges == 0 && host_direct_allocations() == 0;
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
};

static void *
churn(void *argument)
{
   struct churn *const c = argument;
   for (unsigned i = 0; i < c->cycles; i++) {
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

/* ---- libc --------------------------------------------------------------------- */

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
   for (int i = 0; i < 3; i++) {
      snprintf(path, sizeof(path), "%s/%s", root, names[i]);
      unlink(path);
   }
   rmdir(root);
}

int
main(void)
{
   printf("%s\n", "test_model");
   fflush(stdout);
   test_model();
   printf("%s\n", "test_exec_anywhere");
   fflush(stdout);
   test_exec_anywhere();
   printf("%s\n", "test_exec_near");
   fflush(stdout);
   test_exec_near();
   test_exec_pointer();
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
   printf("%s\n", "test_libc");
   fflush(stdout);
   test_libc();
   printf("%s\n", "test_directories");
   fflush(stdout);
   test_directories();
   printf("ps5-platform host tests: %u of %u checks passed\n", checks - failures, checks);
   return failures != 0;
}
