/*
 * PS5 Platform - the console capability probe (include/ps5platform/probe.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each test states what it establishes, logs what it measured as one
 * "platform-probe:" line of key=value pairs, and releases everything it took.
 * The generated code is hand-assembled x86-64: a function returning an
 * immediate (mov eax, imm32; ret) and, for the fault test, a function that
 * loads every general register with a marker and then reads an address that
 * is reserved but not mapped. Only exported kernel functions are called.
 */
#define _GNU_SOURCE 1

#include "ps5platform/kernel.h"
#include "ps5platform/probe.h"

#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <ucontext.h>

#define KIB ((size_t)1024)
#define MIB (KIB * 1024)
#define GIB (MIB * 1024)

#define PROT_RW (PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE)
#define PROT_RX (PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_EXEC)
#define PROT_RWX (PROT_RW | PS5_KERNEL_PROT_CPU_EXEC)

/* The Vulkan driver's GPU window, which nothing else may occupy. */
#define GPU_WINDOW_LOW ((uintptr_t)0x200000000ull)
#define GPU_WINDOW_HIGH ((uintptr_t)0x300000000ull)
/* Where views asked for without an address are put (above the window). */
#define VIEW_HINT ((uintptr_t)0x400000000ull)

struct probe {
   ps5_probe_log_fn log;
   void *context;
   int failures;
   uint64_t tsc_hz;
};

static void
say(struct probe *p, const char *format, ...)
{
   char line[512];
   va_list arguments;
   va_start(arguments, format);
   int used = snprintf(line, sizeof(line), "platform-probe: ");
   vsnprintf(line + used, sizeof(line) - (size_t)used, format, arguments);
   va_end(arguments);
   p->log(p->context, line);
}

static void
check(struct probe *p, bool passed, const char *what)
{
   if (!passed)
      p->failures++;
   say(p, "check %s %s", passed ? "PASS" : "FAIL", what);
}

static uint64_t
ns_since(const struct probe *p, uint64_t start_tsc)
{
   const uint64_t ticks = sceKernelReadTsc() - start_tsc;
   return p->tsc_hz ? (uint64_t)((__uint128_t)ticks * 1000000000u / p->tsc_hz) : 0;
}

static bool
in_gpu_window(uintptr_t address, size_t bytes)
{
   return address < GPU_WINDOW_HIGH && address + bytes > GPU_WINDOW_LOW;
}

static int32_t
direct_allocate(size_t bytes, int64_t *start)
{
   return sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes,
                                        PS5_KERNEL_DIRECT_ALIGNMENT, PS5_KERNEL_DIRECT_TYPE_CPU,
                                        start);
}

static int32_t
direct_map(void **address, size_t bytes, int protection, int flags, int64_t start)
{
   return sceKernelMapDirectMemory(address, bytes, protection, flags, start,
                                   PS5_KERNEL_DIRECT_ALIGNMENT);
}

/* mov eax, imm32; ret */
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
   uint32_t (*const function)(void) = (uint32_t(*)(void))(uintptr_t)at;
   return function();
}

static size_t
available_flexible(void)
{
   size_t available = 0;
   sceKernelAvailableFlexibleMemorySize(&available);
   return available;
}

static size_t
available_direct(int64_t *start)
{
   size_t available = 0;
   int64_t found = -1;
   sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(),
                                      PS5_KERNEL_DIRECT_ALIGNMENT, &found, &available);
   if (start)
      *start = found;
   return available;
}

/* ---- identity and pool ---------------------------------------------------- */

static void
probe_identity(struct probe *p)
{
   struct ps5_kernel_sw_version version;
   memset(&version, 0, sizeof(version));
   version.size = sizeof(version);
   const int32_t result = sceKernelGetSystemSwVersion(&version);
   version.text[sizeof(version.text) - 1] = '\0';
   say(p, "identity sw_version_result=0x%08x sw_version_text=%s sw_version=0x%08x", (unsigned)result,
       result == 0 ? version.text : "-", version.version);
   uint32_t sdk = 0;
   size_t sdk_bytes = sizeof(sdk);
   const int sysctl_result = sysctlbyname("kern.sdk_version", &sdk, &sdk_bytes, NULL, 0);
   say(p, "identity kern_sdk_version_result=%d kern_sdk_version=0x%08x", sysctl_result, sdk);
   size_t configured = 0;
   const int32_t configured_result = sceKernelConfiguredFlexibleMemorySize(&configured);
   int64_t start = -1;
   const size_t available = available_direct(&start);
   say(p,
       "pool direct_size=%lld direct_available=%zu direct_available_start=0x%llx "
       "flexible_configured_result=0x%08x flexible_configured=%zu flexible_available=%zu "
       "tsc_hz=%llu",
       (long long)sceKernelGetDirectMemorySize(), available, (unsigned long long)start,
       (unsigned)configured_result, configured, available_flexible(),
       (unsigned long long)p->tsc_hz);
}

/* The largest single allocation, to 64 MiB, found by allocating and releasing. */
static void
probe_largest(struct probe *p)
{
   const size_t step = 64 * MIB;
   size_t low = 0;
   size_t high = (size_t)sceKernelGetDirectMemorySize() / step * step;
   unsigned attempts = 0;
   while (high - low > step) {
      const size_t middle = (low + high) / 2 / step * step;
      int64_t start = -1;
      attempts++;
      if (direct_allocate(middle, &start) == 0) {
         sceKernelReleaseDirectMemory(start, middle);
         low = middle;
      } else {
         high = middle;
      }
   }
   /* The last candidate above low. */
   int64_t start = -1;
   if (direct_allocate(high, &start) == 0) {
      sceKernelReleaseDirectMemory(start, high);
      low = high;
   }
   say(p, "pool largest_allocation=%zu attempts=%u", low, attempts);
   check(p, low >= 4 * GIB, "one direct allocation of 4 GiB or more");
}

/* ---- executable direct memory ---------------------------------------------- */

static void
probe_map_exec(struct probe *p)
{
   static const int protections[] = {PROT_RX, PROT_RWX};
   for (unsigned i = 0; i < sizeof(protections) / sizeof(protections[0]); i++) {
      int64_t start = -1;
      if (direct_allocate(64 * KIB, &start) != 0) {
         check(p, false, "a 64 KiB direct allocation for the map-time test");
         return;
      }
      void *address = (void *)VIEW_HINT;
      const int32_t result = direct_map(&address, 64 * KIB, protections[i], 0, start);
      say(p, "exec map_time_protection=0x%x result=0x%08x", protections[i], (unsigned)result);
      if (result == 0)
         sceKernelMunmap(address, 64 * KIB);
      sceKernelReleaseDirectMemory(start, 64 * KIB);
   }
}

static void
probe_mprotect_exec(struct probe *p)
{
   int64_t start = -1;
   void *address = (void *)VIEW_HINT;
   if (direct_allocate(64 * KIB, &start) != 0 || direct_map(&address, 64 * KIB, PROT_RW, 0, start) != 0) {
      check(p, false, "a 64 KiB read-write direct mapping");
      return;
   }
   uint8_t *const code = address;
   write_return(code, 0x12345678u);
   const int32_t rx = sceKernelMprotect(code, 64 * KIB, PROT_RX);
   const uint32_t value = rx == 0 ? call(code) : 0;
   say(p, "exec mprotect_rx_result=0x%08x returned=0x%08x address=0x%llx", (unsigned)rx, value,
       (unsigned long long)(uintptr_t)code);
   check(p, rx == 0 && value == 0x12345678u,
         "read-write direct memory made executable with sceKernelMprotect runs its code");
   /* Read, write and execute at once: code rewritten with no change. */
   const int32_t rwx = sceKernelMprotect(code, 64 * KIB, PROT_RWX);
   uint32_t rewritten = 0;
   if (rwx == 0) {
      write_return(code, 0x9abcdef0u);
      rewritten = call(code);
   }
   say(p, "exec mprotect_rwx_result=0x%08x rewritten_returned=0x%08x", (unsigned)rwx, rewritten);
   sceKernelMunmap(code, 64 * KIB);
   sceKernelReleaseDirectMemory(start, 64 * KIB);
}

/* Placement in reach of an anchor (RIP-relative and 32-bit absolute code),
 * outside the GPU window. The anchor is this module's own code. */
static void
probe_placement(struct probe *p)
{
   const uintptr_t anchor = (uintptr_t)(void *)&probe_placement;
   const uintptr_t granule = 256 * MIB;
   const size_t reserve_bytes = 512 * MIB;
   say(p, "placement anchor=0x%llx anchor_in_gpu_window=%d", (unsigned long long)anchor,
       in_gpu_window(anchor, 1) ? 1 : 0);
   /* Where a mapping asked for with no address lands. */
   {
      int64_t start = -1;
      void *address = NULL;
      if (direct_allocate(64 * KIB, &start) == 0) {
         const int32_t result = direct_map(&address, 64 * KIB, PROT_RW, 0, start);
         say(p, "placement unhinted_result=0x%08x unhinted_address=0x%llx in_gpu_window=%d",
             (unsigned)result, (unsigned long long)(uintptr_t)address,
             result == 0 && in_gpu_window((uintptr_t)address, 64 * KIB) ? 1 : 0);
         if (result == 0)
            sceKernelMunmap(address, 64 * KIB);
         sceKernelReleaseDirectMemory(start, 64 * KIB);
      }
   }
   static const int offsets[] = {1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6};
   unsigned placed = 0;
   for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
      const intptr_t hint = (intptr_t)(anchor / granule * granule) + offsets[i] * (intptr_t)granule;
      if (hint <= 0 || in_gpu_window((uintptr_t)hint, reserve_bytes))
         continue;
      void *reserved = (void *)hint;
      const int32_t reserve = sceKernelReserveVirtualRange(&reserved, reserve_bytes, 0,
                                                           PS5_KERNEL_DIRECT_ALIGNMENT);
      if (reserve != 0) {
         say(p, "placement hint=0x%llx reserve_result=0x%08x", (unsigned long long)hint,
             (unsigned)reserve);
         continue;
      }
      const uintptr_t base = (uintptr_t)reserved;
      const long long distance_low = (long long)base - (long long)anchor;
      const long long distance_high = (long long)(base + reserve_bytes) - (long long)anchor;
      const bool reach = distance_low > -0x7c000000ll && distance_high < 0x7c000000ll;
      /* Executable code mapped at a fixed address inside the reservation. */
      int64_t start = -1;
      void *code = (void *)(base + 64 * MIB);
      int32_t map = -1;
      uint32_t value = 0;
      if (direct_allocate(16 * MIB, &start) == 0) {
         map = direct_map(&code, 16 * MIB, PROT_RW, PS5_KERNEL_MAP_FIXED, start);
         if (map == 0) {
            write_return(code, 0x0badc0deu + i);
            if (sceKernelMprotect(code, 16 * MIB, PROT_RX) == 0)
               value = call(code);
            sceKernelMunmap(code, 16 * MIB);
         }
         sceKernelReleaseDirectMemory(start, 16 * MIB);
      }
      const bool ran = map == 0 && value == 0x0badc0deu + i &&
                       (uintptr_t)code == base + 64 * MIB;
      say(p,
          "placement hint=0x%llx reserved=0x%llx distance=%lld..%lld in_reach=%d in_gpu_window=%d "
          "fixed_map_result=0x%08x at_hint=%d ran=%d",
          (unsigned long long)hint, (unsigned long long)base, distance_low, distance_high,
          reach ? 1 : 0, in_gpu_window(base, reserve_bytes) ? 1 : 0, (unsigned)map,
          base == (uintptr_t)hint ? 1 : 0, ran ? 1 : 0);
      sceKernelMunmap(reserved, reserve_bytes);
      if (ran && reach && !in_gpu_window(base, reserve_bytes))
         placed++;
   }
   check(p, placed > 0,
         "executable direct memory placed at a fixed address within 2 GiB of the anchor, "
         "outside the GPU window");
}

/* ---- rewriting ------------------------------------------------------------- */

static void
probe_rewrite_cycles(struct probe *p)
{
   enum { REGIONS = 64, REGION_BYTES = 64 * 1024, ONE = 5000, ROUNDS = 100 };
   int64_t starts[REGIONS];
   uint8_t *regions[REGIONS];
   unsigned mapped = 0;
   for (; mapped < REGIONS; mapped++) {
      void *address = (void *)VIEW_HINT;
      if (direct_allocate(REGION_BYTES, &starts[mapped]) != 0)
         break;
      if (direct_map(&address, REGION_BYTES, PROT_RW, 0, starts[mapped]) != 0) {
         sceKernelReleaseDirectMemory(starts[mapped], REGION_BYTES);
         break;
      }
      regions[mapped] = address;
   }
   check(p, mapped == REGIONS, "64 direct regions of 64 KiB mapped read-write");
   unsigned wrong = 0;
   unsigned refused = 0;
   uint64_t t = sceKernelReadTsc();
   for (unsigned i = 0; mapped && i < ONE; i++) {
      refused += sceKernelMprotect(regions[0], REGION_BYTES, PROT_RW) != 0;
      write_return(regions[0] + (i % 64) * 64, i);
      refused += sceKernelMprotect(regions[0], REGION_BYTES, PROT_RX) != 0;
      wrong += call(regions[0] + (i % 64) * 64) != i;
   }
   const uint64_t one_ns = ns_since(p, t);
   say(p, "rewrite one_region cycles=%u wrong=%u refused=%u total_ns=%llu", ONE, wrong, refused,
       (unsigned long long)one_ns);
   check(p, mapped && wrong == 0 && refused == 0, "5000 write/execute cycles on one region");
   unsigned many_wrong = 0;
   unsigned many_refused = 0;
   t = sceKernelReadTsc();
   for (unsigned round = 0; round < ROUNDS; round++)
      for (unsigned r = 0; r < mapped; r++) {
         many_refused += sceKernelMprotect(regions[r], REGION_BYTES, PROT_RW) != 0;
         write_return(regions[r] + (round % 64) * 64, round * 1000u + r);
         many_refused += sceKernelMprotect(regions[r], REGION_BYTES, PROT_RX) != 0;
         many_wrong += call(regions[r] + (round % 64) * 64) != round * 1000u + r;
      }
   say(p, "rewrite many_regions regions=%u rounds=%u wrong=%u refused=%u total_ns=%llu", mapped,
       ROUNDS, many_wrong, many_refused, (unsigned long long)ns_since(p, t));
   check(p, mapped == REGIONS && many_wrong == 0 && many_refused == 0,
         "6400 write/execute cycles across 64 regions");
   /* The cost of one protection change, on one page and on 1 MiB. */
   if (mapped) {
      enum { PAIRS = 10000 };
      t = sceKernelReadTsc();
      for (unsigned i = 0; i < PAIRS; i++) {
         sceKernelMprotect(regions[1], PS5_KERNEL_PAGE_SIZE, PROT_RW);
         sceKernelMprotect(regions[1], PS5_KERNEL_PAGE_SIZE, PROT_RX);
      }
      say(p, "rewrite mprotect_page_ns=%llu", (unsigned long long)(ns_since(p, t) / (2 * PAIRS)));
   }
   for (unsigned r = 0; r < mapped; r++) {
      sceKernelMunmap(regions[r], REGION_BYTES);
      sceKernelReleaseDirectMemory(starts[r], REGION_BYTES);
   }
   int64_t start = -1;
   void *address = (void *)VIEW_HINT;
   if (direct_allocate(MIB, &start) == 0) {
      if (direct_map(&address, MIB, PROT_RW, 0, start) == 0) {
         enum { PAIRS = 1000 };
         const uint64_t t2 = sceKernelReadTsc();
         for (unsigned i = 0; i < PAIRS; i++) {
            sceKernelMprotect(address, MIB, PROT_RW);
            sceKernelMprotect(address, MIB, PROT_RX);
         }
         say(p, "rewrite mprotect_mib_ns=%llu", (unsigned long long)(ns_since(p, t2) / (2 * PAIRS)));
         sceKernelMunmap(address, MIB);
      }
      sceKernelReleaseDirectMemory(start, MIB);
   }
}

/* ---- rewriting while other threads execute --------------------------------- */

struct runners {
   const uint8_t *function;
   uint32_t expected;
   atomic_bool stop;
   atomic_ulong calls;
   atomic_ulong wrong;
};

static void *
run(void *argument)
{
   struct runners *const r = argument;
   unsigned long calls = 0, wrong = 0;
   while (!atomic_load_explicit(&r->stop, memory_order_relaxed)) {
      wrong += call(r->function) != r->expected;
      calls++;
   }
   atomic_fetch_add(&r->calls, calls);
   atomic_fetch_add(&r->wrong, wrong);
   return NULL;
}

static void
probe_concurrent(struct probe *p)
{
   enum { THREADS = 4, CYCLES = 3000 };
   /* Per-page toggles: the threads run page 0 while page 2 is rewritten. */
   {
      const size_t bytes = 4 * PS5_KERNEL_PAGE_SIZE;
      int64_t start = -1;
      void *address = (void *)VIEW_HINT;
      if (direct_allocate(PS5_KERNEL_DIRECT_ALIGNMENT, &start) != 0 ||
          direct_map(&address, PS5_KERNEL_DIRECT_ALIGNMENT, PROT_RW, 0, start) != 0) {
         check(p, false, "the concurrent test's region");
         return;
      }
      uint8_t *const code = address;
      write_return(code, 7);
      sceKernelMprotect(code, bytes, PROT_RX);
      struct runners r = {.function = code, .expected = 7};
      pthread_t threads[THREADS];
      unsigned started = 0;
      for (; started < THREADS; started++)
         if (pthread_create(&threads[started], NULL, run, &r) != 0)
            break;
      uint8_t *const page = code + 2 * PS5_KERNEL_PAGE_SIZE;
      unsigned wrong = 0, refused = 0;
      for (unsigned i = 0; i < CYCLES; i++) {
         refused += sceKernelMprotect(page, PS5_KERNEL_PAGE_SIZE, PROT_RW) != 0;
         write_return(page, i);
         refused += sceKernelMprotect(page, PS5_KERNEL_PAGE_SIZE, PROT_RX) != 0;
         wrong += call(page) != i;
      }
      atomic_store(&r.stop, true);
      for (unsigned t = 0; t < started; t++)
         pthread_join(threads[t], NULL);
      say(p, "concurrent page_toggle threads=%u cycles=%u wrong=%u refused=%u thread_calls=%lu "
             "thread_wrong=%lu",
          started, CYCLES, wrong, refused, atomic_load(&r.calls), atomic_load(&r.wrong));
      check(p, started == THREADS && wrong == 0 && refused == 0 && atomic_load(&r.wrong) == 0 &&
                  atomic_load(&r.calls) > 0,
            "a page rewritten with per-page toggles while four threads run its neighbour");
      sceKernelMunmap(code, PS5_KERNEL_DIRECT_ALIGNMENT);
      sceKernelReleaseDirectMemory(start, PS5_KERNEL_DIRECT_ALIGNMENT);
   }
   /* Two views of one allocation: written through a read-write view and run
    * through a read-execute one, with no protection change at all. */
   {
      int64_t start = -1;
      void *writable = (void *)VIEW_HINT;
      void *executable = (void *)VIEW_HINT;
      if (direct_allocate(PS5_KERNEL_DIRECT_ALIGNMENT, &start) != 0 ||
          direct_map(&writable, PS5_KERNEL_DIRECT_ALIGNMENT, PROT_RW, 0, start) != 0 ||
          direct_map(&executable, PS5_KERNEL_DIRECT_ALIGNMENT, PROT_RW, 0, start) != 0) {
         check(p, false, "two views of one allocation");
         return;
      }
      const int32_t rx = sceKernelMprotect(executable, PS5_KERNEL_DIRECT_ALIGNMENT, PROT_RX);
      uint8_t *const w = writable;
      const uint8_t *const x = executable;
      write_return(w, 11);
      struct runners r = {.function = x, .expected = 11};
      pthread_t threads[THREADS];
      unsigned started = 0;
      for (; rx == 0 && started < THREADS; started++)
         if (pthread_create(&threads[started], NULL, run, &r) != 0)
            break;
      unsigned wrong = 0;
      const unsigned cycles = rx == 0 ? 5000u : 0u;
      for (unsigned i = 0; i < cycles; i++) {
         write_return(w + 32 * KIB + (i % 128) * 64, i);
         wrong += call(x + 32 * KIB + (i % 128) * 64) != i;
      }
      atomic_store(&r.stop, true);
      for (unsigned t = 0; t < started; t++)
         pthread_join(threads[t], NULL);
      say(p, "concurrent dual_view rx_result=0x%08x distinct_views=%d threads=%u cycles=%u "
             "wrong=%u thread_calls=%lu thread_wrong=%lu",
          (unsigned)rx, writable != executable ? 1 : 0, started, cycles, wrong,
          atomic_load(&r.calls), atomic_load(&r.wrong));
      check(p, rx == 0 && writable != executable && wrong == 0 && atomic_load(&r.wrong) == 0 &&
                  started == THREADS,
            "code written through a read-write view runs through a read-execute view of the "
            "same allocation, while four threads run it");
      sceKernelMunmap(executable, PS5_KERNEL_DIRECT_ALIGNMENT);
      sceKernelMunmap(writable, PS5_KERNEL_DIRECT_ALIGNMENT);
      sceKernelReleaseDirectMemory(start, PS5_KERNEL_DIRECT_ALIGNMENT);
   }
}

/* ---- faults, the machine context and backpatching --------------------------- */

#define MARKER(index) (UINT64_C(0x5a17000000000000) | ((uint64_t)(index) << 32) | 0x1234u)
enum { MARKERS = 15, SCAN_WORDS = 96, SITES = 200 };
/* The registers the fault function loads, in marker order. */
static const char *const marker_names[MARKERS] = {
   "rax", "rcx", "rdx", "rsi", "rdi", "rbp", "r8",  "r9",
   "r10", "r11", "r12", "r13", "r14", "r15", "rbx"};

static struct {
   uint8_t *write_view;     /* where the code is written */
   const uint8_t *run_view; /* where it runs */
   size_t fault_offset;     /* the faulting instruction, from run_view */
   volatile int signal_number;
   volatile int signal_code;
   volatile uintptr_t reported_address;
   volatile int rip_word; /* from the start of ucontext_t, -1 if never seen */
   volatile int marker_word[MARKERS];
   volatile unsigned patched;
   volatile unsigned unknown;
   sigjmp_buf escape;
} g_fault;

/* The fault function: saves the callee-saved registers, loads every register
 * with its marker (rbx last, with the fault address it then reads through),
 * reads [rbx + 0] with a 6-byte instruction, restores and returns eax. The
 * handler rewrites that instruction as mov eax, imm32 plus a nop. Returns the
 * offset of the faulting instruction. */
static size_t
write_fault_function(uint8_t *at, uintptr_t fault_address)
{
   size_t n = 0;
   static const uint8_t push[] = {0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
   memcpy(at + n, push, sizeof(push));
   n += sizeof(push);
   /* mov r64, imm64 for each marker: REX.W (+B for r8-r15), B8+reg. */
   static const uint8_t encodings[MARKERS][2] = {
      {0x48, 0xb8}, {0x48, 0xb9}, {0x48, 0xba}, {0x48, 0xbe}, {0x48, 0xbf},
      {0x48, 0xbd}, {0x49, 0xb8}, {0x49, 0xb9}, {0x49, 0xba}, {0x49, 0xbb},
      {0x49, 0xbc}, {0x49, 0xbd}, {0x49, 0xbe}, {0x49, 0xbf}, {0x48, 0xbb}};
   for (unsigned m = 0; m < MARKERS; m++) {
      const uint64_t value = m == MARKERS - 1 ? (uint64_t)fault_address : MARKER(m);
      at[n++] = encodings[m][0];
      at[n++] = encodings[m][1];
      memcpy(at + n, &value, 8);
      n += 8;
   }
   const size_t fault = n;
   static const uint8_t load[] = {0x8b, 0x83, 0x00, 0x00, 0x00, 0x00}; /* mov eax, [rbx+0] */
   memcpy(at + n, load, sizeof(load));
   n += sizeof(load);
   static const uint8_t pop[] = {0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5d, 0x5b, 0xc3};
   memcpy(at + n, pop, sizeof(pop));
   return fault;
}

static void
fault_handler(int signal_number, siginfo_t *info, void *opaque)
{
   const uint64_t *const words = (const uint64_t *)opaque;
   g_fault.signal_number = signal_number;
   g_fault.signal_code = info->si_code;
   g_fault.reported_address = (uintptr_t)info->si_addr;
   /* Where each register is, found by its value, from the start of the
    * context; the first fault establishes it. */
   const uintptr_t fault_rip = (uintptr_t)g_fault.run_view + g_fault.fault_offset;
   int rip = -1;
   for (int w = 0; w < SCAN_WORDS; w++) {
      if (words[w] == fault_rip && rip < 0)
         rip = w;
      for (unsigned m = 0; m + 1 < MARKERS; m++)
         if (words[w] == MARKER(m) && g_fault.marker_word[m] < 0)
            g_fault.marker_word[m] = w;
   }
   if (rip < 0) {
      g_fault.unknown++;
      siglongjmp(g_fault.escape, 1);
   }
   g_fault.rip_word = rip;
   /* Backpatch through the writable view: mov eax, imm32; nop. The saved rip
    * still points at the patched instruction, which now runs. */
   uint8_t *const site = g_fault.write_view + g_fault.fault_offset;
   const uint32_t value = 0xfa017000u + g_fault.patched;
   site[0] = 0xb8;
   memcpy(site + 1, &value, 4);
   site[5] = 0x90;
   g_fault.patched++;
}

static void
probe_faults(struct probe *p)
{
   /* The code: two views of one allocation, so the handler writes the site
    * through the writable view while the faulting thread runs the other. */
   int64_t start = -1;
   void *writable = (void *)VIEW_HINT;
   void *executable = (void *)VIEW_HINT;
   const size_t code_bytes = 256 * KIB;
   if (direct_allocate(code_bytes, &start) != 0 ||
       direct_map(&writable, code_bytes, PROT_RW, 0, start) != 0 ||
       direct_map(&executable, code_bytes, PROT_RW, 0, start) != 0 ||
       sceKernelMprotect(executable, code_bytes, PROT_RX) != 0) {
      check(p, false, "the fault test's code views");
      return;
   }
   /* The fault target: a reserved range with nothing mapped, as a fastmem
    * window's unmapped page is, and a mapped page with no access. */
   void *reserved = (void *)(VIEW_HINT + 4 * GIB);
   const int32_t reserve = sceKernelReserveVirtualRange(&reserved, 64 * MIB, 0,
                                                        PS5_KERNEL_DIRECT_ALIGNMENT);
   int64_t guarded_start = -1;
   void *guarded = (void *)VIEW_HINT;
   const bool guard = direct_allocate(PS5_KERNEL_DIRECT_ALIGNMENT, &guarded_start) == 0 &&
                      direct_map(&guarded, PS5_KERNEL_DIRECT_ALIGNMENT, PROT_RW, 0, guarded_start) == 0 &&
                      sceKernelMprotect(guarded, PS5_KERNEL_DIRECT_ALIGNMENT, 0) == 0;
   say(p, "fault reserve_result=0x%08x guard_page=%d", (unsigned)reserve, guard ? 1 : 0);
   memset(&g_fault, 0, sizeof(g_fault));
   g_fault.write_view = writable;
   g_fault.run_view = executable;
   g_fault.rip_word = -1;
   for (unsigned m = 0; m < MARKERS; m++)
      g_fault.marker_word[m] = -1;

   struct sigaction action, old_segv, old_bus;
   memset(&action, 0, sizeof(action));
   action.sa_sigaction = fault_handler;
   action.sa_flags = SA_SIGINFO | SA_NODEFER;
   sigemptyset(&action.sa_mask);
   sigaction(SIGSEGV, &action, &old_segv);
   sigaction(SIGBUS, &action, &old_bus);

   /* One site against the reserved range: the layout, the signal and one
    * backpatch. */
   const uintptr_t unmapped = reserve == 0 ? (uintptr_t)reserved + 0x10000 : 0;
   uint32_t first = 0;
   int escaped = 0;
   if (unmapped) {
      g_fault.fault_offset = write_fault_function(g_fault.write_view, unmapped);
      if (sigsetjmp(g_fault.escape, 1) == 0)
         first = call(g_fault.run_view);
      else
         escaped = 1;
   }
   say(p, "fault reserved signal=%d code=%d reported_matches=%d escaped=%d returned=0x%08x "
          "rip_word=%d",
       g_fault.signal_number, g_fault.signal_code, g_fault.reported_address == unmapped ? 1 : 0,
       escaped, first, g_fault.rip_word);
   {
      char line[400];
      int used = snprintf(line, sizeof(line), "context");
      for (unsigned m = 0; m + 1 < MARKERS; m++)
         used += snprintf(line + used, sizeof(line) - (size_t)used, " %s=%d", marker_names[m],
                          g_fault.marker_word[m]);
      say(p, "fault %s rip=%d mcontext_offset_words=%d", line, g_fault.rip_word,
          (int)(offsetof(ucontext_t, uc_mcontext) / 8));
   }
   check(p, !escaped && first == 0xfa017000u,
         "a read of a reserved, unmapped address faults, and the handler's backpatch runs");

   /* Many sites, alternating the two targets. */
   unsigned good = 0, sites = 0, repeat = 0;
   const uint64_t t = sceKernelReadTsc();
   for (unsigned s = 0; unmapped && s < SITES; s++) {
      const size_t offset = 1024 + (size_t)s * 512;
      if (offset + 512 > code_bytes)
         break;
      const uintptr_t target = (s & 1) && guard ? (uintptr_t)guarded + 64 : unmapped + s * 64;
      const size_t fault = write_fault_function(g_fault.write_view + offset, target);
      g_fault.fault_offset = offset + fault;
      const unsigned before = g_fault.patched;
      uint32_t value = 0;
      sites++;
      if (sigsetjmp(g_fault.escape, 1) == 0) {
         value = call(g_fault.run_view + offset);
         good += value == 0xfa017000u + before;
         /* Once patched, the site runs without faulting. */
         const unsigned patched = g_fault.patched;
         repeat += call(g_fault.run_view + offset) == value && g_fault.patched == patched;
      }
   }
   const uint64_t sites_ns = ns_since(p, t);
   say(p, "fault sites=%u recovered=%u rerun_without_fault=%u unknown=%u total_ns=%llu "
          "last_signal=%d",
       sites, good, repeat, g_fault.unknown, (unsigned long long)sites_ns, g_fault.signal_number);
   check(p, sites == SITES && good == SITES && repeat == SITES,
         "200 fault sites in direct-memory code, each backpatched from the handler");

   sigaction(SIGSEGV, &old_segv, NULL);
   sigaction(SIGBUS, &old_bus, NULL);
   if (guard) {
      sceKernelMunmap(guarded, PS5_KERNEL_DIRECT_ALIGNMENT);
      sceKernelReleaseDirectMemory(guarded_start, PS5_KERNEL_DIRECT_ALIGNMENT);
   }
   if (reserve == 0)
      sceKernelMunmap(reserved, 64 * MIB);
   sceKernelMunmap(executable, code_bytes);
   sceKernelMunmap(writable, code_bytes);
   sceKernelReleaseDirectMemory(start, code_bytes);
}

/* ---- release and reuse ------------------------------------------------------ */

static void
probe_reuse(struct probe *p)
{
   enum { CYCLES = 200 };
   const size_t bytes = 32 * MIB;
   const size_t direct_before = available_direct(NULL);
   const size_t flexible_before = available_flexible();
   unsigned ran = 0, failed = 0;
   for (unsigned i = 0; i < CYCLES; i++) {
      void *reserved = (void *)VIEW_HINT;
      int64_t start = -1;
      if (sceKernelReserveVirtualRange(&reserved, 2 * bytes, 0, PS5_KERNEL_DIRECT_ALIGNMENT) != 0) {
         failed++;
         continue;
      }
      void *code = reserved;
      if (direct_allocate(bytes, &start) == 0) {
         if (direct_map(&code, bytes, PROT_RW, PS5_KERNEL_MAP_FIXED, start) == 0) {
            write_return(code, i);
            if (sceKernelMprotect(code, bytes, PROT_RX) == 0 && call(code) == i)
               ran++;
            else
               failed++;
            sceKernelMunmap(code, bytes);
         } else {
            failed++;
         }
         sceKernelReleaseDirectMemory(start, bytes);
      } else {
         failed++;
      }
      sceKernelMunmap(reserved, 2 * bytes);
   }
   const size_t direct_after = available_direct(NULL);
   const size_t flexible_after = available_flexible();
   say(p, "reuse cycles=%u ran=%u failed=%u direct_before=%zu direct_after=%zu flexible_before=%zu "
          "flexible_after=%zu",
       CYCLES, ran, failed, direct_before, direct_after, flexible_before, flexible_after);
   check(p, ran == CYCLES && direct_after == direct_before && flexible_after == flexible_before,
         "200 reserve/allocate/map/run/release cycles return direct and flexible memory to "
         "baseline");
}

/* ---- 4 GiB of guest memory beside 1 GiB of code ---------------------------- */

static void
probe_large(struct probe *p)
{
   const size_t guest_bytes = 4 * GIB;
   const size_t code_bytes = GIB;
   const size_t flexible_before = available_flexible();
   const size_t direct_before = available_direct(NULL);
   void *guest = (void *)(VIEW_HINT + 8 * GIB);
   int64_t guest_start = -1, code_start = -1;
   const int32_t reserve = sceKernelReserveVirtualRange(&guest, guest_bytes, 0,
                                                        PS5_KERNEL_DIRECT_ALIGNMENT);
   const int32_t guest_allocation = reserve == 0 ? direct_allocate(guest_bytes, &guest_start) : -1;
   void *guest_view = guest;
   const int32_t guest_map = guest_allocation == 0
                                ? direct_map(&guest_view, guest_bytes, PROT_RW, PS5_KERNEL_MAP_FIXED,
                                             guest_start)
                                : -1;
   unsigned guest_ok = 0;
   if (guest_map == 0)
      for (size_t at = 0; at < guest_bytes; at += MIB)
         ((volatile uint8_t *)guest_view)[at] = (uint8_t)(at >> 20);
   void *code = (void *)(VIEW_HINT + 16 * GIB);
   const int32_t code_allocation = direct_allocate(code_bytes, &code_start);
   const int32_t code_map = code_allocation == 0
                               ? direct_map(&code, code_bytes, PROT_RW, 0, code_start)
                               : -1;
   unsigned functions = 0, ran = 0;
   int32_t rx = -1;
   const uint64_t t = sceKernelReadTsc();
   if (code_map == 0) {
      for (size_t at = 0; at < code_bytes; at += MIB, functions++)
         write_return((uint8_t *)code + at, (uint32_t)(at >> 20));
      rx = sceKernelMprotect(code, code_bytes, PROT_RX);
      if (rx == 0)
         for (size_t at = 0; at < code_bytes; at += MIB)
            ran += call((uint8_t *)code + at) == (uint32_t)(at >> 20);
   }
   if (guest_map == 0)
      for (size_t at = 0; at < guest_bytes; at += MIB)
         guest_ok += ((volatile uint8_t *)guest_view)[at] == (uint8_t)(at >> 20);
   const size_t flexible_during = available_flexible();
   say(p,
       "large guest_reserve=0x%08x guest_allocation=0x%08x guest_map=0x%08x guest_pages_ok=%u "
       "code_allocation=0x%08x code_map=0x%08x code_rx=0x%08x functions=%u ran=%u ns=%llu "
       "flexible_before=%zu flexible_during=%zu",
       (unsigned)reserve, (unsigned)guest_allocation, (unsigned)guest_map, guest_ok,
       (unsigned)code_allocation, (unsigned)code_map, (unsigned)rx, functions, ran,
       (unsigned long long)ns_since(p, t), flexible_before, flexible_during);
   check(p, guest_ok == guest_bytes / MIB && ran == code_bytes / MIB && functions == ran,
         "1 GiB of executable direct memory runs beside 4 GiB of guest memory");
   check(p, flexible_during == flexible_before,
         "neither the guest memory nor the code is charged to flexible memory");
   if (code_map == 0)
      sceKernelMunmap(code, code_bytes);
   if (code_allocation == 0)
      sceKernelReleaseDirectMemory(code_start, code_bytes);
   if (reserve == 0)
      sceKernelMunmap(guest, guest_bytes);
   if (guest_allocation == 0)
      sceKernelReleaseDirectMemory(guest_start, guest_bytes);
   say(p, "large direct_before=%zu direct_after=%zu", direct_before, available_direct(NULL));
}

/* ---- 10 GiB in use at once --------------------------------------------------- */

/* Every 64-bit word gets a value derived from its offset and the seed, so a
 * page mapped twice, dropped or aliased reads back wrong. */
static uint64_t
pattern(size_t offset, uint64_t seed)
{
   uint64_t z = (uint64_t)offset + seed;
   z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
   return z ^ (z >> 27);
}

static void
fill(uint8_t *base, size_t bytes, size_t first_offset, uint64_t seed)
{
   uint64_t *const words = (uint64_t *)base;
   for (size_t i = 0; i < bytes / 8; i++)
      words[i] = pattern(first_offset + i * 8, seed);
}

static size_t
count_wrong(const uint8_t *base, size_t bytes, size_t first_offset, uint64_t seed)
{
   const uint64_t *const words = (const uint64_t *)base;
   size_t wrong = 0;
   for (size_t i = 0; i < bytes / 8; i++)
      wrong += words[i] != pattern(first_offset + i * 8, seed);
   return wrong;
}

#define HUGE_BYTES (10 * GIB)
#define HUGE_PIECES 10

/* Where 10 GiB of virtual space can be had. For each hint, the largest range
 * sceKernelReserveVirtualRange grants there, in whole GiB up to 16, and where
 * it put it; each reservation is released at once. A zero hint is the
 * kernel's own choice. */
static const uintptr_t survey_hints[] = {
   0,
   VIEW_HINT,
   VIEW_HINT + 8 * GIB,
   0x800000000ull,
   0x1000000000ull,
   0x2000000000ull,
   0x4000000000ull,
   0x8000000000ull,
   0x10000000000ull,
   0x40000000000ull,
   0x100000000000ull,
   0x400000000000ull,
};
#define SURVEY_HINTS (sizeof(survey_hints) / sizeof(survey_hints[0]))

static uintptr_t
probe_huge_survey(struct probe *p)
{
   uintptr_t best = 0;
   size_t best_bytes = 0;
   for (unsigned i = 0; i < SURVEY_HINTS; i++) {
      size_t largest = 0;
      uintptr_t where = 0;
      int32_t refusal = 0;
      for (size_t gib = 16; gib >= 1 && largest == 0; gib--) {
         void *at = (void *)survey_hints[i];
         const int32_t result = sceKernelReserveVirtualRange(&at, gib * GIB, 0,
                                                             PS5_KERNEL_DIRECT_ALIGNMENT);
         if (result == 0) {
            largest = gib * GIB;
            where = (uintptr_t)at;
            sceKernelMunmap(at, largest);
         } else if (refusal == 0) {
            refusal = result;
         }
      }
      say(p, "huge survey hint=0x%llx largest=%zu at=0x%llx in_gpu_window=%u refusal=0x%08x",
          (unsigned long long)survey_hints[i], largest, (unsigned long long)where,
          (unsigned)(largest && in_gpu_window(where, largest)), (unsigned)refusal);
      if (survey_hints[i] != 0 && largest > best_bytes && !in_gpu_window(where, largest)) {
         best_bytes = largest;
         best = where;
      }
   }
   say(p, "huge survey best=0x%llx best_bytes=%zu", (unsigned long long)best, best_bytes);
   return best_bytes >= HUGE_BYTES ? best : 0;
}

/* One 10 GiB allocation mapped read-write in one view, at the surveyed place
 * (or the view area when none held 10 GiB), with no reservation first. */
static void
probe_huge_single(struct probe *p, uintptr_t hint)
{
   const size_t flexible_before = available_flexible();
   const size_t direct_before = available_direct(NULL);
   void *view = (void *)(hint ? hint : VIEW_HINT + 8 * GIB);
   int64_t start = -1;
   const int32_t allocation = direct_allocate(HUGE_BYTES, &start);
   const int32_t map = allocation == 0 ? direct_map(&view, HUGE_BYTES, PROT_RW, 0, start) : -1;
   size_t wrong = HUGE_BYTES / 8;
   uint64_t fill_ns = 0, verify_ns = 0;
   size_t direct_during = 0, flexible_during = 0;
   if (map == 0) {
      uint64_t t = sceKernelReadTsc();
      fill(view, HUGE_BYTES, 0, 0x5a5a);
      fill_ns = ns_since(p, t);
      direct_during = available_direct(NULL);
      flexible_during = available_flexible();
      t = sceKernelReadTsc();
      wrong = count_wrong(view, HUGE_BYTES, 0, 0x5a5a);
      verify_ns = ns_since(p, t);
   }
   say(p,
       "huge single bytes=%zu hint=0x%llx allocation=0x%08x map=0x%08x view=%p in_gpu_window=%u "
       "wrong_words=%zu fill_ns=%llu verify_ns=%llu direct_before=%zu direct_during=%zu "
       "flexible_before=%zu flexible_during=%zu",
       (size_t)HUGE_BYTES, (unsigned long long)hint, (unsigned)allocation, (unsigned)map,
       map == 0 ? view : NULL, (unsigned)(map == 0 && in_gpu_window((uintptr_t)view, HUGE_BYTES)),
       wrong, (unsigned long long)fill_ns, (unsigned long long)verify_ns, direct_before,
       direct_during, flexible_before, flexible_during);
   check(p, map == 0 && wrong == 0,
         "one 10 GiB direct allocation is mapped and every word of it written and read back");
   check(p, map == 0 && flexible_during == flexible_before &&
               direct_before - direct_during >= HUGE_BYTES,
         "the 10 GiB comes out of direct memory (the largest free block shrinks by at least "
         "10 GiB) and flexible memory is unchanged");
   if (map == 0)
      sceKernelMunmap(view, HUGE_BYTES);
   if (allocation == 0)
      sceKernelReleaseDirectMemory(start, HUGE_BYTES);
   const size_t direct_after = available_direct(NULL);
   say(p, "huge single direct_after=%zu", direct_after);
   check(p, direct_after == direct_before, "releasing the 10 GiB returns direct memory to baseline");
}

/* Ten 1 GiB allocations, each mapped at the next GiB after the last, with no
 * reservation: 10 GiB in use at once whether or not the views are adjacent. */
static void
probe_huge_pieces(struct probe *p, uintptr_t hint)
{
   const size_t piece = HUGE_BYTES / HUGE_PIECES;
   const size_t flexible_before = available_flexible();
   const size_t direct_before = available_direct(NULL);
   const uintptr_t base = hint ? hint : VIEW_HINT + 8 * GIB;
   int64_t starts[HUGE_PIECES];
   void *views[HUGE_PIECES];
   unsigned allocated = 0, mapped = 0, adjacent = 0, in_window = 0;
   int32_t first_error = 0;
   for (unsigned i = 0; i < HUGE_PIECES; i++) {
      int32_t result = direct_allocate(piece, &starts[i]);
      if (result == 0) {
         allocated++;
         views[i] = (void *)(base + i * piece);
         result = direct_map(&views[i], piece, PROT_RW, 0, starts[i]);
         if (result == 0) {
            mapped++;
            adjacent += (uintptr_t)views[i] == base + i * piece;
            in_window += in_gpu_window((uintptr_t)views[i], piece);
         }
      }
      if (result != 0) {
         first_error = result;
         break;
      }
   }
   size_t wrong = HUGE_BYTES / 8;
   uint64_t fill_ns = 0, verify_ns = 0;
   size_t flexible_during = 0;
   if (mapped == HUGE_PIECES) {
      uint64_t t = sceKernelReadTsc();
      for (unsigned i = 0; i < HUGE_PIECES; i++)
         fill(views[i], piece, i * piece, 0xa5a5);
      fill_ns = ns_since(p, t);
      flexible_during = available_flexible();
      t = sceKernelReadTsc();
      wrong = 0;
      for (unsigned i = 0; i < HUGE_PIECES; i++)
         wrong += count_wrong(views[i], piece, i * piece, 0xa5a5);
      verify_ns = ns_since(p, t);
   }
   say(p,
       "huge pieces pieces=%u piece_bytes=%zu base=0x%llx allocated=%u mapped=%u adjacent=%u "
       "in_gpu_window=%u first_error=0x%08x wrong_words=%zu fill_ns=%llu verify_ns=%llu "
       "flexible_before=%zu flexible_during=%zu",
       HUGE_PIECES, piece, (unsigned long long)base, allocated, mapped, adjacent, in_window,
       (unsigned)first_error, wrong, (unsigned long long)fill_ns, (unsigned long long)verify_ns,
       flexible_before, flexible_during);
   check(p, mapped == HUGE_PIECES && wrong == 0,
         "ten 1 GiB direct allocations are mapped at once, every word written and read back");
   check(p, mapped == HUGE_PIECES && flexible_during == flexible_before,
         "the ten pieces are not charged to flexible memory");
   for (unsigned i = 0; i < mapped; i++)
      sceKernelMunmap(views[i], piece);
   for (unsigned i = 0; i < allocated; i++)
      sceKernelReleaseDirectMemory(starts[i], piece);
   const size_t direct_after = available_direct(NULL);
   say(p, "huge pieces direct_after=%zu", direct_after);
   check(p, direct_after == direct_before,
         "releasing the ten pieces returns direct memory to baseline");
}

/* ---- the JIT interface ------------------------------------------------------ */

static void
probe_jit_api(struct probe *p)
{
   int fd = -1, alias = -1;
   const int32_t create = sceKernelJitCreateSharedMemory("ps5-platform-probe", 64 * KIB, PROT_RWX, &fd);
   int32_t alias_result = -1, map_result = -1;
   void *address = NULL;
   if (create == 0) {
      alias_result = sceKernelJitCreateAliasOfSharedMemory(fd, PROT_RW, &alias);
      map_result = sceKernelJitMapSharedMemory(fd, PROT_RX, &address);
   }
   say(p, "jit create=0x%08x alias=0x%08x map=0x%08x", (unsigned)create, (unsigned)alias_result,
       (unsigned)map_result);
}

int
ps5_platform_probe(ps5_probe_log_fn log, void *context, unsigned flags)
{
   struct probe p = {.log = log, .context = context, .tsc_hz = sceKernelGetTscFrequency()};
   const size_t flexible_start = available_flexible();
   const size_t direct_start = available_direct(NULL);
   say(&p, "begin flags=0x%x", flags);
   probe_identity(&p);
   probe_map_exec(&p);
   probe_mprotect_exec(&p);
   probe_largest(&p);
   probe_placement(&p);
   probe_rewrite_cycles(&p);
   probe_concurrent(&p);
   probe_faults(&p);
   probe_reuse(&p);
   if (flags & PS5_PROBE_LARGE)
      probe_large(&p);
   if (flags & PS5_PROBE_HUGE) {
      const uintptr_t place = probe_huge_survey(&p);
      probe_huge_single(&p, place);
      probe_huge_pieces(&p, place);
   }
   const size_t flexible_end = available_flexible();
   const size_t direct_end = available_direct(NULL);
   check(&p, flexible_end == flexible_start && direct_end == direct_start,
         "the probe leaves direct and flexible memory as it found them");
   say(&p, "end failures=%d flexible_start=%zu flexible_end=%zu direct_start=%zu direct_end=%zu",
       p.failures, flexible_start, flexible_end, direct_start, direct_end);
   /* Last and optional: a refused import would stop the process here. */
   if (flags & PS5_PROBE_JIT_API)
      probe_jit_api(&p);
   return p.failures;
}
