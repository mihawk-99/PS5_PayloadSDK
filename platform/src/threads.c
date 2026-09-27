/*
 * PS5 Platform - a thread's default stack (include/ps5platform/libc.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A thread whose creator asks for no stack size runs on 64 KiB, where the main
 * thread has 2 MiB, and a thread created asking for 2 MiB gets it (docs/PROBE.md,
 * "Threads"). Libraries start their threads without asking -- Mesa's queues, the
 * Vulkan CTS, libc++'s std::thread -- and code with larger frames than a
 * sixteenth of a desktop's default overruns them.
 *
 * libkernel takes a thread's stack from flexible memory, about 400 MiB for the
 * whole title, which 256 threads of 2 MiB exceed (the Vulkan CTS's concurrent
 * image copies start that many, and pthread_create failed). So a consumer that
 * links with --wrap for pthread_create, pthread_join and pthread_detach gets
 * these instead: a thread whose attributes place no stack of their own and ask
 * for less than the main thread's gets the main thread's 2 MiB, in direct
 * memory, with an unmapped guard page below it. Everything else the caller set
 * is kept, and a thread that places its own stack or asks for 2 MiB or more is
 * created as asked.
 *
 * A stack is freed once its thread has ended: pthread_join frees it, and a
 * detached thread's is freed by a reaper thread that joins it once its start
 * routine has returned (or it called pthread_exit). Such threads are created
 * joinable, so the reaper can; for the caller they are detached as asked. A
 * few freed stacks are kept for the next threads.
 *
 * Every thread created through the wrap starts with its creator's MXCSR, as a
 * Linux thread does, so a title that set the IEEE state (ps5platform/fp.h)
 * keeps it in its threads.
 */
#define _GNU_SOURCE 1
/* pthread_attr_getstackaddr is deprecated, and the one call that says what
 * this needs. */
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include "ps5platform/libc.h"

#include "placement.h"
#include "ps5platform/kernel.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*start)(void *), void *argument);
int __real_pthread_join(pthread_t thread, void **value);
int __real_pthread_detach(pthread_t thread);

#define STACK_GUARD PS5P_PAGE
#define STACK_CACHE 32
#define STACK_PROTECTION (PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE)

struct thread_stack {
   /* The reservation: the guard page, then the stack. */
   void *base;
   int64_t direct_start;
};

struct thread_record {
   struct thread_record *next;
   pthread_t thread;
   struct thread_stack stack;
   void *(*start)(void *);
   void *argument;
   uint32_t mxcsr;
   bool detached;
   bool finished;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reaper_wake = PTHREAD_COND_INITIALIZER;
static struct thread_record *records;
static struct thread_stack cache[STACK_CACHE];
static unsigned cached;
static unsigned live_stacks;
static bool reaper_started;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static pthread_key_t finish_key;

/* ------------------------------------------------------------- the stacks */

static bool
stack_map(struct thread_stack *stack)
{
   const size_t bytes = PS5_THREAD_STACK_BYTES;
   void *base = NULL;
   if (ps5p_reserve_placed(bytes + STACK_GUARD, 0, PS5P_DIRECT_UNIT, -1, &base) != 0)
      return false;
   int64_t start = -1;
   if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, PS5P_DIRECT_UNIT,
                                     PS5_KERNEL_DIRECT_TYPE_CPU, &start) != 0) {
      sceKernelMunmap(base, bytes + STACK_GUARD);
      return false;
   }
   void *const at = (char *)base + STACK_GUARD;
   void *mapped = at;
   if (sceKernelMapDirectMemory(&mapped, bytes, STACK_PROTECTION, PS5_KERNEL_MAP_FIXED, start,
                                PS5P_DIRECT_UNIT) != 0 ||
       mapped != at) {
      sceKernelReleaseDirectMemory(start, bytes);
      sceKernelMunmap(base, bytes + STACK_GUARD);
      return false;
   }
   stack->base = base;
   stack->direct_start = start;
   return true;
}

static void
stack_unmap(const struct thread_stack *stack)
{
   sceKernelMunmap(stack->base, PS5_THREAD_STACK_BYTES + STACK_GUARD);
   sceKernelReleaseDirectMemory(stack->direct_start, PS5_THREAD_STACK_BYTES);
}

/* Called with the lock held. */
static bool
stack_take(struct thread_stack *stack)
{
   if (cached) {
      *stack = cache[--cached];
      live_stacks++;
      return true;
   }
   if (!stack_map(stack))
      return false;
   live_stacks++;
   return true;
}

/* Called with the lock held. */
static void
stack_give(const struct thread_stack *stack)
{
   live_stacks--;
   if (cached < STACK_CACHE)
      cache[cached++] = *stack;
   else
      stack_unmap(stack);
}

/* ------------------------------------------------------------ the records */

/* Called with the lock held; unlinks the record it returns. */
static struct thread_record *
record_take(pthread_t thread)
{
   for (struct thread_record **at = &records; *at; at = &(*at)->next) {
      if (pthread_equal((*at)->thread, thread)) {
         struct thread_record *const record = *at;
         *at = record->next;
         return record;
      }
   }
   return NULL;
}

static void *
reaper(void *unused)
{
   (void)unused;
   pthread_mutex_lock(&lock);
   for (;;) {
      struct thread_record *found = NULL;
      for (struct thread_record **at = &records; *at; at = &(*at)->next) {
         if ((*at)->detached && (*at)->finished) {
            found = *at;
            *at = found->next;
            break;
         }
      }
      if (!found) {
         pthread_cond_wait(&reaper_wake, &lock);
         continue;
      }
      pthread_mutex_unlock(&lock);
      __real_pthread_join(found->thread, NULL);
      pthread_mutex_lock(&lock);
      stack_give(&found->stack);
      free(found);
   }
   return NULL;
}

/* Called with the lock held, once a detached thread is known. */
static void
reaper_ensure(void)
{
   if (reaper_started)
      return;
   pthread_t thread;
   pthread_attr_t attributes;
   pthread_attr_init(&attributes);
   pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
   if (__real_pthread_create(&thread, &attributes, reaper, NULL) == 0)
      reaper_started = true;
   pthread_attr_destroy(&attributes);
}

/* The thread's start routine returned or it called pthread_exit: its record
 * is finished, and a detached one is the reaper's to join. */
static void
thread_finished(void *opaque)
{
   struct thread_record *const record = opaque;
   pthread_mutex_lock(&lock);
   record->finished = true;
   if (record->detached)
      pthread_cond_signal(&reaper_wake);
   pthread_mutex_unlock(&lock);
}

static void
key_create(void)
{
   pthread_key_create(&finish_key, thread_finished);
}

static uint32_t
mxcsr_read(void)
{
   uint32_t mxcsr;
   __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
   return mxcsr;
}

static void
mxcsr_write(uint32_t mxcsr)
{
   __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}

static void *
thread_start(void *opaque)
{
   struct thread_record *const record = opaque;
   mxcsr_write(record->mxcsr);
   pthread_setspecific(finish_key, record);
   return record->start(record->argument);
}

/* A thread on a stack of its own (or libkernel's): only the creator's MXCSR
 * travels with it. */
struct plain_start {
   void *(*start)(void *);
   void *argument;
   uint32_t mxcsr;
};

static void *
plain_thread_start(void *opaque)
{
   const struct plain_start begin = *(const struct plain_start *)opaque;
   free(opaque);
   mxcsr_write(begin.mxcsr);
   return begin.start(begin.argument);
}

static int
create_plain(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *), void *argument)
{
   struct plain_start *const begin = malloc(sizeof(*begin));
   if (!begin)
      return EAGAIN;
   begin->start = start;
   begin->argument = argument;
   begin->mxcsr = mxcsr_read();
   const int result = __real_pthread_create(thread, attributes, plain_thread_start, begin);
   if (result != 0)
      free(begin);
   return result;
}

/* -------------------------------------------------------------- the wraps */

/* A copy of attributes that keeps what the caller set, for a stack of its own;
 * false when the caller placed the stack itself or asked for one at least as
 * large as the main thread's. detached says what the caller asked for. */
static bool
stack_attributes(const pthread_attr_t *attributes, pthread_attr_t *copy, bool *detached)
{
   *detached = false;
   if (attributes) {
      /* pthread_attr_getstackaddr, not getstack: glibc's getstack derives an
       * address even for attributes that place no stack. */
      void *address = NULL;
      size_t size = 0;
      if (pthread_attr_getstackaddr(attributes, &address) == 0 && address != NULL)
         return false;
      if (pthread_attr_getstacksize(attributes, &size) == 0 && size >= PS5_THREAD_STACK_BYTES)
         return false;
   }
   if (pthread_attr_init(copy) != 0)
      return false;
   if (attributes) {
      int value;
      struct sched_param parameter;
      if (pthread_attr_getdetachstate(attributes, &value) == 0)
         *detached = value == PTHREAD_CREATE_DETACHED;
      if (pthread_attr_getinheritsched(attributes, &value) == 0)
         pthread_attr_setinheritsched(copy, value);
      if (pthread_attr_getschedpolicy(attributes, &value) == 0)
         pthread_attr_setschedpolicy(copy, value);
      if (pthread_attr_getschedparam(attributes, &parameter) == 0)
         pthread_attr_setschedparam(copy, &parameter);
      if (pthread_attr_getscope(attributes, &value) == 0)
         pthread_attr_setscope(copy, value);
   }
   pthread_attr_setdetachstate(copy, PTHREAD_CREATE_JOINABLE);
   return true;
}

int
__wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *),
                      void *argument)
{
   pthread_attr_t copy;
   bool detached;
   if (!stack_attributes(attributes, &copy, &detached))
      return create_plain(thread, attributes, start, argument);
   pthread_once(&key_once, key_create);

   struct thread_record *const record = calloc(1, sizeof(*record));
   if (!record) {
      pthread_attr_destroy(&copy);
      return EAGAIN;
   }
   record->start = start;
   record->argument = argument;
   record->mxcsr = mxcsr_read();
   record->detached = detached;

   pthread_mutex_lock(&lock);
   const bool have_stack = stack_take(&record->stack);
   pthread_mutex_unlock(&lock);
   if (!have_stack) {
      /* No direct memory: libkernel's own stack, as large as it can. */
      free(record);
      pthread_attr_setdetachstate(&copy, detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);
      pthread_attr_setstacksize(&copy, PS5_THREAD_STACK_BYTES);
      const int result = create_plain(thread, &copy, start, argument);
      pthread_attr_destroy(&copy);
      return result;
   }
   pthread_attr_setstack(&copy, (char *)record->stack.base + STACK_GUARD, PS5_THREAD_STACK_BYTES);

   /* The record is listed before the thread runs, so its end finds it. */
   pthread_mutex_lock(&lock);
   if (detached)
      reaper_ensure();
   const int result = __real_pthread_create(&record->thread, &copy, thread_start, record);
   if (result == 0) {
      record->next = records;
      records = record;
      *thread = record->thread;
   } else {
      stack_give(&record->stack);
   }
   pthread_mutex_unlock(&lock);
   pthread_attr_destroy(&copy);
   if (result != 0)
      free(record);
   return result;
}

int
__wrap_pthread_join(pthread_t thread, void **value)
{
   pthread_mutex_lock(&lock);
   bool ours = false;
   for (struct thread_record *record = records; record; record = record->next)
      ours |= pthread_equal(record->thread, thread) && !record->detached;
   pthread_mutex_unlock(&lock);
   const int result = __real_pthread_join(thread, value);
   if (result == 0 && ours) {
      pthread_mutex_lock(&lock);
      struct thread_record *const record = record_take(thread);
      if (record)
         stack_give(&record->stack);
      pthread_mutex_unlock(&lock);
      free(record);
   }
   return result;
}

int
__wrap_pthread_detach(pthread_t thread)
{
   pthread_mutex_lock(&lock);
   for (struct thread_record *record = records; record; record = record->next) {
      if (pthread_equal(record->thread, thread)) {
         const int result = record->detached ? EINVAL : 0;
         record->detached = true;
         reaper_ensure();
         if (record->finished)
            pthread_cond_signal(&reaper_wake);
         pthread_mutex_unlock(&lock);
         return result;
      }
   }
   pthread_mutex_unlock(&lock);
   return __real_pthread_detach(thread);
}

void
ps5_thread_stacks(unsigned *live, unsigned *cached_stacks)
{
   pthread_mutex_lock(&lock);
   *live = live_stacks;
   *cached_stacks = cached;
   pthread_mutex_unlock(&lock);
}
