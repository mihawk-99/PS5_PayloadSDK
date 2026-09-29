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
#include <sys/event.h>

#include "ps5platform/kernel.h"
#endif
#include "ps5platform/libc.h"
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
   /* What a program asks the system about its CPUs and pages (RPCS3 sizes its
    * thread pools and its protection by these). */
   thread_say(p, "scheduling sysconf nprocessors_onln=%ld nprocessors_conf=%ld pagesize=%ld",
              sysconf(_SC_NPROCESSORS_ONLN), sysconf(_SC_NPROCESSORS_CONF), sysconf(_SC_PAGESIZE));
#if !defined(__linux__)
   /* The affinity mask sizes pthread_getaffinity_np accepts, and the exported
    * 64-bit form, read and set back unchanged. */
   char sizes[160];
   int written = snprintf(sizes, sizeof(sizes), "scheduling getaffinity_np");
   static const size_t tried[] = {8, 16, 32, 64, 128};
   for (unsigned i = 0; i < sizeof(tried) / sizeof(tried[0]); i++) {
      unsigned char mask[128];
      memset(mask, 0, sizeof(mask));
      const int result = pthread_getaffinity_np(pthread_self(), tried[i], (cpuset_t *)(void *)mask);
      uint64_t low = 0;
      memcpy(&low, mask, sizeof(low));
      if (written > 0 && written < (int)sizeof(sizes) - 32)
         written += snprintf(sizes + written, sizeof(sizes) - (size_t)written, " size%zu=%d:%#llx", tried[i], result,
                          (unsigned long long)low);
   }
   thread_say(p, "%s", sizes);
   uint64_t sce_mask = 0;
   const int32_t got_mask = scePthreadGetaffinity(pthread_self(), &sce_mask);
   const int32_t set_mask = got_mask == 0 ? scePthreadSetaffinity(pthread_self(), sce_mask) : -1;
   uint64_t again = 0;
   const int32_t reread_mask = scePthreadGetaffinity(pthread_self(), &again);
   thread_say(p, "scheduling scePthreadGetaffinity=%#x mask=%#llx set=%#x reread=%#x mask=%#llx",
              (unsigned)got_mask, (unsigned long long)sce_mask, (unsigned)set_mask, (unsigned)reread_mask,
              (unsigned long long)again);
#endif
   /* The platform layer's answer for FreeBSD's cpuset_t (32 bytes). */
   unsigned char set[32];
   memset(set, 0xff, sizeof(set));
   const int got_set = ps5_pthread_getaffinity_np(pthread_self(), sizeof(set), set);
   uint64_t set_low = 0;
   memcpy(&set_low, set, sizeof(set_low));
   const int put_set = got_set == 0 ? ps5_pthread_setaffinity_np(pthread_self(), sizeof(set), set) : -1;
   size_t page_sizes[2] = {0, 0};
   const int page_count = ps5_getpagesizes(page_sizes, 2);
   thread_say(p, "scheduling platform getaffinity=%d mask=%#llx setaffinity=%d getpagesizes=%d:%zu sysconf_cpus=%ld",
              got_set, (unsigned long long)set_low, put_set, page_count, page_sizes[0],
              ps5_sysconf(_SC_NPROCESSORS_ONLN));
}

/* A thread_local's destructor sees its own thread's thread-local storage: the
 * C++ destructors (ps5___cxa_thread_atexit_impl's key) run before emulated TLS
 * frees the thread's storage (its own key). */
static _Thread_local uint64_t order_value;
static uint64_t order_seen;

static void
order_destructor(void *object)
{
   (void)object;
   order_seen = order_value;
}

static void *
order_body(void *unused)
{
   (void)unused;
   order_value = 0x5a5a1234u;
   ps5___cxa_thread_atexit_impl(order_destructor, NULL, NULL);
   return NULL;
}

static void
thread_local_order(struct thread_probe *p)
{
   order_seen = 0;
   pthread_t thread;
   const int created = pthread_create(&thread, NULL, order_body, NULL);
   if (created == 0)
      pthread_join(thread, NULL);
   const bool ok = created == 0 && order_seen == 0x5a5a1234u;
   p->failures += ok ? 0 : 1;
   thread_say(p, "check %s a thread_local destructor at thread exit reads its thread's storage (%#llx)",
              ok ? "PASS" : "FAIL", (unsigned long long)order_seen);
}

#if !defined(__linux__)
/* kqueue as RPCS3's audio timer uses it (sys_rsxaudio.cpp): a thread waits
 * with no timeout for a nanosecond timer or a user event another thread
 * triggers to cancel the wait. */
struct kqueue_wait {
   int kq;
   int result;
   uintptr_t ident;
   double waited_us;
};

static void *
kqueue_waiter(void *opaque)
{
   struct kqueue_wait *const wait = opaque;
   struct kevent event;
   memset(&event, 0, sizeof(event));
   const struct timespec timeout = {2, 0};
   const double begin = thread_now_us();
   wait->result = kevent(wait->kq, NULL, 0, &event, 1, &timeout);
   wait->waited_us = thread_now_us() - begin;
   wait->ident = event.ident;
   return NULL;
}

static void
thread_kqueue(struct thread_probe *p)
{
   struct kqueue_wait wait = {.kq = kqueue(), .result = -2, .ident = 99};
   struct kevent change;
   EV_SET(&change, 1, EVFILT_USER, EV_ADD | EV_ENABLE | EV_CLEAR, NOTE_FFNOP, 0, NULL);
   const int added = wait.kq >= 0 ? kevent(wait.kq, &change, 1, NULL, 0, NULL) : -1;
   pthread_t thread;
   const int created = pthread_create(&thread, NULL, kqueue_waiter, &wait);
   struct timespec pause = {0, 50 * 1000 * 1000};
   nanosleep(&pause, NULL);
   EV_SET(&change, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
   const int triggered = kevent(wait.kq, &change, 1, NULL, 0, NULL);
   const int trigger_errno = triggered < 0 ? errno : 0;
   if (created == 0)
      pthread_join(thread, NULL);
   const bool user_ok = added == 0 && triggered == 0 && wait.result == 1 && wait.ident == 1 && wait.waited_us < 1000000;
   thread_say(p, "kqueue user event added=%d triggered=%d (errno %d) waiter=%d ident=%lu waited_us=%.0f", added,
              triggered, trigger_errno, wait.result, (unsigned long)wait.ident, wait.waited_us);

   /* A 5 ms one-shot timer in nanoseconds. */
   struct kqueue_wait timer = {.kq = wait.kq, .result = -2, .ident = 99};
   EV_SET(&change, 0, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_NSECONDS, 5 * 1000 * 1000, NULL);
   const int armed = wait.kq >= 0 ? kevent(wait.kq, &change, 1, NULL, 0, NULL) : -1;
   kqueue_waiter(&timer);
   thread_say(p, "kqueue timer armed=%d waiter=%d ident=%lu waited_us=%.0f", armed, timer.result,
              (unsigned long)timer.ident, timer.waited_us);
   if (wait.kq >= 0)
      close(wait.kq);

   /* RPCS3's own form: the timer (armed for 10 s) and the user event kept as
    * EV_SET left them, and one kevent call that disables the timer and
    * re-submits the user event (EV_ADD, NOTE_FFNOP | NOTE_TRIGGER). */
   struct kqueue_wait rpcs3 = {.kq = kqueue(), .result = -2, .ident = 99};
   struct kevent handles[2];
   EV_SET(&handles[0], 0, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_NSECONDS, 0, NULL);
   EV_SET(&handles[1], 1, EVFILT_USER, EV_ADD | EV_ENABLE | EV_CLEAR, NOTE_FFNOP, 0, NULL);
   const int user_added = kevent(rpcs3.kq, &handles[1], 1, NULL, 0, NULL);
   handles[1].fflags |= NOTE_TRIGGER;
   handles[0].data = 10LL * 1000 * 1000 * 1000;
   const int timer_armed = kevent(rpcs3.kq, &handles[0], 1, NULL, 0, NULL);
   const int rpcs3_created = pthread_create(&thread, NULL, kqueue_waiter, &rpcs3);
   nanosleep(&pause, NULL);
   handles[0].flags = (handles[0].flags & ~EV_ENABLE) | EV_DISABLE;
   handles[0].data = 0;
   const int cancelled = kevent(rpcs3.kq, handles, 2, NULL, 0, NULL);
   const int cancel_errno = cancelled < 0 ? errno : 0;
   if (rpcs3_created == 0)
      pthread_join(thread, NULL);
   close(rpcs3.kq);
   thread_say(p, "kqueue rpcs3-form added=%d armed=%d cancelled=%d (errno %d) waiter=%d ident=%lu waited_us=%.0f",
              user_added, timer_armed, cancelled, cancel_errno, rpcs3.result, (unsigned long)rpcs3.ident,
              rpcs3.waited_us);
   p->failures += user_ok ? 0 : 1;
   thread_say(p, "check %s a kqueue user event triggered by another thread wakes a waiter", user_ok ? "PASS" : "FAIL");
}
#endif

/* ---- topology ------------------------------------------------------------ */

/* How far apart the title's CPUs are: for each pair, two threads pinned one to
 * each pass a cache line back and forth, and the mean round trip says whether
 * they are one core's two threads, two cores sharing an L3, or two cores in
 * different clusters. Both are fresh threads that pin themselves and meet
 * before the first round, so neither starts on a CPU the other holds; the
 * caller only waits, and gives up on a pair after two seconds. */
#define TOPOLOGY_ROUNDS 20000u
#define TOPOLOGY_CPUS 16

struct topology_pair {
   volatile unsigned turn;
   volatile unsigned abandoned; /* the caller gave up on the pair */
   volatile unsigned ready;     /* how many of the two are pinned */
   volatile unsigned done;      /* how many of the two have finished */
   int cpu[2];
   int pinned[2];
   volatile int ran_on[2];
   double ns;                   /* the prober's mean round trip */
};

static int
topology_pin(int cpu)
{
#if defined(__linux__)
   cpu_set_t set;
   CPU_ZERO(&set);
   CPU_SET(cpu, &set);
   return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
   unsigned char set[32];
   memset(set, 0, sizeof(set));
   set[cpu / 8] = (unsigned char)(1u << (cpu % 8));
   return ps5_pthread_setaffinity_np(pthread_self(), sizeof(set), set);
#endif
}

static inline void
topology_pause(void)
{
#if defined(__x86_64__)
   __builtin_ia32_pause();
#endif
}

/* Spins until turn reads want or the pair is abandoned; false when abandoned. */
static bool
topology_wait(struct topology_pair *pair, unsigned want)
{
   unsigned spins = 0;
   while (__atomic_load_n(&pair->turn, __ATOMIC_ACQUIRE) != want) {
      topology_pause();
      if ((++spins & 1023u) == 0 && __atomic_load_n(&pair->abandoned, __ATOMIC_ACQUIRE))
         return false;
   }
   return true;
}

struct topology_side {
   struct topology_pair *pair;
   int side; /* 0: the prober, 1: its partner */
};

static void *
topology_body(void *opaque)
{
   const struct topology_side *const me = opaque;
   struct topology_pair *const pair = me->pair;
   const int side = me->side;
   pair->pinned[side] = topology_pin(pair->cpu[side]);
   pair->ran_on[side] = thread_current_cpu();
   __atomic_fetch_add(&pair->ready, 1u, __ATOMIC_ACQ_REL);
   while (__atomic_load_n(&pair->ready, __ATOMIC_ACQUIRE) < 2u) {
      if (__atomic_load_n(&pair->abandoned, __ATOMIC_ACQUIRE))
         goto out;
      sched_yield();
   }
   double start = 0.0;
   for (unsigned round = 0; round < TOPOLOGY_ROUNDS; round++) {
      if (side == 0) {
         /* The first hundred rounds warm the line. */
         if (round == 100u)
            start = thread_now_us();
         __atomic_store_n(&pair->turn, 2u * round + 1u, __ATOMIC_RELEASE);
         if (!topology_wait(pair, 2u * round + 2u))
            goto out;
      } else {
         if (!topology_wait(pair, 2u * round + 1u))
            goto out;
         __atomic_store_n(&pair->turn, 2u * round + 2u, __ATOMIC_RELEASE);
      }
   }
   if (side == 0)
      pair->ns = (thread_now_us() - start) * 1000.0 / (double)(TOPOLOGY_ROUNDS - 100u);
out:
   __atomic_fetch_add(&pair->done, 1u, __ATOMIC_ACQ_REL);
   return NULL;
}

/* The mean round trip in nanoseconds between a thread pinned to cpu a and one
 * pinned to cpu b; negative when a thread was not created (-2), could not be
 * pinned (-3), the pair did not finish in two seconds (-4) or a thread found
 * itself on another CPU (-5). */
static double
topology_round_trip(int a, int b)
{
   static struct topology_pair pair;
   memset(&pair, 0, sizeof(pair));
   pair.cpu[0] = a;
   pair.cpu[1] = b;
   pair.ran_on[0] = pair.ran_on[1] = -1;
   pair.ns = -4.0;
   static struct topology_side sides[2];
   pthread_t threads[2];
   bool created[2] = {false, false};
   for (int side = 0; side < 2; side++) {
      sides[side] = (struct topology_side){.pair = &pair, .side = side};
      pthread_attr_t attributes;
      pthread_attr_init(&attributes);
      pthread_attr_setstacksize(&attributes, 256u * 1024u);
      created[side] = pthread_create(&threads[side], &attributes, topology_body, &sides[side]) == 0;
      pthread_attr_destroy(&attributes);
   }
   if (!created[0] || !created[1]) {
      __atomic_store_n(&pair.abandoned, 1u, __ATOMIC_RELEASE);
      for (int side = 0; side < 2; side++)
         if (created[side])
            pthread_join(threads[side], NULL);
      return -2.0;
   }
   const double deadline = thread_now_us() + 2e6;
   while (__atomic_load_n(&pair.done, __ATOMIC_ACQUIRE) < 2u && thread_now_us() < deadline)
      usleep(1000);
   const bool finished = __atomic_load_n(&pair.done, __ATOMIC_ACQUIRE) == 2u;
   __atomic_store_n(&pair.abandoned, 1u, __ATOMIC_RELEASE);
   for (int side = 0; side < 2; side++)
      pthread_join(threads[side], NULL);
   if (pair.pinned[0] != 0 || pair.pinned[1] != 0)
      return -3.0;
   if (!finished)
      return -4.0;
   if (pair.ran_on[0] != a || pair.ran_on[1] != b)
      return -5.0;
   return pair.ns;
}

int
ps5_platform_probe_topology(ps5_probe_log_fn log, void *context)
{
   struct thread_probe p = {.log = log, .context = context};
   thread_say(&p, "topology begin");

   int affinity_result = -1;
#if defined(__linux__)
   const uint64_t affinity = thread_affinity(pthread_self(), &affinity_result);
#else
   /* The platform layer's call: libc's refuses a cpuset_t there (ERANGE). */
   unsigned char set[32];
   memset(set, 0, sizeof(set));
   affinity_result = ps5_pthread_getaffinity_np(pthread_self(), sizeof(set), set);
   uint64_t affinity = 0;
   memcpy(&affinity, set, sizeof(affinity));
#endif
   int cpus[TOPOLOGY_CPUS];
   int count = 0;
   for (int cpu = 0; cpu < 64 && count < TOPOLOGY_CPUS; cpu++)
      if (affinity & (1ull << cpu))
         cpus[count++] = cpu;
   thread_say(&p, "topology affinity=%#llx result=%d cpus=%d", (unsigned long long)affinity, affinity_result,
              count);

   /* One line a CPU: its round trips to every other, in nanoseconds. */
   double worst = 0.0, best = 1e30;
   int refused = 0;
   for (int i = 0; i < count; i++) {
      char line[400];
      int used = snprintf(line, sizeof(line), "topology cpu %d:", cpus[i]);
      for (int j = 0; j < count; j++) {
         if (i == j) {
            used += snprintf(line + used, sizeof(line) - (size_t)used, " -");
            continue;
         }
         const double ns = topology_round_trip(cpus[i], cpus[j]);
         refused += ns < 0.0;
         if (ns > 0.0) {
            worst = ns > worst ? ns : worst;
            best = ns < best ? ns : best;
         }
         used += snprintf(line + used, sizeof(line) - (size_t)used, " %.0f", ns);
      }
      thread_say(&p, "%s", line);
   }

   /* The calling thread goes back to every CPU it had. */
#if defined(__linux__)
   cpu_set_t all;
   CPU_ZERO(&all);
   for (int i = 0; i < count; i++)
      CPU_SET(cpus[i], &all);
   pthread_setaffinity_np(pthread_self(), sizeof(all), &all);
#else
   unsigned char all[32];
   memset(all, 0, sizeof(all));
   for (int i = 0; i < count; i++)
      all[cpus[i] / 8] |= (unsigned char)(1u << (cpus[i] % 8));
   ps5_pthread_setaffinity_np(pthread_self(), sizeof(all), all);
#endif

   const bool ok = count >= 2 && refused == 0;
   p.failures += ok ? 0 : 1;
   thread_say(&p, "check %s every pair of the %d CPUs could be pinned and measured (%d refused), %.0f..%.0f ns",
              ok ? "PASS" : "FAIL", count, refused, count >= 2 ? best : 0.0, worst);
   thread_say(&p, "topology end failures=%d", p.failures);
   return p.failures;
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
   thread_local_order(&p);
#if !defined(__linux__)
   thread_kqueue(&p);
#endif

   thread_say(&p, "end failures=%d", p.failures);
   return p.failures;
}
