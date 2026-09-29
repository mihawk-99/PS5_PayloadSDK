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
