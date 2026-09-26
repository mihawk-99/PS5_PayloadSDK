/*
 * PS5 Platform - POSIX functions no system module exports
 * (include/ps5platform/libc.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Mesa's RADV port (PS5_Mesa) was the first consumer to need these: its util
 * library sorts with qsort_r, logs through openlog, names temporary files with
 * mkstemps and reads uname, and the SDK's own headers turn assert and a
 * fortified memset into __assert and __memset_chk. Each is built on functions
 * the console does export.
 */
#define _GNU_SOURCE 1

#include "ps5platform/libc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ qsort_r */

/* The comparator and its thunk for the qsort this thread is running. A
 * comparator that sorts again nests: each call keeps the outer pair and puts
 * it back. */
static _Thread_local void *qsort_thunk;
static _Thread_local int (*qsort_compare)(void *, const void *, const void *);

static int
qsort_trampoline(const void *a, const void *b)
{
   return qsort_compare(qsort_thunk, a, b);
}

void
ps5_qsort_r(void *base, size_t count, size_t size, void *thunk,
            int (*compare)(void *thunk, const void *a, const void *b))
{
   void *const outer_thunk = qsort_thunk;
   int (*const outer_compare)(void *, const void *, const void *) = qsort_compare;
   qsort_thunk = thunk;
   qsort_compare = compare;
   qsort(base, count, size, qsort_trampoline);
   qsort_thunk = outer_thunk;
   qsort_compare = outer_compare;
}

/* ----------------------------------------------------------------- mkstemps */

int
ps5_mkstemps(char *path_template, int suffix_length)
{
   static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
   const size_t length = path_template ? strlen(path_template) : 0;
   if (suffix_length < 0 || length < 6u + (size_t)suffix_length) {
      errno = EINVAL;
      return -1;
   }
   char *const x = path_template + length - (size_t)suffix_length - 6;
   if (memcmp(x, "XXXXXX", 6) != 0) {
      errno = EINVAL;
      return -1;
   }
   for (int attempt = 0; attempt < 1000; attempt++) {
      for (int i = 0; i < 6; i++)
         x[i] = alphabet[ps5_arc4random_uniform(sizeof(alphabet) - 1)];
      const int fd = open(path_template, O_RDWR | O_CREAT | O_EXCL, 0666);
      if (fd >= 0 || errno != EEXIST)
         return fd;
   }
   memcpy(x, "XXXXXX", 6);
   errno = EEXIST;
   return -1;
}

/* ------------------------------------------------------------------- syslog */

void
ps5_openlog(const char *ident, int option, int facility)
{
   (void)ident;
   (void)option;
   (void)facility;
}

/* ---------------------------------------------------------------- processes */

FILE *
ps5_popen(const char *command, const char *mode)
{
   (void)command;
   (void)mode;
   errno = ENOSYS;
   return NULL;
}

int
ps5_pclose(FILE *stream)
{
   (void)stream;
   errno = ECHILD;
   return -1;
}

FILE *
ps5_open_memstream(char **buffer, size_t *size)
{
   (void)buffer;
   (void)size;
   errno = ENOSYS;
   return NULL;
}

/* -------------------------------------------------------------------- uname */

/* libkernel's sysctl, with FreeBSD's names, declared here rather than taken
 * from <sys/sysctl.h> so the host tests can model it (tests/host_kernel.c). */
int sysctl(const int *name, unsigned int length, void *old_value, size_t *old_length,
           const void *new_value, size_t new_length);
#define PS5_CTL_KERN 1
#define PS5_CTL_HW 6
#define PS5_KERN_OSTYPE 1
#define PS5_KERN_OSRELEASE 2
#define PS5_KERN_VERSION 4
#define PS5_KERN_HOSTNAME 10
#define PS5_HW_MACHINE 1

/* FreeBSD's struct utsname: five fields of length bytes each. */
static void
uname_field(char *field, int length, int top, int second, const char *fallback)
{
   int name[2] = {top, second};
   size_t size = (size_t)length;
   if (sysctl(name, 2, field, &size, NULL, 0) != 0 || size == 0) {
      strncpy(field, fallback, (size_t)length - 1);
      field[length - 1] = '\0';
      return;
   }
   field[size < (size_t)length ? size : (size_t)length - 1] = '\0';
   /* kern.version ends with a newline, as FreeBSD's uname strips. */
   char *const newline = strchr(field, '\n');
   if (newline)
      *newline = '\0';
}

int
ps5___xuname(int length, void *names)
{
   if (length <= 0 || !names) {
      errno = EFAULT;
      return -1;
   }
   char *const field = names;
   uname_field(field + 0 * length, length, PS5_CTL_KERN, PS5_KERN_OSTYPE, "PlayStation");
   uname_field(field + 1 * length, length, PS5_CTL_KERN, PS5_KERN_HOSTNAME, "ps5");
   uname_field(field + 2 * length, length, PS5_CTL_KERN, PS5_KERN_OSRELEASE, "");
   uname_field(field + 3 * length, length, PS5_CTL_KERN, PS5_KERN_VERSION, "");
   uname_field(field + 4 * length, length, PS5_CTL_HW, PS5_HW_MACHINE, "amd64");
   return 0;
}

/* ----------------------------------------------------------------- checking */

void
ps5___assert(const char *function, const char *file, int line, const char *expression)
{
   if (function)
      fprintf(stderr, "Assertion failed: (%s), function %s, file %s, line %d.\n", expression, function,
              file, line);
   else
      fprintf(stderr, "Assertion failed: (%s), file %s, line %d.\n", expression, file, line);
   fflush(stderr);
   abort();
}

void *
ps5___memset_chk(void *destination, int value, size_t length, size_t destination_length)
{
   if (length > destination_length) {
      fprintf(stderr, "memset of %zu bytes into an object of %zu\n", length, destination_length);
      fflush(stderr);
      abort();
   }
   return memset(destination, value, length);
}

/* ------------------------------------------------------------------- dladdr */

int
ps5_dladdr(const void *address, void *info)
{
   (void)address;
   (void)info;
   return 0;
}
