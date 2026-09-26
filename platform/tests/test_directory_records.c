/*
 * PS5 Platform - directory streams over crafted getdents records.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * src/directory.c against the console's FreeBSD records as a fixture hands
 * them over: several batches, EOF, a denied open, a failed rewind, and records
 * that are truncated, oversized, zero-length or unterminated, which must fail
 * closed with EIO. The fixture refuses reads smaller than 64 KiB, as the
 * console's mounted filesystems do. Linked with --wrap=open and --wrap=close;
 * fstat, lseek and getdents are this file's. This is the RetroArch title's
 * tests/ps5_directory_test.cpp, moved here with the code it tests.
 */
#define _GNU_SOURCE 1

#include "ps5platform/libc.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_BATCHES 4
#define RECORD_BYTES 256

static char batches[MAX_BATCHES][RECORD_BYTES * 2];
static size_t batch_bytes[MAX_BATCHES];
static size_t batch_count, batch;
static int closed;
static int seek_fails;

/* One record: a 32-bit inode, a 16-bit length, the type, the name length,
 * then the terminated name, 4-byte aligned. Returns its length. */
static size_t
record(char *out, uint32_t inode, unsigned type, const char *name)
{
   const size_t name_length = strlen(name);
   const uint16_t length = (uint16_t)((8 + name_length + 1 + 3) & ~3u);
   memset(out, 0, length);
   memcpy(out, &inode, 4);
   memcpy(out + 4, &length, 2);
   out[6] = (char)type;
   out[7] = (char)name_length;
   memcpy(out + 8, name, name_length + 1);
   return length;
}

int
__wrap_open(const char *path, int flags, ...)
{
   assert(flags == (O_RDONLY | O_DIRECTORY));
   if (strcmp(path, "/denied") == 0) {
      errno = EACCES;
      return -1;
   }
   return 0; /* descriptor zero is valid, and must be closed too */
}

int
__wrap_close(int fd)
{
   assert(fd == 0);
   ++closed;
   return 0;
}

int
fstat(int fd, struct stat *status)
{
   assert(fd == 0);
   memset(status, 0, sizeof(*status));
   status->st_mode = S_IFDIR | 0777;
   status->st_ino = 1;
   return 0;
}

off_t
lseek(int fd, off_t offset, int whence)
{
   assert(fd == 0 && offset == 0 && whence == SEEK_SET);
   if (seek_fails) {
      errno = EINVAL;
      return -1;
   }
   batch = 0;
   return 0;
}

int
getdents(int fd, char *buffer, int size)
{
   assert(fd == 0);
   if (size < 64 * 1024) {
      errno = EINVAL;
      return -1;
   }
   if (batch == batch_count)
      return 0;
   memcpy(buffer, batches[batch], batch_bytes[batch]);
   return (int)batch_bytes[batch++];
}

int
main(void)
{
   assert(ps5_opendir("/denied") == NULL && errno == EACCES);
   assert(ps5_opendir(NULL) == NULL && errno == ENOENT);

   size_t used = record(batches[0], 0, DT_REG, "deleted");
   used += record(batches[0] + used, 2, DT_DIR, "cores");
   batch_bytes[0] = used;
   batch_bytes[1] = record(batches[1], 3, DT_REG, "Example ROM.bin");
   batch_count = 2;
   DIR *dir = ps5_opendir("/app0");
   assert(dir);
   struct dirent *entry = ps5_readdir(dir);
   assert(entry && entry->d_type == DT_DIR && strcmp(entry->d_name, "cores") == 0);
   entry = ps5_readdir(dir);
   assert(entry && entry->d_type == DT_REG && strcmp(entry->d_name, "Example ROM.bin") == 0);
   errno = 0;
   assert(!ps5_readdir(dir) && errno == 0 && !ps5_readdir(dir));
   seek_fails = 1;
   ps5_rewinddir(dir);
   assert(errno == EINVAL && !ps5_readdir(dir));
   seek_fails = 0;
   ps5_rewinddir(dir);
   entry = ps5_readdir(dir);
   assert(entry && strcmp(entry->d_name, "cores") == 0);
   ps5_rewinddir(dir); /* a partly read batch is reset too */
   entry = ps5_readdir(dir);
   assert(entry && strcmp(entry->d_name, "cores") == 0);
   assert(ps5_closedir(dir) == 0 && closed == 1);

   /* Truncated headers, oversized and zero-length records and names without
    * their terminator fail closed. */
   char bad[4][RECORD_BYTES];
   size_t bad_bytes[4];
   memset(bad[0], 0, 7);
   bad_bytes[0] = 7;
   bad_bytes[1] = record(bad[1], 1, DT_DIR, "x");
   bad[1][4] = (char)255;
   bad_bytes[2] = record(bad[2], 1, DT_DIR, "x");
   bad[2][4] = 0;
   bad_bytes[3] = record(bad[3], 1, DT_DIR, "x");
   bad[3][9] = 'x';
   for (unsigned i = 0; i < 4; i++) {
      memcpy(batches[0], bad[i], bad_bytes[i]);
      batch_bytes[0] = bad_bytes[i];
      batch_count = 1;
      batch = 0;
      dir = ps5_opendir("/app0");
      assert(dir);
      errno = 0;
      assert(!ps5_readdir(dir) && errno == EIO);
      assert(!ps5_readdir(dir));
      ps5_closedir(dir);
   }
   assert(closed == 5);
   puts("ps5-platform directory records: parsing, several batches, EOF, denial and corruption PASS");
   return 0;
}
