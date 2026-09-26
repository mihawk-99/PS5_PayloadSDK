/*
 * PS5 Platform - a thread's default stack (include/ps5platform/libc.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A thread whose creator asks for no stack size runs on 64 KiB, where the main
 * thread has 2 MiB, and a thread created asking for 2 MiB gets it (docs/PROBE.md,
 * "Threads"). Libraries start their threads without asking -- Mesa's queues, the
 * Vulkan CTS, libc++'s std::thread -- and code with larger frames than a
 * sixteenth of a desktop's default overruns them. A consumer that links with
 * --wrap=pthread_create gets __wrap_pthread_create below instead: a thread
 * whose attributes name no stack of its own, or a smaller one than the main
 * thread's, gets the main thread's 2 MiB; everything else the caller set is
 * kept.
 */
#define _GNU_SOURCE 1
/* pthread_attr_getstackaddr is deprecated, and the one call that says what
 * this needs. */
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include "ps5platform/libc.h"

#include <pthread.h>
#include <string.h>

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*start)(void *), void *argument);

/* A copy of attributes with a larger stack; false when it could not be made,
 * or when the caller placed the stack itself. */
static bool
enlarged_copy(const pthread_attr_t *attributes, pthread_attr_t *copy)
{
   if (pthread_attr_init(copy) != 0)
      return false;
   if (attributes) {
      /* pthread_attr_getstackaddr, not getstack: glibc's getstack derives an
       * address even for attributes that place no stack. */
      void *address = NULL;
      size_t size = 0;
      if (pthread_attr_getstackaddr(attributes, &address) == 0 && address != NULL) {
         pthread_attr_destroy(copy);
         return false;
      }
      if (pthread_attr_getstacksize(attributes, &size) == 0 && size >= PS5_THREAD_STACK_BYTES) {
         pthread_attr_destroy(copy);
         return false;
      }
      int value;
      struct sched_param parameter;
      size_t guard;
      if (pthread_attr_getdetachstate(attributes, &value) == 0)
         pthread_attr_setdetachstate(copy, value);
      if (pthread_attr_getinheritsched(attributes, &value) == 0)
         pthread_attr_setinheritsched(copy, value);
      if (pthread_attr_getschedpolicy(attributes, &value) == 0)
         pthread_attr_setschedpolicy(copy, value);
      if (pthread_attr_getschedparam(attributes, &parameter) == 0)
         pthread_attr_setschedparam(copy, &parameter);
      if (pthread_attr_getscope(attributes, &value) == 0)
         pthread_attr_setscope(copy, value);
      if (pthread_attr_getguardsize(attributes, &guard) == 0)
         pthread_attr_setguardsize(copy, guard);
   }
   if (pthread_attr_setstacksize(copy, PS5_THREAD_STACK_BYTES) != 0) {
      pthread_attr_destroy(copy);
      return false;
   }
   return true;
}

int
__wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *),
                      void *argument)
{
   pthread_attr_t copy;
   if (!enlarged_copy(attributes, &copy))
      return __real_pthread_create(thread, attributes, start, argument);
   const int result = __real_pthread_create(thread, &copy, start, argument);
   pthread_attr_destroy(&copy);
   return result;
}
