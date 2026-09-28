/*
 * PS5 Platform - the libc gaps outside directories (include/ps5platform/libc.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE 1

#include "ps5platform/libc.h"

#include "at.h"
#include "ps5platform/kernel.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#if __has_include(<sys/sockio.h>)
#include <sys/sockio.h> /* SIOCATMARK on FreeBSD; <sys/socket.h> has it elsewhere */
#endif
#include <sys/time.h>
#include <sys/times.h>
#include <unistd.h>

/* gmtime and localtime return one static buffer each; the copy is taken under
 * a lock, so concurrent callers each get their own conversion. */
static pthread_mutex_t time_lock = PTHREAD_MUTEX_INITIALIZER;

struct tm *
ps5_gmtime_r(const time_t *time, struct tm *result)
{
   pthread_mutex_lock(&time_lock);
   const struct tm *const shared = gmtime(time);
   if (shared)
      memcpy(result, shared, sizeof(*result));
   pthread_mutex_unlock(&time_lock);
   return shared ? result : NULL;
}

struct tm *
ps5_localtime_r(const time_t *time, struct tm *result)
{
   pthread_mutex_lock(&time_lock);
   const struct tm *const shared = localtime(time);
   if (shared)
      memcpy(result, shared, sizeof(*result));
   pthread_mutex_unlock(&time_lock);
   return shared ? result : NULL;
}

static atomic_uint_fast64_t random_state;

uint32_t
ps5_arc4random(void)
{
   uint64_t state = atomic_load_explicit(&random_state, memory_order_relaxed);
   uint64_t next;
   do {
      /* The first call seeds from the timestamp counter; the stored state is
       * what the exchange compares against, so it stays the zero it was. */
      next = (state != 0 ? state : sceKernelReadTsc() | 1u) + UINT64_C(0x9e3779b97f4a7c15);
   } while (!atomic_compare_exchange_weak_explicit(&random_state, &state, next,
                                                   memory_order_relaxed, memory_order_relaxed));
   uint64_t z = next;
   z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
   z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
   return (uint32_t)(z ^ (z >> 31));
}

void
ps5_arc4random_buf(void *buffer, size_t bytes)
{
   unsigned char *out = buffer;
   while (bytes > 0) {
      const uint32_t value = ps5_arc4random();
      const size_t chunk = bytes < sizeof(value) ? bytes : sizeof(value);
      memcpy(out, &value, chunk);
      out += chunk;
      bytes -= chunk;
   }
}

uint32_t
ps5_arc4random_uniform(uint32_t bound)
{
   if (bound < 2)
      return 0;
   /* Rejection sampling: no bias toward the low values. */
   const uint32_t floor = (uint32_t)(-bound) % bound;
   uint32_t value;
   do
      value = ps5_arc4random();
   while (value < floor);
   return value % bound;
}

static void
fill_statvfs(struct statvfs *result)
{
   memset(result, 0, sizeof(*result));
   result->f_bsize = 32768;
   result->f_frsize = 32768;
   result->f_blocks = (fsblkcnt_t)((UINT64_C(64) << 30) / 32768);
   result->f_bfree = (fsblkcnt_t)((UINT64_C(16) << 30) / 32768);
   result->f_bavail = result->f_bfree;
   result->f_namemax = 255;
}

int
ps5_getpwuid_r(uid_t uid, struct passwd *entry, char *buffer, size_t size, struct passwd **result)
{
   (void)uid;
   (void)entry;
   (void)buffer;
   (void)size;
   if (result)
      *result = NULL;
   return 0;
}

struct passwd *
ps5_getpwuid(uid_t uid)
{
   (void)uid;
   errno = 0;
   return NULL;
}

void *
ps5_memccpy(void *destination, const void *source, int c, size_t size)
{
   unsigned char *to = destination;
   const unsigned char *from = source;
   for (size_t i = 0; i < size; i++) {
      to[i] = from[i];
      if (from[i] == (unsigned char)c)
         return to + i + 1;
   }
   return NULL;
}

/* times() counts in FreeBSD's tick, which the SDK's <time.h> gives callers as
 * CLK_TCK; a host build of the tests has no such macro. */
#ifndef CLK_TCK
#define CLK_TCK 128
#endif

clock_t
ps5_times(struct tms *buffer)
{
   struct timespec cpu, now;
   if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) != 0 || clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return (clock_t)-1;
   if (buffer) {
      buffer->tms_utime = (clock_t)(cpu.tv_sec * CLK_TCK + cpu.tv_nsec / (1000000000 / CLK_TCK));
      buffer->tms_stime = 0;
      buffer->tms_cutime = 0;
      buffer->tms_cstime = 0;
   }
   return (clock_t)(now.tv_sec * CLK_TCK + now.tv_nsec / (1000000000 / CLK_TCK));
}

int
ps5_sockatmark(int fd)
{
   int mark = 0;
   return ioctl(fd, SIOCATMARK, &mark) == -1 ? -1 : mark != 0;
}

int
ps5_posix_fallocate(int fd, off_t offset, off_t length)
{
   if (offset < 0 || length <= 0)
      return EINVAL;
   struct stat status;
   if (fstat(fd, &status) != 0)
      return errno;
   if (!S_ISREG(status.st_mode))
      return ENODEV;
   const off_t end = offset + length;
   if (end < offset)
      return EFBIG;

   static const char zeros[16384];
   for (off_t at = status.st_size; at < end;) {
      const size_t piece = (size_t)(end - at < (off_t)sizeof(zeros) ? end - at : (off_t)sizeof(zeros));
      const ssize_t written = pwrite(fd, zeros, piece, at);
      if (written < 0) {
         if (errno == EINTR)
            continue;
         return errno;
      }
      at += written;
   }
   return 0;
}

int
ps5_access(const char *path, int mode)
{
   struct stat status;
   if (!path) {
      errno = EFAULT;
      return -1;
   }
   if (mode & ~(R_OK | W_OK | X_OK)) {
      errno = EINVAL;
      return -1;
   }
   if (stat(path, &status) != 0)
      return -1;
   if ((mode & X_OK) && !(status.st_mode & 0111)) {
      errno = EACCES;
      return -1;
   }
   if (S_ISDIR(status.st_mode)) {
      if ((mode & W_OK) && !(status.st_mode & 0222)) {
         errno = EACCES;
         return -1;
      }
      if (mode & R_OK) {
         const int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
         if (fd < 0)
            return -1;
         close(fd);
      }
      return 0;
   }
   if (mode & (R_OK | W_OK)) {
      const int how = (mode & R_OK) && (mode & W_OK) ? O_RDWR : (mode & W_OK) ? O_WRONLY : O_RDONLY;
      const int fd = open(path, how | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
      if (fd < 0)
         return -1;
      close(fd);
   }
   return 0;
}

char *
ps5_strcasestr(const char *haystack, const char *needle)
{
   for (;; haystack++) {
      size_t i = 0;
      while (needle[i] && tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[i]))
         i++;
      if (!needle[i])
         return (char *)haystack;
      if (!*haystack)
         return NULL;
   }
}

int
ps5_statvfs(const char *path, struct statvfs *result)
{
   struct stat status;
   if (!path || !result) {
      errno = EFAULT;
      return -1;
   }
   if (stat(path, &status) != 0)
      return -1;
   fill_statvfs(result);
   return 0;
}

int
ps5_fstatvfs(int fd, struct statvfs *result)
{
   struct stat status;
   if (!result) {
      errno = EFAULT;
      return -1;
   }
   if (fstat(fd, &status) != 0)
      return -1;
   fill_statvfs(result);
   return 0;
}

/* Both times given, or both "now": what std::filesystem::last_write_time and
 * the other callers pass. Omitting one time needs the file's current value,
 * which utimes cannot keep, so it is refused rather than approximated. */
static int
to_timevals(const struct timespec times[2], struct timeval values[2], bool *now)
{
   *now = times == NULL || (times[0].tv_nsec == UTIME_NOW && times[1].tv_nsec == UTIME_NOW);
   if (*now)
      return 0;
   for (int index = 0; index < 2; index++) {
      if (times[index].tv_nsec == UTIME_OMIT || times[index].tv_nsec == UTIME_NOW ||
          times[index].tv_nsec < 0 || times[index].tv_nsec >= 1000000000L) {
         errno = times[index].tv_nsec == UTIME_OMIT || times[index].tv_nsec == UTIME_NOW ? ENOSYS
                                                                                          : EINVAL;
         return -1;
      }
      values[index].tv_sec = times[index].tv_sec;
      values[index].tv_usec = times[index].tv_nsec / 1000;
   }
   return 0;
}

int
ps5_utimensat(int directory, const char *path, const struct timespec times[2], int flags)
{
   /* AT_SYMLINK_NOFOLLOW is not honoured: utimes follows links. */
   (void)flags;
   char resolved[1024];
   if (ps5p_resolve_at(directory, path, resolved, sizeof(resolved)) != 0)
      return -1;
   struct timeval values[2];
   bool now = false;
   if (to_timevals(times, values, &now) != 0)
      return -1;
   return utimes(resolved, now ? NULL : values);
}

int
ps5_futimens(int fd, const struct timespec times[2])
{
   struct timeval values[2];
   bool now = false;
   if (to_timevals(times, values, &now) != 0)
      return -1;
   return futimes(fd, now ? NULL : values);
}

int
ps5_clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
                    struct timespec *remaining)
{
   if (!request)
      return EFAULT;
   struct timespec wait = *request;
   if (flags & TIMER_ABSTIME) {
      struct timespec now;
      if (clock_gettime(clock, &now) != 0)
         return errno;
      const long long left = ((long long)request->tv_sec - now.tv_sec) * 1000000000LL +
                             (request->tv_nsec - now.tv_nsec);
      if (left <= 0)
         return 0;
      wait.tv_sec = (time_t)(left / 1000000000LL);
      wait.tv_nsec = (long)(left % 1000000000LL);
      /* An absolute sleep reports no remainder: the deadline is unchanged. */
      remaining = NULL;
   }
   return nanosleep(&wait, remaining) == 0 ? 0 : errno;
}

int
ps5_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **result)
{
   (void)node;
   (void)service;
   (void)hints;
   if (result)
      *result = NULL;
   return EAI_FAIL;
}

void
ps5_freeaddrinfo(struct addrinfo *info)
{
   (void)info;
}

struct hostent *
ps5_gethostbyaddr(const void *address, unsigned int length, int type)
{
   (void)address;
   (void)length;
   (void)type;
   h_errno = HOST_NOT_FOUND;
   return NULL;
}

struct if_nameindex *
ps5_if_nameindex(void)
{
   errno = ENOSYS;
   return NULL;
}

void
ps5_if_freenameindex(struct if_nameindex *list)
{
   (void)list;
}
