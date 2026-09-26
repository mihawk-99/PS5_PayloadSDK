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
#if !defined(__linux__)
#include <pthread_np.h>
#endif
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

   thread_say(&p, "end failures=%d", p.failures);
   return p.failures;
}
