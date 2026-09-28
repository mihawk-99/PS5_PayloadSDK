/*
 * PS5 Platform - the thread probe.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What stack a thread gets: the size a fresh attribute object reports, the
 * stack the calling thread runs on, and the stack a thread created with no
 * attributes and one created asking for 2 MiB actually run on, each read back
 * with pthread_attr_get_np() from inside the thread. A frontend's own threads
 * (RetroArch's rthreads) are created with no attributes, and a tester's title
 * faulted in a frame of about 133 KB on one of them (the RetroArch title's
 * PHASE_LOG, 2026-09-26). Only libc and libkernel's pthread calls are used, so
 * the host tests run it too.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* the host tests: glibc's pthread_getattr_np */
#endif
#include "ps5platform/probe.h"

#include <pthread.h>
#include <sched.h>
#if !defined(__linux__)
#include <pthread_np.h>
#include <sys/cpuset.h>

#include "ps5platform/kernel.h"
#endif
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define THREAD_PROBE_ASKED (2u * 1024u * 1024u)

struct thread_probe {
   ps5_probe_log_fn log;
   void *context;
   int failures;
};

static void
thread_say(struct thread_probe *p, const char *format, ...)
{
   char line[512];
   va_list arguments;
   va_start(arguments, format);
   int used = snprintf(line, sizeof(line), "platform-probe: threads ");
   vsnprintf(line + used, sizeof(line) - (size_t)used, format, arguments);
   va_end(arguments);
   p->log(p->context, line);
}

/* The calling thread's stack as the kernel describes it, and how far below
 * its top the caller's frame already is. */
struct thread_stack {
   int result;
   uintptr_t base;
   size_t size;
   size_t used;
};

static struct thread_stack
thread_stack_of_self(void)
{
   struct thread_stack stack = {0};
   pthread_attr_t attributes;
   void *base = NULL;
#if defined(__linux__)
   stack.result = pthread_getattr_np(pthread_self(), &attributes);
   if (stack.result != 0)
      return stack;
#else
   /* FreeBSD's call fills an initialised attribute object. */
   if (pthread_attr_init(&attributes) != 0) {
      stack.result = -1;
      return stack;
   }
   stack.result = pthread_attr_get_np(pthread_self(), &attributes);
#endif
   if (stack.result == 0)
      stack.result = pthread_attr_getstack(&attributes, &base, &stack.size);
   pthread_attr_destroy(&attributes);
   const uintptr_t here = (uintptr_t)&stack;
   stack.base = (uintptr_t)base;
   if (stack.result == 0 && here >= stack.base && here < stack.base + stack.size)
      stack.used = stack.base + stack.size - here;
   return stack;
}

static void *
thread_probe_body(void *out)
{
   *(struct thread_stack *)out = thread_stack_of_self();
   return NULL;
}

static void
thread_report(struct thread_probe *p, const char *label, const struct thread_stack *stack)
{
   if (stack->result != 0) {
      thread_say(p, "%s stack unknown (pthread_attr_get_np or getstack %d)", label, stack->result);
      return;
   }
   thread_say(p, "%s stack=%zu bytes (%zu KiB) base=%#llx in_use=%zu", label, stack->size,
              stack->size / 1024u, (unsigned long long)stack->base, stack->used);
}

/* A thread created with `attributes` (NULL for none) reports its own stack. */
static int
thread_spawn(struct thread_probe *p, const char *label, const pthread_attr_t *attributes,
             struct thread_stack *stack)
{
   pthread_t thread;
   const int created = pthread_create(&thread, attributes, thread_probe_body, stack);
   if (created != 0) {
      thread_say(p, "%s pthread_create=%d", label, created);
      return created;
   }
   pthread_join(thread, NULL);
   thread_report(p, label, stack);
   return 0;
}

/* The hand-off: how long one thread takes to wake another and be woken back,
 * through a condition variable (what an emulator's CPU and GPU threads use to
 * pass a frame between them) and through a flag both spin on. */
#define THREAD_PING_ROUNDS 2000u

struct ping {
   pthread_mutex_t lock;
   pthread_cond_t changed;
   unsigned turn; /* even: the probe's, odd: the partner's */
   volatile unsigned spin_turn;
   bool spin;
};

static void *
ping_partner(void *opaque)
{
   struct ping *const ping = opaque;
   for (unsigned round = 0; round < THREAD_PING_ROUNDS; round++) {
      const unsigned mine = 2u * round + 1u;
      if (ping->spin) {
         while (__atomic_load_n(&ping->spin_turn, __ATOMIC_ACQUIRE) != mine)
            ;
         __atomic_store_n(&ping->spin_turn, mine + 1u, __ATOMIC_RELEASE);
         continue;
      }
      pthread_mutex_lock(&ping->lock);
      while (ping->turn != mine)
         pthread_cond_wait(&ping->changed, &ping->lock);
      ping->turn = mine + 1u;
      pthread_cond_signal(&ping->changed);
      pthread_mutex_unlock(&ping->lock);
   }
   return NULL;
}

static double
thread_now_us(void)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (double)now.tv_sec * 1e6 + (double)now.tv_nsec / 1e3;
}

/* Round trips in microseconds: the mean, and the share over 100 us. */
static void
thread_ping(struct thread_probe *p, bool spin)
{
   static struct ping ping;
   memset(&ping, 0, sizeof(ping));
   pthread_mutex_init(&ping.lock, NULL);
   pthread_cond_init(&ping.changed, NULL);
   ping.spin = spin;
   pthread_t partner;
   pthread_attr_t attributes;
   pthread_attr_init(&attributes);
   pthread_attr_setstacksize(&attributes, 256u * 1024u);
   const int created = pthread_create(&partner, &attributes, ping_partner, &ping);
   pthread_attr_destroy(&attributes);
   if (created != 0) {
      thread_say(p, "hand-off %s: pthread_create=%d", spin ? "spin" : "condvar", created);
      return;
   }
   double total = 0.0, worst = 0.0;
   unsigned slow = 0;
   for (unsigned round = 0; round < THREAD_PING_ROUNDS; round++) {
      const unsigned mine = 2u * round;
      const double start = thread_now_us();
      if (spin) {
         __atomic_store_n(&ping.spin_turn, mine + 1u, __ATOMIC_RELEASE);
         while (__atomic_load_n(&ping.spin_turn, __ATOMIC_ACQUIRE) != mine + 2u)
            ;
      } else {
         pthread_mutex_lock(&ping.lock);
         ping.turn = mine + 1u;
         pthread_cond_signal(&ping.changed);
         while (ping.turn != mine + 2u)
            pthread_cond_wait(&ping.changed, &ping.lock);
         pthread_mutex_unlock(&ping.lock);
      }
      const double took = thread_now_us() - start;
      total += took;
      worst = took > worst ? took : worst;
      slow += took > 100.0;
   }
   pthread_join(partner, NULL);
   pthread_cond_destroy(&ping.changed);
   pthread_mutex_destroy(&ping.lock);
   thread_say(p, "hand-off %s: %u round trips, mean %.1f us, worst %.1f us, %u over 100 us",
              spin ? "spin" : "condvar", THREAD_PING_ROUNDS, total / THREAD_PING_ROUNDS, worst,
              slow);
}

/* ---- scheduling ---------------------------------------------------------- */

/* The CPU a thread runs on now, and the calling thread's affinity as a mask of
 * CPUs 0-63. */
static int
thread_current_cpu(void)
{
#if defined(__linux__)
   return sched_getcpu();
#else
   return sceKernelGetCurrentCpu();
#endif
}

static uint64_t
thread_affinity(pthread_t thread, int *result)
{
   uint64_t mask = 0;
#if defined(__linux__)
   cpu_set_t set;
   CPU_ZERO(&set);
   *result = pthread_getaffinity_np(thread, sizeof(set), &set);
   for (int cpu = 0; cpu < 64; cpu++)
      if (CPU_ISSET(cpu, &set))
         mask |= 1ull << cpu;
#else
   cpuset_t set;
   CPU_ZERO(&set);
   *result = pthread_getaffinity_np(thread, sizeof(set), &set);
   for (int cpu = 0; cpu < 64 && cpu < CPU_SETSIZE; cpu++)
      if (CPU_ISSET(cpu, &set))
         mask |= 1ull << cpu;
#endif
   return mask;
}

enum { SPREAD_THREADS = 16, SPREAD_MS = 200 };

struct spread {
   uint64_t seen; /* the CPUs this thread ran on */
};

static void *
spread_body(void *opaque)
{
   struct spread *const s = opaque;
   const double end = thread_now_us() + SPREAD_MS * 1000.0;
   while (thread_now_us() < end) {
      const int cpu = thread_current_cpu();
      if (cpu >= 0 && cpu < 64)
         s->seen |= 1ull << cpu;
   }
   return NULL;
}

/* How the console schedules a title's threads: the policy and priority a
 * thread has, the range the policy allows, whether a priority can be set and
 * read back, the affinity threads start with, and the CPUs sixteen spinning
 * threads run on. */
static void
thread_scheduling(struct thread_probe *p)
{
   int policy = -1;
   struct sched_param param;
   memset(&param, 0, sizeof(param));
   const int got = pthread_getschedparam(pthread_self(), &policy, &param);
   thread_say(p, "scheduling calling thread policy=%d priority=%d result=%d range=%d..%d", policy,
              param.sched_priority, got, sched_get_priority_min(policy), sched_get_priority_max(policy));
   int affinity_result = -1;
   const uint64_t affinity = thread_affinity(pthread_self(), &affinity_result);
   thread_say(p, "scheduling calling thread affinity=%#llx result=%d cpu_now=%d",
              (unsigned long long)affinity, affinity_result, thread_current_cpu());

   struct spread spreads[SPREAD_THREADS];
   pthread_t threads[SPREAD_THREADS];
   unsigned created = 0;
   memset(spreads, 0, sizeof(spreads));
   for (unsigned i = 0; i < SPREAD_THREADS; i++)
      created += pthread_create(&threads[i], NULL, spread_body, &spreads[i]) == 0 ? 1u : 0u;
   /* One thread's priority changed while it runs, and read back. */
   int set_result = -1, reread_result = -1, reread_priority = -1, new_policy = -1;
   if (created == SPREAD_THREADS) {
      struct sched_param lower = param;
      lower.sched_priority = policy >= 0 ? sched_get_priority_min(policy) : param.sched_priority;
      set_result = pthread_setschedparam(threads[0], policy, &lower);
      struct sched_param back;
      memset(&back, 0, sizeof(back));
      reread_result = pthread_getschedparam(threads[0], &new_policy, &back);
      reread_priority = back.sched_priority;
   }
   uint64_t all = 0;
   char line[256];
   int used = snprintf(line, sizeof(line), "scheduling spread");
   for (unsigned i = 0; i < SPREAD_THREADS && i < created; i++) {
      pthread_join(threads[i], NULL);
      all |= spreads[i].seen;
      if (used > 0 && used < (int)sizeof(line) - 24)
         used += snprintf(line + used, sizeof(line) - (size_t)used, " %#llx",
                          (unsigned long long)spreads[i].seen);
   }
   thread_say(p, "%s", line);
   unsigned cpus = 0;
   for (int cpu = 0; cpu < 64; cpu++)
      cpus += (all >> cpu) & 1u;
   thread_say(p, "scheduling threads=%u cpus_seen=%#llx (%u) set_priority=%d result=%d reread=%d policy=%d result=%d",
              created, (unsigned long long)all, cpus,
              policy >= 0 ? sched_get_priority_min(policy) : -1, set_result, reread_priority, new_policy,
              reread_result);
}

int
ps5_platform_probe_threads(ps5_probe_log_fn log, void *context)
{
   struct thread_probe p = {.log = log, .context = context};
   thread_say(&p, "begin");

   pthread_attr_t attributes;
   size_t reported = 0;
   if (pthread_attr_init(&attributes) == 0) {
      const int got = pthread_attr_getstacksize(&attributes, &reported);
      thread_say(&p, "default attribute stacksize=%zu bytes (%zu KiB) result=%d", reported,
                 reported / 1024u, got);
      pthread_attr_destroy(&attributes);
   }

   const struct thread_stack own = thread_stack_of_self();
   thread_report(&p, "calling thread", &own);

   struct thread_stack plain = {0};
   const int plain_created = thread_spawn(&p, "no-attribute thread", NULL, &plain);
   const int plain_ok = plain_created == 0 && plain.result == 0 && plain.size > 0;
   p.failures += plain_ok ? 0 : 1;
   thread_say(&p, "check %s a thread created with no attributes reports its stack",
              plain_ok ? "PASS" : "FAIL");

   struct thread_stack asked = {0};
   int asked_ok = 0;
   if (pthread_attr_init(&attributes) == 0) {
      const int set = pthread_attr_setstacksize(&attributes, THREAD_PROBE_ASKED);
      const int created = set == 0 ? thread_spawn(&p, "2 MiB thread", &attributes, &asked) : set;
      asked_ok = created == 0 && asked.result == 0 && asked.size >= THREAD_PROBE_ASKED;
      pthread_attr_destroy(&attributes);
   }
   p.failures += asked_ok ? 0 : 1;
   thread_say(&p, "check %s a thread asking for %u bytes runs on at least that", asked_ok ? "PASS" : "FAIL",
              THREAD_PROBE_ASKED);

   thread_ping(&p, false);
   thread_ping(&p, true);
   thread_scheduling(&p);

   thread_say(&p, "end failures=%d", p.failures);
   return p.failures;
}
