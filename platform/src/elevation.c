/*
 * PS5 Platform - /data for a sandboxed title, by asking the Lapy daemon (elevation.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Derived from the cooperative elevation client of ps5-native-app-boilerplate
 * (src/elevation.cpp, Copyright (C) 2026 BlackBearReloaded, GPL-3.0-or-later), which
 * implements the application side of mpereiraesaa/PS5-Lapy-JB-Daemon's contract. The
 * steps are that client's: preopen the result file (the path may not be visible after
 * the root changes), make the process's credentials the private shape Lapy needs with
 * seteuid(geteuid()), publish {"PID":n} as the request, atomically, then poll for
 * /data to work with a real write/read/compare of a probe file. The disappearance of
 * the request is not taken as success: it proves only that the daemon read it.
 */
#include "ps5platform/elevation.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PROBE_TOKEN "LAPYOWN\n"

static bool
write_all(int fd, const char *bytes, size_t length)
{
   while (length) {
      const ssize_t count = write(fd, bytes, length);

      if (count <= 0 || (size_t)count > length)
         return false;
      bytes += count;
      length -= (size_t)count;
   }
   return true;
}

/* {"PID":n}\n in <directory>/.elevate_proc.<n>, then renamed over the request path: the
 * daemon never sees a half-written file. */
static bool
publish_request(const struct ps5_elevation_config *config, pid_t pid)
{
   char temporary[256], body[64];
   int body_length, path_length, fd, saved;

   body_length = snprintf(body, sizeof(body), "{\"PID\":%ld}\n", (long)pid);
   path_length = snprintf(temporary, sizeof(temporary), "%s/.elevate_proc.%ld", config->request_directory, (long)pid);
   if (body_length <= 0 || (size_t)body_length >= sizeof(body) || path_length <= 0 ||
       (size_t)path_length >= sizeof(temporary)) {
      errno = EOVERFLOW;
      return false;
   }
   (void)unlink(temporary);
   fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644);
   if (fd < 0 || !write_all(fd, body, (size_t)body_length)) {
      saved = errno ? errno : EIO;
      if (fd >= 0)
         (void)close(fd);
      (void)unlink(temporary);
      errno = saved;
      return false;
   }
   if (close(fd) != 0 || rename(temporary, config->request_path) != 0) {
      saved = errno ? errno : EIO;
      (void)unlink(temporary);
      errno = saved;
      return false;
   }
   return true;
}

/* Create, write, seek, read, compare and remove a probe file under /data. True when it all
 * worked. *open_error is the errno of the open (0 when it opened), which tells "not
 * reachable yet" (keep polling) from "reachable and wrong" (stop). */
static bool
verify_data(const struct ps5_elevation_config *config, pid_t pid, int *open_error)
{
   char path[256], actual[sizeof(PROBE_TOKEN) - 1];
   const size_t token_length = sizeof(PROBE_TOKEN) - 1;
   int length, fd;
   bool passed;

   length = snprintf(path, sizeof(path), "%s/.lapy_probe_%ld", config->data_directory, (long)pid);
   if (length <= 0 || (size_t)length >= sizeof(path)) {
      *open_error = EOVERFLOW;
      return false;
   }
   (void)unlink(path);
   fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
   *open_error = fd < 0 ? errno : 0;
   passed = fd >= 0 && write_all(fd, PROBE_TOKEN, token_length) && lseek(fd, 0, SEEK_SET) == 0 &&
            read(fd, actual, sizeof(actual)) == (ssize_t)token_length && !memcmp(actual, PROBE_TOKEN, token_length);
   if (fd >= 0)
      (void)close(fd);
   (void)unlink(path);
   return passed;
}

static bool
wait_for_elevation(const struct ps5_elevation_config *config, pid_t pid, int *open_error, bool *proof_failed)
{
   for (unsigned poll = 0; poll < config->polls; ++poll) {
      if (verify_data(config, pid, open_error))
         return true;
      if (*open_error == 0) {
         *proof_failed = true;
         return false;
      }
      (void)usleep(config->poll_interval_us);
   }
   return false;
}

enum ps5_elevation_status
ps5_elevation_request_with(const struct ps5_elevation_config *config, enum ps5_elevation_capability capability)
{
   char report[96];
   int result, open_error = 0, length;
   bool proof_failed = false, data_ok;
   pid_t pid;

   if (capability != PS5_ELEVATION_FILESYSTEM)
      return PS5_ELEVATION_UNSUPPORTED_CAPABILITY;
   result = open(config->result_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
   if (result < 0)
      return PS5_ELEVATION_UNAVAILABLE;
   (void)fchmod(result, 0644);
   if (seteuid(geteuid()) != 0) {
      (void)close(result);
      return PS5_ELEVATION_PREPARE_FAILED;
   }
   pid = getpid();
   if (pid <= 1 || !publish_request(config, pid)) {
      (void)close(result);
      return PS5_ELEVATION_TRANSPORT_ERROR;
   }
   data_ok = wait_for_elevation(config, pid, &open_error, &proof_failed);
   length = snprintf(report, sizeof(report), "DATA_OK=%d OPEN_ERRNO=%d\n", data_ok, open_error);
   if (length > 0 && (size_t)length < sizeof(report))
      (void)write_all(result, report, (size_t)length);
   (void)close(result);
   return data_ok ? PS5_ELEVATION_OK : proof_failed ? PS5_ELEVATION_APPLY_FAILED : PS5_ELEVATION_TIMEOUT;
}

enum ps5_elevation_status
ps5_elevation_request(enum ps5_elevation_capability capability)
{
   static const struct ps5_elevation_config contract = {
      "/download0/elevate_proc", "/download0", "/download0/lapy_owned_result", "/data", 200, 50000,
   };

   return ps5_elevation_request_with(&contract, capability);
}

const char *
ps5_elevation_status_name(enum ps5_elevation_status status)
{
   switch (status) {
   case PS5_ELEVATION_OK: return "ok";
   case PS5_ELEVATION_INVALID_REQUEST: return "invalid_request";
   case PS5_ELEVATION_UNSUPPORTED_CAPABILITY: return "unsupported_capability";
   case PS5_ELEVATION_UNAVAILABLE: return "unavailable";
   case PS5_ELEVATION_PREPARE_FAILED: return "prepare_failed";
   case PS5_ELEVATION_APPLY_FAILED: return "apply_failed";
   case PS5_ELEVATION_TRANSPORT_ERROR: return "transport_error";
   case PS5_ELEVATION_TIMEOUT: return "timeout";
   }
   return "unknown";
}
