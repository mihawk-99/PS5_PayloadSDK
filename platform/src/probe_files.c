/*
 * PS5 Platform - the file probe.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What a file in the caller's directory costs to write and read, which is what
 * a save state, a memory card, a screenshot or a shader cache pays. A title's
 * save of a 50 MB state took 2.1-2.9 s whatever RetroArch's chunk size, about
 * 18 MB/s (the RetroArch title's PHASE_LOG, LRPS2's Profile 10), and this says
 * whether that is the storage or the way the file is written.
 *
 * 256 MiB are written in chunks of 100 KiB (RetroArch's save-state chunk),
 * 1 MiB and 16 MiB, timed to the last write() and again after fsync(), then
 * read back in the same chunks and compared word for word; the 16 MiB chunk
 * is tried once more with O_DIRECT. The same 256 MiB are then written
 * through stdio, fwrite() of 16 MiB at a time, with the stream's own buffer
 * and with setvbuf() buffers of 1 MiB and 4 MiB: RetroArch writes its files
 * that way, and FreeBSD's fwrite() writes large data directly but one buffer's
 * worth per write(). The file is the probe's own, ps5-platform-probe.tmp,
 * removed after each pass. Only libc is used, so the host tests run it too.
 */
#include "ps5platform/probe.h"
#include "ps5platform/ftp.h"
#include "ps5platform/libc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#if defined(__FreeBSD__)
#include <sys/mount.h>
int _fstatfs(int fd, struct statfs *buf);
#endif
#include <time.h>
#include <unistd.h>

#define FILE_PROBE_BYTES (256u * 1024u * 1024u)
#define FILE_PROBE_CHUNK_MAX (16u * 1024u * 1024u)
#define FILE_PROBE_NAME "ps5-platform-probe.tmp"

struct file_probe {
   ps5_probe_log_fn log;
   void *context;
   int failures;
};

static void
file_say(struct file_probe *p, const char *format, ...)
{
   char line[512];
   va_list arguments;
   va_start(arguments, format);
   int used = snprintf(line, sizeof(line), "platform-probe: files ");
   vsnprintf(line + used, sizeof(line) - (size_t)used, format, arguments);
   va_end(arguments);
   p->log(p->context, line);
}

static double
file_ms_since(const struct timespec *start)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (double)(now.tv_sec - start->tv_sec) * 1e3 + (double)(now.tv_nsec - start->tv_nsec) / 1e6;
}

/* The word at byte offset `at` of the file: its own offset, mixed, so a chunk
 * read from the wrong place shows. */
static uint64_t
file_word(uint64_t at)
{
   return (at + 1u) * 0x9e3779b97f4a7c15ull;
}

static void
file_fill(uint64_t *words, uint64_t at, size_t bytes)
{
   for (size_t i = 0; i < bytes / sizeof(uint64_t); i++)
      words[i] = file_word(at + i * sizeof(uint64_t));
}

static bool
file_matches(const uint64_t *words, uint64_t at, size_t bytes)
{
   for (size_t i = 0; i < bytes / sizeof(uint64_t); i++)
      if (words[i] != file_word(at + i * sizeof(uint64_t)))
         return false;
   return true;
}

/* One pass: write the file in `chunk` bytes, fsync, read it back and compare.
 * False when a call failed or the bytes differ, with the reason logged. */
static bool
file_pass(struct file_probe *p, const char *path, uint64_t *buffer, size_t chunk, int extra_flags,
          const char *label)
{
   int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | extra_flags, 0666);
   if (fd < 0) {
      file_say(p, "%s chunk=%zu open for write errno=%d", label, chunk, errno);
      return false;
   }
   struct timespec start;
   clock_gettime(CLOCK_MONOTONIC, &start);
   bool ok = true;
   for (uint64_t at = 0; ok && at < FILE_PROBE_BYTES; at += chunk) {
      file_fill(buffer, at, chunk);
      ok = write(fd, buffer, chunk) == (ssize_t)chunk;
   }
   const double written_ms = file_ms_since(&start);
   const int synced = ok ? fsync(fd) : -1;
   const double synced_ms = file_ms_since(&start);
   close(fd);
   if (!ok || synced != 0) {
      file_say(p, "%s chunk=%zu write or fsync failed errno=%d", label, chunk, errno);
      unlink(path);
      return false;
   }
   fd = open(path, O_RDONLY | extra_flags);
   if (fd < 0) {
      file_say(p, "%s chunk=%zu open for read errno=%d", label, chunk, errno);
      unlink(path);
      return false;
   }
   clock_gettime(CLOCK_MONOTONIC, &start);
   bool same = true;
   double read_only_ms = 0.0;
   for (uint64_t at = 0; ok && at < FILE_PROBE_BYTES; at += chunk) {
      struct timespec before;
      clock_gettime(CLOCK_MONOTONIC, &before);
      ok = read(fd, buffer, chunk) == (ssize_t)chunk;
      read_only_ms += file_ms_since(&before);
      same = same && ok && file_matches(buffer, at, chunk);
   }
   close(fd);
   unlink(path);
   const double mib = (double)FILE_PROBE_BYTES / (1024.0 * 1024.0);
   file_say(p,
            "%s chunk=%zu write %.1f MiB/s (%.0f ms, %.0f ms with fsync) read %.1f MiB/s "
            "(%.0f ms) %s",
            label, chunk, mib * 1e3 / written_ms, written_ms, synced_ms,
            mib * 1e3 / read_only_ms, read_only_ms, same ? "same" : "DIFFERENT");
   return ok && same;
}

/* One stdio pass: fopen("wb"), a buffer of `buffer_bytes` (0: the stream's own),
 * fwrite() of FILE_PROBE_CHUNK_MAX at a time, fclose(); the file is read back
 * with read() and compared. */
static bool
file_stdio_pass(struct file_probe *p, const char *path, uint64_t *buffer, size_t buffer_bytes)
{
   FILE *const file = fopen(path, "wb");
   if (file == NULL) {
      file_say(p, "stdio buffer=%zu fopen errno=%d", buffer_bytes, errno);
      return false;
   }
   const int buffered = buffer_bytes ? setvbuf(file, NULL, _IOFBF, buffer_bytes) : 0;
   struct timespec start;
   clock_gettime(CLOCK_MONOTONIC, &start);
   bool ok = true;
   for (uint64_t at = 0; ok && at < FILE_PROBE_BYTES; at += FILE_PROBE_CHUNK_MAX) {
      file_fill(buffer, at, FILE_PROBE_CHUNK_MAX);
      ok = fwrite(buffer, 1, FILE_PROBE_CHUNK_MAX, file) == FILE_PROBE_CHUNK_MAX;
   }
   ok = fclose(file) == 0 && ok;
   const double written_ms = file_ms_since(&start);
   bool same = false;
   const int fd = ok ? open(path, O_RDONLY) : -1;
   if (fd >= 0) {
      same = true;
      for (uint64_t at = 0; same && at < FILE_PROBE_BYTES; at += FILE_PROBE_CHUNK_MAX)
         same = read(fd, buffer, FILE_PROBE_CHUNK_MAX) == (ssize_t)FILE_PROBE_CHUNK_MAX &&
                file_matches(buffer, at, FILE_PROBE_CHUNK_MAX);
      close(fd);
   }
   unlink(path);
   const double mib = (double)FILE_PROBE_BYTES / (1024.0 * 1024.0);
   file_say(p, "stdio buffer=%zu setvbuf=%d write %.1f MiB/s (%.0f ms) %s", buffer_bytes,
            buffered, mib * 1e3 / written_ms, written_ms, same ? "same" : "DIFFERENT");
   return ok && same;
}

int
ps5_platform_probe_files(ps5_probe_log_fn log, void *context, const char *directory)
{
   struct file_probe p = {.log = log, .context = context};
   char path[512];
   if (snprintf(path, sizeof(path), "%s/%s", directory, FILE_PROBE_NAME) >= (int)sizeof(path)) {
      file_say(&p, "directory name too long");
      return 1;
   }
   /* A file of this name is only ever the probe's own, left by a run that
    * stopped before its unlink. */
   unlink(path);
   /* The buffer is mapped, not allocated: the console's libc heap refused
    * aligned_alloc of 16 MiB in a title with 400 MiB of flexible memory free. */
   void *const mapped = mmap(NULL, FILE_PROBE_CHUNK_MAX, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON, -1, 0);
   if (mapped == MAP_FAILED) {
      file_say(&p, "no memory for a %u byte buffer, errno=%d", FILE_PROBE_CHUNK_MAX, errno);
      return 1;
   }
   uint64_t *const buffer = mapped;
   file_say(&p, "begin directory=%s bytes=%u", directory, FILE_PROBE_BYTES);
   /* What realpath answers a title (std::filesystem's canonical paths are
    * built on it): the directory, a path below it that does not exist, and
    * one with a "." and a "..". */
   const char *const resolve[3] = {directory, "missing-probe-entry", "./x/.."};
   for (unsigned i = 0; i < 3; i++) {
      char asked[600], answer[1024];
      if (i == 0)
         snprintf(asked, sizeof(asked), "%s", directory);
      else
         snprintf(asked, sizeof(asked), "%s/%s", directory, resolve[i]);
      memset(answer, 0x55, sizeof(answer));
      errno = 0;
      const char *const got = realpath(asked, answer);
      file_say(&p, "realpath '%s' -> %s '%.200s' errno=%d", asked, got ? "ok" : "NULL",
               got ? got : "", errno);
   }
   char resolved[1024];
   const bool resolves = ps5_realpath(directory, resolved) && !strcmp(resolved, directory);
   p.failures += resolves ? 0 : 1;
   file_say(&p, "check %s the platform's realpath resolves %s", resolves ? "PASS" : "FAIL", directory);
   static const size_t chunks[] = {100u * 1024u, 1024u * 1024u, FILE_PROBE_CHUNK_MAX};
   for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
      const bool passed = file_pass(&p, path, buffer, chunks[i], 0, "buffered");
      p.failures += passed ? 0 : 1;
      file_say(&p, "check %s buffered chunk=%zu reads back what it wrote",
               passed ? "PASS" : "FAIL", chunks[i]);
   }
#ifdef O_DIRECT
   /* Informational: a file system may refuse direct I/O, which is an answer. */
   (void)file_pass(&p, path, buffer, FILE_PROBE_CHUNK_MAX, O_DIRECT, "direct");
#endif
   static const size_t stdio_buffers[] = {0, 1024u * 1024u, 4u * 1024u * 1024u};
   for (size_t i = 0; i < sizeof(stdio_buffers) / sizeof(stdio_buffers[0]); i++) {
      const bool passed = file_stdio_pass(&p, path, buffer, stdio_buffers[i]);
      p.failures += passed ? 0 : 1;
      file_say(&p, "check %s stdio buffer=%zu reads back what it wrote",
               passed ? "PASS" : "FAIL", stdio_buffers[i]);
   }
   munmap(mapped, FILE_PROBE_CHUNK_MAX);
   file_say(&p, "end failures=%d", p.failures);
   return p.failures;
}

/* One sustained pass: false when a write fails. */
static bool
writes_pass(struct file_probe *p, const char *path, uint64_t *buffer, unsigned mib, unsigned seconds,
            int flags, bool sync_segments, const char *how)
{
   const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | flags, 0666);
   if (fd < 0) {
      file_say(p, "writes %s: open failed errno=%d", how, errno);
      return flags != 0; /* a refused O_DIRECT is an answer, not a failure */
   }
   const uint64_t segment = 256u * 1024u * 1024u, total = (uint64_t)mib * 1024u * 1024u;
   struct timespec start, mark;
   clock_gettime(CLOCK_MONOTONIC, &start);
   mark = start;
   uint64_t at = 0;
   bool ok = true;
   while (at < total) {
      const size_t chunk = (size_t)(total - at < FILE_PROBE_CHUNK_MAX ? total - at : FILE_PROBE_CHUNK_MAX);
      file_fill(buffer, at, chunk);
      if (write(fd, buffer, chunk) != (ssize_t)chunk) {
         file_say(p, "writes %s: write at %llu failed errno=%d", how, (unsigned long long)at, errno);
         ok = false;
         break;
      }
      at += chunk;
      if (at % segment == 0 || at == total) {
         if (sync_segments)
            fsync(fd);
         const double ms = file_ms_since(&mark);
         const uint64_t bytes = at % segment ? at % segment : segment;
         file_say(p, "writes %s: %llu MiB, this segment %.1f MiB/s", how,
                  (unsigned long long)(at >> 20), (double)bytes / 1048576.0 / (ms / 1000.0));
         clock_gettime(CLOCK_MONOTONIC, &mark);
         if (file_ms_since(&start) > seconds * 1000.0) {
            file_say(p, "writes %s: stopped after %u s", how, seconds);
            break;
         }
      }
   }
   const double written_ms = file_ms_since(&start);
   fsync(fd);
   const double synced_ms = file_ms_since(&start);
   close(fd);
   unlink(path);
   file_say(p, "writes %s: %llu MiB, %.1f MiB/s to the last write, %.1f MiB/s after fsync", how,
            (unsigned long long)(at >> 20), (double)at / 1048576.0 / (written_ms / 1000.0),
            (double)at / 1048576.0 / (synced_ms / 1000.0));
   return ok;
}

/* One mapped pass: the file sized first, then written through MAP_SHARED
 * windows of 64 MiB with no write() at all, each unmapped when full; fsync()
 * after the last. The kernel writes the dirty pages back itself. */
static bool
mapped_pass(struct file_probe *p, const char *path, unsigned mib, unsigned seconds)
{
   const int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
   if (fd < 0) {
      file_say(p, "routes mmap: open failed errno=%d", errno);
      return false;
   }
   const uint64_t window = 64u * 1024u * 1024u, segment = 256u * 1024u * 1024u;
   const uint64_t total = (uint64_t)mib * 1024u * 1024u;
   if (ftruncate(fd, (off_t)total) != 0) {
      file_say(p, "routes mmap: ftruncate to %u MiB failed errno=%d", mib, errno);
      close(fd);
      unlink(path);
      return false;
   }
   struct timespec start, mark;
   clock_gettime(CLOCK_MONOTONIC, &start);
   mark = start;
   uint64_t at = 0;
   bool ok = true;
   while (at < total) {
      const size_t size = (size_t)(total - at < window ? total - at : window);
      void *const view = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)at);
      if (view == MAP_FAILED) {
         file_say(p, "routes mmap: mmap at %llu failed errno=%d", (unsigned long long)at, errno);
         ok = false;
         break;
      }
      file_fill(view, at, size);
      munmap(view, size);
      at += size;
      if (at % segment == 0 || at == total) {
         const double ms = file_ms_since(&mark);
         const uint64_t bytes = at % segment ? at % segment : segment;
         file_say(p, "routes mmap: %llu MiB, this segment %.1f MiB/s", (unsigned long long)(at >> 20),
                  (double)bytes / 1048576.0 / (ms / 1000.0));
         clock_gettime(CLOCK_MONOTONIC, &mark);
         if (file_ms_since(&start) > seconds * 1000.0) {
            file_say(p, "routes mmap: stopped after %u s", seconds);
            break;
         }
      }
   }
   const double written_ms = file_ms_since(&start);
   fsync(fd);
   const double synced_ms = file_ms_since(&start);
   close(fd);
   unlink(path);
   file_say(p, "routes mmap: %llu MiB, %.1f MiB/s to the last unmap, %.1f MiB/s after fsync",
            (unsigned long long)(at >> 20), (double)at / 1048576.0 / (written_ms / 1000.0),
            (double)at / 1048576.0 / (synced_ms / 1000.0));
   return ok;
}

int
ps5_platform_probe_write_routes(ps5_probe_log_fn log, void *context, const char *const *directories,
                                unsigned count, unsigned mib, unsigned seconds)
{
   struct file_probe p = {.log = log, .context = context};
   void *const buffer = mmap(NULL, FILE_PROBE_CHUNK_MAX, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
   if (buffer == MAP_FAILED) {
      file_say(&p, "no memory for a %u byte buffer, errno=%d", FILE_PROBE_CHUNK_MAX, errno);
      return 1;
   }
   for (unsigned i = 0; i < count; ++i) {
      char path[512];
      if (snprintf(path, sizeof(path), "%s/%s", directories[i], FILE_PROBE_NAME) >= (int)sizeof(path)) {
         file_say(&p, "directory name too long");
         p.failures++;
         continue;
      }
      file_say(&p, "routes begin directory=%s mib=%u seconds=%u", directories[i], mib, seconds);
      unlink(path);
      /* write() first, long enough to spend a title's burst; then the mapped
       * route; then write() again, which shows whether the mapped route spent
       * what write() is allowed. */
      p.failures += writes_pass(&p, path, buffer, mib, seconds, 0, false, "buffered") ? 0 : 1;
      p.failures += mapped_pass(&p, path, mib, seconds) ? 0 : 1;
      p.failures += writes_pass(&p, path, buffer, mib, seconds / 3 ? seconds / 3 : 1, 0, false, "buffered") ? 0 : 1;
   }
   munmap(buffer, FILE_PROBE_CHUNK_MAX);
   file_say(&p, "routes end failures=%d", p.failures);
   return p.failures;
}

int
ps5_platform_probe_writes(ps5_probe_log_fn log, void *context, const char *directory, unsigned mib,
                          unsigned seconds)
{
   struct file_probe p = {.log = log, .context = context};
   char path[512];
   if (snprintf(path, sizeof(path), "%s/%s", directory, FILE_PROBE_NAME) >= (int)sizeof(path)) {
      file_say(&p, "directory name too long");
      return 1;
   }
   unlink(path);
   void *const mapped = mmap(NULL, FILE_PROBE_CHUNK_MAX, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON, -1, 0);
   if (mapped == MAP_FAILED) {
      file_say(&p, "no memory for a %u byte buffer, errno=%d", FILE_PROBE_CHUNK_MAX, errno);
      return 1;
   }
   file_say(&p, "writes begin directory=%s mib=%u seconds=%u", directory, mib, seconds);
#ifdef O_DIRECT
   p.failures += writes_pass(&p, path, mapped, mib, seconds, O_DIRECT, false, "direct") ? 0 : 1;
#endif
   p.failures += writes_pass(&p, path, mapped, mib, seconds, 0, true, "buffered+fsync") ? 0 : 1;
   p.failures += writes_pass(&p, path, mapped, mib, seconds, 0, false, "buffered") ? 0 : 1;
   munmap(mapped, FILE_PROBE_CHUNK_MAX);
   file_say(&p, "writes end failures=%d", p.failures);
   return p.failures;
}

/* Appends [at, total) of the probe's pattern to `server_path` through `ftp`,
 * timed per 256 MiB and stopped after `seconds`. Returns the end reached, or 0
 * when the server refused or did not confirm. */
static uint64_t
offload_send(struct file_probe *p, struct ps5_ftp *ftp, const char *server_path, uint64_t *buffer,
             uint64_t at, uint64_t total, unsigned seconds, size_t send_size, double pace_mibs, const char *how)
{
   const int data = ps5_ftp_append_begin(ftp, server_path);
   if (data < 0) {
      file_say(p, "offload %s: APPE %s refused (code %d: %s)", how, server_path, ftp->code, ftp->reply);
      return 0;
   }
   struct timespec start, mark;
   clock_gettime(CLOCK_MONOTONIC, &start);
   mark = start;
   const uint64_t segment = 256u * 1024u * 1024u, from = at;
   while (at < total) {
      const size_t chunk = (size_t)(total - at < FILE_PROBE_CHUNK_MAX ? total - at : FILE_PROBE_CHUNK_MAX);
      file_fill(buffer, at, chunk);
      bool sent = true;
      for (size_t done = 0; sent && done < chunk; done += send_size) {
         const size_t size = chunk - done < send_size ? chunk - done : send_size;
         sent = ps5_ftp_send(data, (const char *)buffer + done, size) == 0;
         /* A paced sender waits until its bytes so far fit the rate. */
         const double due_ms = (double)(at - from + done + size) / 1048576.0 / pace_mibs * 1000.0;
         while (pace_mibs > 0 && file_ms_since(&start) < due_ms)
            usleep(1000);
      }
      if (!sent) {
         file_say(p, "offload %s: send at %llu failed errno=%d", how, (unsigned long long)at, errno);
         break;
      }
      at += chunk;
      if (at % segment == 0 || at == total) {
         const double ms = file_ms_since(&mark);
         const uint64_t bytes = at % segment ? at % segment : segment;
         file_say(p, "offload %s: %llu MiB, this segment %.1f MiB/s", how, (unsigned long long)(at >> 20),
                  (double)bytes / 1048576.0 / (ms / 1000.0));
         clock_gettime(CLOCK_MONOTONIC, &mark);
         if (file_ms_since(&start) > seconds * 1000.0) {
            file_say(p, "offload %s: stopped after %u s", how, seconds);
            break;
         }
      }
   }
   const double sent_ms = file_ms_since(&start);
   const int confirmed = ps5_ftp_transfer_end(ftp, data);
   const double confirmed_ms = file_ms_since(&start);
   file_say(p, "offload %s: %llu MiB through the server, %.1f MiB/s sent, %.1f MiB/s confirmed (%s)", how,
            (unsigned long long)((at - from) >> 20), (double)(at - from) / 1048576.0 / (sent_ms / 1000.0),
            (double)(at - from) / 1048576.0 / (confirmed_ms / 1000.0), confirmed == 0 ? "226" : ftp->reply);
   return confirmed == 0 ? at : 0;
}

/* The descriptor sees `size` bytes of the pattern: its size, and the bytes at
 * the start, the middle and the end. */
static bool
offload_check(int fd, uint64_t size)
{
   struct stat status;
   if (fstat(fd, &status) != 0 || (uint64_t)status.st_size != size || size < 4096)
      return false;
   const uint64_t probes[3] = {0, size / 2 & ~(uint64_t)4095, size - 4096};
   for (int i = 0; i < 3; ++i) {
      uint64_t expected[512], seen[512];
      file_fill(expected, probes[i], sizeof(expected));
      if (pread(fd, seen, sizeof(seen), (off_t)probes[i]) != (ssize_t)sizeof(seen) ||
          memcmp(expected, seen, sizeof(seen)) != 0)
         return false;
   }
   return true;
}

int
ps5_platform_probe_ftp_offload(ps5_probe_log_fn log, void *context, const char *directory,
                               const char *server_directory, unsigned port, unsigned mib, unsigned seconds)
{
   struct file_probe p = {.log = log, .context = context};
#if defined(__FreeBSD__)
   /* FreeBSD's statfs() names what a directory is mounted from (a title's
    * /app0 is its folder, mounted into its sandbox). The console exports
    * neither statfs() nor fstatfs(), only _fstatfs(). */
   struct statfs mounted;
   memset(&mounted, 0, sizeof(mounted));
   const int opened = open(directory, O_RDONLY | O_DIRECTORY);
   const int stated = opened >= 0 ? _fstatfs(opened, &mounted) : -1;
   if (opened >= 0)
      close(opened);
   if (stated == 0)
      file_say(&p, "offload statfs %s: from=%s on=%s type=%s", directory, mounted.f_mntfromname,
               mounted.f_mntonname, mounted.f_fstypename);
   else
      file_say(&p, "offload statfs %s failed errno=%d", directory, errno);
   if (!server_directory)
      server_directory = mounted.f_mntfromname;
#else
   if (!server_directory)
      server_directory = directory;
   file_say(&p, "offload %s: the server names it %s", directory, server_directory);
#endif
   /* "/dev/null" as the server's folder sends the bytes to its null device:
    * the route's own rate, with no storage behind it (nothing to check). */
   const bool sink = !strcmp(server_directory, "/dev/null");
   char path[512], server_path[512];
   if (snprintf(path, sizeof(path), "%s/%s", directory, FILE_PROBE_NAME) >= (int)sizeof(path) ||
       snprintf(server_path, sizeof(server_path), sink ? "%s" : "%s/%s", server_directory, FILE_PROBE_NAME) >=
          (int)sizeof(server_path)) {
      file_say(&p, "offload: directory name too long");
      return 1;
   }
   if (!port) {
      struct timespec scan;
      clock_gettime(CLOCK_MONOTONIC, &scan);
      port = ps5_ftp_find_local(1, 65535, 300);
      file_say(&p, "offload: the loopback scan %s an FTP server in %.0f ms", port ? "found" : "did not find",
               file_ms_since(&scan));
   }
   uint64_t *const buffer =
      mmap(NULL, FILE_PROBE_CHUNK_MAX, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
   if (buffer == MAP_FAILED)
      return 1;
   struct ps5_ftp ftp;
   if (ps5_ftp_open(&ftp, port) != 0) {
      file_say(&p, "offload: no FTP server to log in to (code %d: %s)", ftp.code, ftp.reply);
      munmap(buffer, FILE_PROBE_CHUNK_MAX);
      return 1;
   }
   const uint64_t half = (uint64_t)mib * 1024u * 1024u / 2;

   /* The server's own file: it creates it, and the caller opens it after;
    * sent in 16 MiB pieces as fast as the route takes them, then paced at
    * 60 MiB/s in pieces of 64 KiB, 256 KiB and 1 MiB. The server writes what
    * each recv() gives it: a paced sender keeps those writes to its pieces. */
   static const struct {
      size_t send_size;
      double pace_mibs;
      const char *how;
   } ways[4] = {{FILE_PROBE_CHUNK_MAX, 0, "server's file"},
                {64u * 1024u, 60, "server's file, 64 KiB at 60 MiB/s"},
                {256u * 1024u, 60, "server's file, 256 KiB at 60 MiB/s"},
                {1024u * 1024u, 60, "server's file, 1 MiB at 60 MiB/s"}};
   uint64_t fresh = 0;
   bool fresh_ok = true;
   for (int way = 0; way < 4; ++way) {
      unlink(path);
      fresh = offload_send(&p, &ftp, server_path, buffer, 0, half, seconds / 5, ways[way].send_size,
                           ways[way].pace_mibs, ways[way].how);
      if (!sink) {
         const int fd = open(path, O_RDONLY);
         const bool ok = fresh && fd >= 0 && offload_check(fd, fresh);
         if (fd >= 0)
            close(fd);
         unlink(path);
         file_say(&p, "offload %s: the caller reads it back=%d", ways[way].how, ok);
         fresh_ok &= ok;
      }
   }

   /* The caller's own file: created, opened and its first 16 MiB written
    * here; the server appends the rest while the caller holds it open. */
   bool own_ok = sink;
   const int fd = sink ? -1 : open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
   struct stat before;
   const uint64_t first = FILE_PROBE_CHUNK_MAX;
   file_fill(buffer, 0, first);
   if (!sink && (fd < 0 || write(fd, buffer, first) != (ssize_t)first || fstat(fd, &before) != 0))
      file_say(&p, "offload caller's file: the local file failed errno=%d", errno);
   else if (!sink) {
      const uint64_t end = offload_send(&p, &ftp, server_path, buffer, first, first + half, seconds / 5,
                                        FILE_PROBE_CHUNK_MAX, 0, "caller's file");
      struct stat after;
      own_ok = end && fstat(fd, &after) == 0 && after.st_ino == before.st_ino && offload_check(fd, end);
      file_say(&p, "offload caller's file: the same file, whole=%d", own_ok);
   }
   if (fd >= 0)
      close(fd);
   if (!sink)
      unlink(path);
   ps5_ftp_close(&ftp);
   munmap(buffer, FILE_PROBE_CHUNK_MAX);
   return (fresh || sink ? 0 : 1) + (fresh_ok ? 0 : 1) + (own_ok ? 0 : 1);
}
