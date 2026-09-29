/*
 * PS5 Platform - C++ thread_local destructors (include/ps5platform/libc.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * libc++abi registers a thread_local object's destructor through
 * __cxa_thread_atexit_impl when the C library has one; no system module
 * exports it, and a title's converter refuses an import nothing provides. This
 * is that function: each thread keeps its destructors in a list that a pthread
 * key's destructor runs, last registered first, when the thread exits, and the
 * thread that calls exit() runs its own from an atexit handler, since exit()
 * runs no key destructors.
 *
 * A thread the platform's pthread_create started runs its list as soon as its
 * start routine returns (src/threads.c), as glibc runs thread_local
 * destructors before any key destructor: the console's libkernel runs key
 * destructors in no set order and in one pass, and emulated TLS frees a
 * thread's thread_local storage from its own key, which left a destructor that
 * ran after it working on freed storage (docs/PROBE.md). The key stays for
 * threads that exit another way.
 */
#include "ps5platform/libc.h"

#include <pthread.h>
#include <stdlib.h>

struct thread_destructor {
   void (*destructor)(void *);
   void *object;
   struct thread_destructor *next;
};

static pthread_key_t destructors_key;
static pthread_once_t destructors_once = PTHREAD_ONCE_INIT;
static bool destructors_ready;

static void
run_destructors(void *list)
{
   struct thread_destructor *entry = list;
   while (entry) {
      struct thread_destructor *const next = entry->next;
      entry->destructor(entry->object);
      free(entry);
      /* A destructor may register another: take what it added too. */
      struct thread_destructor *const added = pthread_getspecific(destructors_key);
      if (added) {
         pthread_setspecific(destructors_key, NULL);
         run_destructors(added);
      }
      entry = next;
   }
}

void
ps5p_run_thread_destructors(void)
{
   if (!destructors_ready)
      return;
   struct thread_destructor *const list = pthread_getspecific(destructors_key);
   pthread_setspecific(destructors_key, NULL);
   run_destructors(list);
}

static void
run_exiting_thread(void)
{
   struct thread_destructor *const list = pthread_getspecific(destructors_key);
   pthread_setspecific(destructors_key, NULL);
   run_destructors(list);
}

static void
setup_destructors(void)
{
   destructors_ready = pthread_key_create(&destructors_key, run_destructors) == 0 &&
                       atexit(run_exiting_thread) == 0;
}

int
ps5___cxa_thread_atexit_impl(void (*destructor)(void *), void *object, void *dso)
{
   (void)dso;
   pthread_once(&destructors_once, setup_destructors);
   if (!destructors_ready)
      return -1;
   struct thread_destructor *const entry = malloc(sizeof(*entry));
   if (!entry)
      return -1;
   entry->destructor = destructor;
   entry->object = object;
   entry->next = pthread_getspecific(destructors_key);
   if (pthread_setspecific(destructors_key, entry) != 0) {
      free(entry);
      return -1;
   }
   return 0;
}
