/*
 * PS5 Platform - the console's kernel functions on the build host, for tests.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Direct memory is a sparse memfd; its allocations come from a first-fit list
 * of 64 KiB units. The console behaviour the library depends on is modelled as
 * measured (docs/PROBE.md): execute asked for at map time is refused, and a
 * mapping asked for with no address lands in the GPU window. Any call can be
 * made to fail on its nth use (host_fail), which is how the tests check that
 * every failure unwinds. getdents returns the console's FreeBSD records,
 * converted from Linux's.
 */
#define _GNU_SOURCE 1

#include "host_kernel.h"

#include "ps5platform/kernel.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define DIRECT_BYTES ((int64_t)16 << 30)
#define UNIT ((int64_t)0x10000)
#define UNITS (DIRECT_BYTES / UNIT)
#define REFUSED ((int32_t)0x80020016)
#define FAILED ((int32_t)0x8002000c)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int memory_fd = -1;
static unsigned char *used; /* one byte per unit */
static long long allocations;
static int fail_call = HOST_CALL_NONE;
static int fail_countdown;

static void
setup(void)
{
   if (memory_fd >= 0)
      return;
   memory_fd = memfd_create("ps5-direct", 0);
   if (memory_fd < 0 || ftruncate(memory_fd, DIRECT_BYTES) != 0)
      abort();
   used = calloc((size_t)UNITS, 1);
   if (!used)
      abort();
}

void
host_fail(int call, int nth)
{
   pthread_mutex_lock(&lock);
   fail_call = call;
   fail_countdown = nth;
   pthread_mutex_unlock(&lock);
}

static bool
failing(int call)
{
   bool fail = false;
   pthread_mutex_lock(&lock);
   if (fail_call == call && --fail_countdown == 0) {
      fail = true;
      fail_call = HOST_CALL_NONE;
   }
   pthread_mutex_unlock(&lock);
   return fail;
}

long long
host_direct_allocations(void)
{
   pthread_mutex_lock(&lock);
   const long long count = allocations;
   pthread_mutex_unlock(&lock);
   return count;
}

int64_t
sceKernelGetDirectMemorySize(void)
{
   return DIRECT_BYTES;
}

int32_t
sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                              size_t alignment, int memory_type, int64_t *physical_start)
{
   (void)memory_type;
   if (failing(HOST_CALL_ALLOCATE))
      return FAILED;
   if (length == 0 || length % UNIT != 0 || alignment % UNIT != 0 || search_end <= search_start)
      return (int32_t)0x80020016;
   pthread_mutex_lock(&lock);
   setup();
   const int64_t units = (int64_t)length / UNIT;
   const int64_t step = alignment ? (int64_t)alignment / UNIT : 1;
   for (int64_t first = search_start / UNIT; first + units <= search_end / UNIT; first += step) {
      int64_t run = 0;
      while (run < units && !used[first + run])
         run++;
      if (run == units) {
         memset(used + first, 1, (size_t)units);
         allocations++;
         pthread_mutex_unlock(&lock);
         *physical_start = first * UNIT;
         return 0;
      }
   }
   pthread_mutex_unlock(&lock);
   return (int32_t)0x8002000c;
}

int32_t
sceKernelReleaseDirectMemory(int64_t start, size_t length)
{
   pthread_mutex_lock(&lock);
   setup();
   memset(used + start / UNIT, 0, length / (size_t)UNIT);
   allocations--;
   pthread_mutex_unlock(&lock);
   /* The pages' contents go with the allocation, as on the console. */
   fallocate(memory_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, start, (off_t)length);
   return 0;
}

int32_t
sceKernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end, size_t alignment,
                                   int64_t *physical_start, size_t *available)
{
   (void)alignment;
   pthread_mutex_lock(&lock);
   setup();
   int64_t best = 0, best_start = -1;
   for (int64_t unit = search_start / UNIT; unit < search_end / UNIT;) {
      if (used[unit]) {
         unit++;
         continue;
      }
      int64_t run = 0;
      while (unit + run < search_end / UNIT && !used[unit + run])
         run++;
      if (run > best) {
         best = run;
         best_start = unit;
      }
      unit += run;
   }
   pthread_mutex_unlock(&lock);
   *physical_start = best_start * UNIT;
   *available = (size_t)(best * UNIT);
   return 0;
}

static int
posix_protection(int protection)
{
   return (protection & PS5_KERNEL_PROT_CPU_READ ? PROT_READ : 0) |
          (protection & PS5_KERNEL_PROT_CPU_WRITE ? PROT_WRITE : 0) |
          (protection & PS5_KERNEL_PROT_CPU_EXEC ? PROT_EXEC : 0);
}

/* The console's choice for a mapping with no address: the GPU window. */
static void *
unhinted(size_t length)
{
   static uintptr_t next = 0x200020000ull;
   pthread_mutex_lock(&lock);
   void *const at = (void *)next;
   next += (length + 0xffff) & ~(uintptr_t)0xffff;
   pthread_mutex_unlock(&lock);
   return at;
}

/* A mapping at the hint when it is free, otherwise wherever Linux puts it,
 * moved to the alignment the console honours: map more, then trim. */
static void *
aligned_mmap(void *hint, size_t length, int protection, int flags, int fd, off_t offset,
             size_t alignment)
{
   void *mapped = mmap(hint, length, protection, flags, fd, offset);
   if (mapped == MAP_FAILED || (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) || alignment <= 4096 ||
       (uintptr_t)mapped % alignment == 0)
      return mapped;
   munmap(mapped, length);
   void *const wide = mmap(NULL, length + alignment, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
   if (wide == MAP_FAILED)
      return MAP_FAILED;
   const uintptr_t base = ((uintptr_t)wide + alignment - 1) / alignment * alignment;
   mapped = mmap((void *)base, length, protection, flags | MAP_FIXED, fd, offset);
   if ((uintptr_t)wide < base)
      munmap(wide, base - (uintptr_t)wide);
   const uintptr_t end = (uintptr_t)wide + length + alignment;
   if (base + length < end)
      munmap((void *)(base + length), end - (base + length));
   return mapped;
}

int32_t
sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                         int64_t direct_start, size_t alignment)
{
   if (failing(HOST_CALL_MAP))
      return FAILED;
   if (protection & PS5_KERNEL_PROT_CPU_EXEC)
      return REFUSED;
   setup();
   void *const hint = *address ? *address : unhinted(length);
   const int placement = (flags & PS5_KERNEL_MAP_FIXED) ? MAP_FIXED
                         : *address                     ? 0
                                                        : MAP_FIXED_NOREPLACE;
   void *const mapped = aligned_mmap(hint, length, posix_protection(protection),
                                     MAP_SHARED | placement, memory_fd, (off_t)direct_start,
                                     alignment < 0x4000 ? 0x4000 : alignment);
   if (mapped == MAP_FAILED)
      return FAILED;
   *address = mapped;
   return 0;
}

int32_t
sceKernelMprotect(const void *address, size_t length, int protection)
{
   if (failing(HOST_CALL_PROTECT))
      return FAILED;
   return mprotect((void *)address, length, posix_protection(protection)) == 0 ? 0 : FAILED;
}

int32_t
sceKernelReserveVirtualRange(void **address, size_t length, int flags, size_t alignment)
{
   if (failing(HOST_CALL_RESERVE))
      return FAILED;
   void *const hint = *address ? *address : unhinted(length);
   const int placement = (flags & PS5_KERNEL_MAP_FIXED) ? MAP_FIXED
                         : *address                     ? 0
                                                        : MAP_FIXED_NOREPLACE;
   void *const mapped = aligned_mmap(hint, length, PROT_NONE,
                                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | placement, -1, 0,
                                     alignment < 0x4000 ? 0x4000 : alignment);
   if (mapped == MAP_FAILED)
      return FAILED;
   *address = mapped;
   return 0;
}

int32_t
sceKernelMunmap(void *address, size_t length)
{
   return munmap(address, length) == 0 ? 0 : FAILED;
}

int32_t
sceKernelQueryMemoryProtection(void *address, void **start, void **end, uint32_t *protection)
{
   (void)address;
   (void)start;
   (void)end;
   (void)protection;
   return FAILED;
}

int32_t
sceKernelAvailableFlexibleMemorySize(size_t *available)
{
   *available = (size_t)403 << 20;
   return 0;
}

int32_t
sceKernelConfiguredFlexibleMemorySize(size_t *configured)
{
   *configured = (size_t)448 << 20;
   return 0;
}

uint64_t
sceKernelReadTsc(void)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

uint64_t
sceKernelGetTscFrequency(void)
{
   return 1000000000u;
}

/* The console's getdents: FreeBSD records of a 32-bit inode, a 16-bit length,
 * an 8-bit type, an 8-bit name length and the terminated name, 4-byte aligned. */
struct linux_dirent64 {
   uint64_t d_ino;
   int64_t d_off;
   unsigned short d_reclen;
   unsigned char d_type;
   char d_name[];
};

int
getdents(int fd, char *buffer, int bytes)
{
   char linux_buffer[16384];
   const off_t position = lseek(fd, 0, SEEK_CUR);
   const long read = syscall(SYS_getdents64, fd, linux_buffer, sizeof(linux_buffer));
   if (read <= 0)
      return (int)read;
   int used_bytes = 0;
   off_t resume = position;
   for (long at = 0; at < read;) {
      const struct linux_dirent64 *const entry = (const void *)(linux_buffer + at);
      const size_t name = strlen(entry->d_name);
      const int length = (int)((8 + name + 1 + 3) & ~(size_t)3);
      if (used_bytes + length > bytes) {
         /* Give back what did not fit: the next read starts at this entry. */
         lseek(fd, resume, SEEK_SET);
         break;
      }
      char *const record = buffer + used_bytes;
      const uint32_t inode = (uint32_t)entry->d_ino | 1u;
      const uint16_t reclen = (uint16_t)length;
      memcpy(record, &inode, 4);
      memcpy(record + 4, &reclen, 2);
      record[6] = (char)entry->d_type;
      record[7] = (char)name;
      memcpy(record + 8, entry->d_name, name + 1);
      used_bytes += length;
      resume = entry->d_off;
      at += entry->d_reclen;
   }
   return used_bytes;
}
