/*
 * PS5 Platform - directory streams and the *at family (include/ps5platform/libc.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A title's opendir is refused, and the *at functions resolve to nothing
 * (only libkernel_sys exports them), so directories are read with getdents
 * and a path relative to a directory descriptor is joined to the path that
 * descriptor was opened with. The record is checked against the descriptor's
 * device and inode before use, so a descriptor number that was closed and
 * reused is never resolved to a stale path. This is the RetroArch title's
 * src/ps5_directory.cpp, moved here.
 */
#define _GNU_SOURCE 1

#include "ps5platform/libc.h"

#include "at.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int getdents(int fd, char *buffer, int bytes);

/* ---- paths of directory descriptors ---------------------------------------- */

#define TRACKED 64
#define PATH_BYTES 1024

struct tracked {
   int fd;
   dev_t device;
   ino_t inode;
   char path[PATH_BYTES];
};

static pthread_mutex_t tracked_lock = PTHREAD_MUTEX_INITIALIZER;
static struct tracked tracked[TRACKED] = {[0 ... TRACKED - 1] = {.fd = -1}};

static void
remember(int fd, const char *path)
{
   struct stat status;
   if (fstat(fd, &status) != 0 || !S_ISDIR(status.st_mode) || strlen(path) >= PATH_BYTES)
      return;
   pthread_mutex_lock(&tracked_lock);
   struct tracked *slot = NULL;
   for (int i = 0; i < TRACKED; i++)
      if (tracked[i].fd == fd || (!slot && tracked[i].fd < 0))
         slot = &tracked[i];
   if (!slot)
      slot = &tracked[fd % TRACKED];
   slot->fd = fd;
   slot->device = status.st_dev;
   slot->inode = status.st_ino;
   strcpy(slot->path, path);
   pthread_mutex_unlock(&tracked_lock);
}

static void
forget(int fd)
{
   pthread_mutex_lock(&tracked_lock);
   for (int i = 0; i < TRACKED; i++)
      if (tracked[i].fd == fd)
         tracked[i].fd = -1;
   pthread_mutex_unlock(&tracked_lock);
}

int
ps5_join_path(const char *directory, const char *name, char *out, size_t size)
{
   const size_t base = strlen(directory);
   const bool slash = base > 0 && directory[base - 1] == '/';
   if (base + (slash ? 0 : 1) + strlen(name) >= size) {
      errno = ENAMETOOLONG;
      return -1;
   }
   strcpy(out, directory);
   if (!slash)
      strcat(out, "/");
   strcat(out, name);
   return 0;
}

int
ps5p_resolve_at(int directory, const char *name, char *out, size_t size)
{
   if (!name) {
      errno = EFAULT;
      return -1;
   }
   if (directory == AT_FDCWD || name[0] == '/') {
      if (strlen(name) >= size) {
         errno = ENAMETOOLONG;
         return -1;
      }
      strcpy(out, name);
      return 0;
   }
   struct stat status;
   if (fstat(directory, &status) != 0)
      return -1;
   int result = -1;
   errno = ENOSYS;
   pthread_mutex_lock(&tracked_lock);
   for (int i = 0; i < TRACKED; i++) {
      if (tracked[i].fd != directory || tracked[i].device != status.st_dev ||
          tracked[i].inode != status.st_ino)
         continue;
      result = ps5_join_path(tracked[i].path, name, out, size);
      break;
   }
   pthread_mutex_unlock(&tracked_lock);
   return result;
}

/* ---- directory streams ------------------------------------------------------ */

struct stream {
   int fd;
   size_t offset, bytes;
   bool finished;
   struct dirent entry;
   /* Mounted title directories need a larger read than the synthetic root. */
   char buffer[64 * 1024];
};

DIR *
ps5_fdopendir(int fd)
{
   struct stat status;
   if (fstat(fd, &status) != 0)
      return NULL;
   if (!S_ISDIR(status.st_mode)) {
      errno = ENOTDIR;
      return NULL;
   }
   struct stream *const stream = calloc(1, sizeof(*stream));
   if (!stream) {
      errno = ENOMEM;
      return NULL;
   }
   stream->fd = fd;
   return (DIR *)stream;
}

DIR *
ps5_opendir(const char *path)
{
   if (!path || !*path) {
      errno = ENOENT;
      return NULL;
   }
   const int fd = open(path, O_RDONLY | O_DIRECTORY);
   if (fd < 0)
      return NULL;
   DIR *const stream = ps5_fdopendir(fd);
   if (!stream) {
      const int saved = errno;
      close(fd);
      errno = saved;
      return NULL;
   }
   remember(fd, path);
   return stream;
}

/* getdents' records are the console's FreeBSD layout: a 32-bit inode, a 16-bit
 * record length, an 8-bit type, an 8-bit name length, then the name and its
 * terminator (the public FreeBSD dirent of this kernel's generation). */
struct dirent *
ps5_readdir(DIR *opaque)
{
   struct stream *const stream = (struct stream *)opaque;
   if (!stream) {
      errno = EBADF;
      return NULL;
   }
   while (!stream->finished) {
      if (stream->offset == stream->bytes) {
         const int count = getdents(stream->fd, stream->buffer, (int)sizeof(stream->buffer));
         if (count <= 0 || (size_t)count > sizeof(stream->buffer)) {
            stream->finished = true;
            if (count > 0)
               errno = EIO;
            return NULL;
         }
         stream->bytes = (size_t)count;
         stream->offset = 0;
      }
      const size_t remaining = stream->bytes - stream->offset;
      const char *const record = stream->buffer + stream->offset;
      uint32_t inode = 0;
      uint16_t length = 0;
      if (remaining >= 8) {
         memcpy(&inode, record, 4);
         memcpy(&length, record + 4, 2);
      }
      const size_t name_length = remaining >= 8 ? (uint8_t)record[7] : 0;
      if (remaining < 8 || length < 8 + name_length + 1 || length > remaining ||
          name_length >= sizeof(stream->entry.d_name) || record[8 + name_length] != 0) {
         stream->finished = true;
         errno = EIO;
         return NULL;
      }
      stream->offset += length;
      if (!inode)
         continue;
      memset(&stream->entry, 0, sizeof(stream->entry));
      stream->entry.d_type = (uint8_t)record[6];
      memcpy(stream->entry.d_name, record + 8, name_length + 1);
      return &stream->entry;
   }
   return NULL;
}

void
ps5_rewinddir(DIR *opaque)
{
   struct stream *const stream = (struct stream *)opaque;
   if (!stream) {
      errno = EBADF;
      return;
   }
   if (lseek(stream->fd, 0, SEEK_SET) < 0)
      return;
   stream->offset = stream->bytes = 0;
   stream->finished = false;
}

int
ps5_dirfd(DIR *opaque)
{
   struct stream *const stream = (struct stream *)opaque;
   if (!stream) {
      errno = EINVAL;
      return -1;
   }
   return stream->fd;
}

int
ps5_closedir(DIR *opaque)
{
   struct stream *const stream = (struct stream *)opaque;
   if (!stream) {
      errno = EBADF;
      return -1;
   }
   forget(stream->fd);
   const int result = close(stream->fd);
   free(stream);
   return result;
}

/* ---- the *at family --------------------------------------------------------- */

int
ps5_openat(int directory, const char *name, int flags, ...)
{
   int mode = 0;
   if (flags & O_CREAT) {
      va_list arguments;
      va_start(arguments, flags);
      mode = va_arg(arguments, int);
      va_end(arguments);
   }
   char path[PATH_BYTES];
   if (ps5p_resolve_at(directory, name, path, sizeof(path)) != 0)
      return -1;
   const int fd = open(path, flags, mode);
   if (fd >= 0)
      remember(fd, path);
   return fd;
}

int
ps5_unlinkat(int directory, const char *name, int flags)
{
   char path[PATH_BYTES];
   if (ps5p_resolve_at(directory, name, path, sizeof(path)) != 0)
      return -1;
   return (flags & AT_REMOVEDIR) ? rmdir(path) : unlink(path);
}

int
ps5_fchmodat(int directory, const char *name, mode_t mode, int flags)
{
   (void)flags;
   char path[PATH_BYTES];
   if (ps5p_resolve_at(directory, name, path, sizeof(path)) != 0)
      return -1;
   return chmod(path, mode);
}

int
ps5_fstatat(int directory, const char *name, struct stat *status, int flags)
{
   char path[PATH_BYTES];
   if (ps5p_resolve_at(directory, name, path, sizeof(path)) != 0)
      return -1;
   return (flags & AT_SYMLINK_NOFOLLOW) ? lstat(path, status) : stat(path, status);
}

int
ps5_mkdirat(int directory, const char *name, mode_t mode)
{
   char path[PATH_BYTES];
   if (ps5p_resolve_at(directory, name, path, sizeof(path)) != 0)
      return -1;
   return mkdir(path, mode);
}

int
ps5_renameat(int from_directory, const char *from, int to_directory, const char *to)
{
   char from_path[PATH_BYTES], to_path[PATH_BYTES];
   if (ps5p_resolve_at(from_directory, from, from_path, sizeof(from_path)) != 0 ||
       ps5p_resolve_at(to_directory, to, to_path, sizeof(to_path)) != 0)
      return -1;
   return rename(from_path, to_path);
}
