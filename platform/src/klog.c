/*
 * PS5 Platform - a title's standard error in klog (include/ps5platform/klog.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ps5platform/klog.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define KLOG_LINE 900
#define KLOG_PREFIX 32

static char klog_prefix[KLOG_PREFIX];
static int klog_read_end = -1;
static pthread_mutex_t klog_lock = PTHREAD_MUTEX_INITIALIZER;
static int klog_started;

static void
klog_emit(char *line, size_t length)
{
   char record[KLOG_PREFIX + KLOG_LINE + 2];
   const int written = snprintf(record, sizeof(record), "%s%.*s\n", klog_prefix, (int)length, line);
   if (written > 0)
      sceKernelDebugOutText(0, record);
}

static void *
klog_reader(void *unused)
{
   (void)unused;
   char line[KLOG_LINE];
   size_t length = 0;
   for (;;) {
      char chunk[512];
      const ssize_t got = read(klog_read_end, chunk, sizeof(chunk));
      if (got < 0 && errno == EINTR)
         continue;
      if (got <= 0)
         break;
      for (ssize_t i = 0; i < got; i++) {
         if (chunk[i] == '\n' || length == sizeof(line)) {
            klog_emit(line, length);
            length = 0;
            if (chunk[i] == '\n')
               continue;
         }
         line[length++] = chunk[i];
      }
   }
   if (length)
      klog_emit(line, length);
   return NULL;
}

int
ps5_klog_capture_stderr(const char *prefix)
{
   pthread_mutex_lock(&klog_lock);
   if (klog_started) {
      pthread_mutex_unlock(&klog_lock);
      return 0;
   }
   snprintf(klog_prefix, sizeof(klog_prefix), "%s", prefix ? prefix : "");
   int ends[2];
   if (pipe(ends) != 0) {
      pthread_mutex_unlock(&klog_lock);
      return -1;
   }
   klog_read_end = ends[0];
   pthread_t thread;
   const int created = pthread_create(&thread, NULL, klog_reader, NULL);
   if (created != 0) {
      close(ends[0]);
      close(ends[1]);
      klog_read_end = -1;
      pthread_mutex_unlock(&klog_lock);
      errno = created;
      return -1;
   }
   pthread_detach(thread);
   fflush(stderr);
   /* A title may not dup2 (the console refuses it with EPERM), so the stream
    * stderr moves to the pipe instead of descriptor 2; everything written
    * through stderr (fprintf, perror, assert) goes there. */
   if (dup2(ends[1], STDERR_FILENO) == STDERR_FILENO) {
      close(ends[1]);
   } else {
      FILE *const stream = fdopen(ends[1], "w");
      if (!stream) {
         close(ends[1]);
         pthread_mutex_unlock(&klog_lock);
         return -1;
      }
      setvbuf(stream, NULL, _IOLBF, 0);
      stderr = stream;
   }
   klog_started = 1;
   pthread_mutex_unlock(&klog_lock);
   return 0;
}
