/*
 * PS5 Platform - the Lapy elevation client (src/elevation.c) against a model of the daemon.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The contract's files live in a temporary directory instead of /download0 and /data. The
 * model daemon takes the request the client publishes, checks it is the exact JSON of the
 * client's pid and that nothing else is there, and then makes /data appear, which is all
 * the real one does that a client can see. A host test cannot establish anything about
 * the kernel or the firmware; what it checks is the client's side of the contract.
 */
#define _GNU_SOURCE 1

#include "ps5platform/elevation.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                                        \
   do {                                                                                         \
      if (!(condition)) {                                                                       \
         fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);          \
         exit(1);                                                                               \
      }                                                                                         \
   } while (0)

static char root[128], download[160], data[160], request[200], result[200];
static struct ps5_elevation_config config;

static void
prepare(void)
{
   snprintf(root, sizeof(root), "/tmp/ps5-elevation-test-XXXXXX");
   CHECK(mkdtemp(root));
   snprintf(download, sizeof(download), "%s/download0", root);
   snprintf(data, sizeof(data), "%s/data", root);
   snprintf(request, sizeof(request), "%s/elevate_proc", download);
   snprintf(result, sizeof(result), "%s/lapy_owned_result", download);
   CHECK(mkdir(download, 0755) == 0);
   config = (struct ps5_elevation_config){ request, download, result, data, 60, 10000 };
}

static void
cleanup(void)
{
   char command[400];

   snprintf(command, sizeof(command), "rm -rf '%s'", root);
   CHECK(system(command) == 0);
}

static bool
read_file(const char *path, char *out, size_t size)
{
   FILE *file = fopen(path, "r");
   size_t count;

   if (!file)
      return false;
   count = fread(out, 1, size - 1, file);
   out[count] = 0;
   fclose(file);
   return true;
}

/* What the daemon sees: the request, as one atomic file. */
static char seen_request[128];
static bool daemon_saw_leftover;

static void *
daemon_thread(void *argument)
{
   (void)argument;
   for (int i = 0; i < 400; i++) {
      if (read_file(request, seen_request, sizeof(seen_request))) {
         char leftover[220];
         snprintf(leftover, sizeof(leftover), "%s/.elevate_proc.%ld", download, (long)getpid());
         daemon_saw_leftover = access(leftover, F_OK) == 0;
         usleep(80000); /* the kernel work */
         CHECK(mkdir(data, 0777) == 0);
         unlink(request); /* the daemon consumes the request */
         return NULL;
      }
      usleep(5000);
   }
   return NULL;
}

static void
test_grants_data_when_the_daemon_acts(void)
{
   pthread_t thread;
   char expected[64], report[96];

   prepare();
   CHECK(pthread_create(&thread, NULL, daemon_thread, NULL) == 0);
   CHECK(ps5_elevation_request_with(&config, PS5_ELEVATION_FILESYSTEM) == PS5_ELEVATION_OK);
   pthread_join(thread, NULL);
   snprintf(expected, sizeof(expected), "{\"PID\":%ld}\n", (long)getpid());
   CHECK(!strcmp(seen_request, expected));
   CHECK(!daemon_saw_leftover);
   CHECK(read_file(result, report, sizeof(report)) && !strcmp(report, "DATA_OK=1 OPEN_ERRNO=0\n"));
   /* the proof file is not left behind */
   char probe[260];
   snprintf(probe, sizeof(probe), "%s/.lapy_probe_%ld", data, (long)getpid());
   CHECK(access(probe, F_OK) != 0);
   cleanup();
}

static void
test_a_missing_daemon_is_a_timeout_and_authorizes_nothing(void)
{
   char report[96];

   prepare();
   config.polls = 5;
   CHECK(ps5_elevation_request_with(&config, PS5_ELEVATION_FILESYSTEM) == PS5_ELEVATION_TIMEOUT);
   CHECK(read_file(result, report, sizeof(report)) && !strcmp(report, "DATA_OK=0 OPEN_ERRNO=2\n"));
   CHECK(access(request, F_OK) == 0); /* the request stays for a daemon that arrives */
   cleanup();
}

static void
test_other_capabilities_are_refused_before_anything_is_written(void)
{
   prepare();
   CHECK(ps5_elevation_request_with(&config, (enum ps5_elevation_capability)2) == PS5_ELEVATION_UNSUPPORTED_CAPABILITY);
   CHECK(access(result, F_OK) != 0);
   CHECK(access(request, F_OK) != 0);
   cleanup();
}

static void
test_no_result_file_means_unavailable(void)
{
   prepare();
   config.result_path = "/nonexistent-directory/lapy_owned_result";
   CHECK(ps5_elevation_request_with(&config, PS5_ELEVATION_FILESYSTEM) == PS5_ELEVATION_UNAVAILABLE);
   CHECK(access(request, F_OK) != 0);
   cleanup();
}

static void
test_an_unwritable_request_directory_is_a_transport_error(void)
{
   prepare();
   config.request_directory = "/nonexistent-directory";
   CHECK(ps5_elevation_request_with(&config, PS5_ELEVATION_FILESYSTEM) == PS5_ELEVATION_TRANSPORT_ERROR);
   cleanup();
}

static void
test_a_stale_request_is_replaced_whole(void)
{
   char body[128];
   FILE *stale;

   prepare();
   stale = fopen(request, "w");
   CHECK(stale);
   fputs("{\"PID\":1}\nand more bytes from an earlier owner that are not JSON\n", stale);
   fclose(stale);
   config.polls = 2;
   (void)ps5_elevation_request_with(&config, PS5_ELEVATION_FILESYSTEM);
   CHECK(read_file(request, body, sizeof(body)));
   char expected[64];
   snprintf(expected, sizeof(expected), "{\"PID\":%ld}\n", (long)getpid());
   CHECK(!strcmp(body, expected));
   cleanup();
}

static void
test_status_names(void)
{
   CHECK(!strcmp(ps5_elevation_status_name(PS5_ELEVATION_OK), "ok"));
   CHECK(!strcmp(ps5_elevation_status_name(PS5_ELEVATION_TIMEOUT), "timeout"));
   CHECK(!strcmp(ps5_elevation_status_name(PS5_ELEVATION_APPLY_FAILED), "apply_failed"));
   CHECK(!strcmp(ps5_elevation_status_name((enum ps5_elevation_status)99), "unknown"));
}

int
main(void)
{
   test_grants_data_when_the_daemon_acts();
   test_a_missing_daemon_is_a_timeout_and_authorizes_nothing();
   test_other_capabilities_are_refused_before_anything_is_written();
   test_no_result_file_means_unavailable();
   test_an_unwritable_request_directory_is_a_transport_error();
   test_a_stale_request_is_replaced_whole();
   test_status_names();
   puts("PASS: the elevation client: grant on a daemon, timeout without one, refusals, atomic replacement");
   return 0;
}
