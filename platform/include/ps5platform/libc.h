/*
 * PS5 Platform - the libc functions the console lacks, refuses or faults in.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each is here because of how the console behaves for a title, not because
 * of the SDK: PS5_RetroArch's docs/PLATFORM_FIRMWARE_ANALYSIS.md records
 * which system module exports what, and every behaviour below was measured
 * by one of our titles.
 *
 *   gmtime_r, localtime_r, utimensat, futimens, dirfd, clock_nanosleep,
 *   arc4random, arc4random_buf, arc4random_uniform, if_nameindex
 *                          no system module exports them
 *   openat, unlinkat, fchmodat, fstatat, mkdirat, renameat
 *                          only libkernel_sys exports them, which titles do
 *                          not import: the imports resolve to nothing
 *   statvfs, fstatvfs      exported by libc but built on statfs (only in
 *                          libkernel_sys): they fault
 *   opendir and its family exported, but refused to a title; enumeration
 *                          goes through getdents
 *   getaddrinfo, freeaddrinfo
 *                          routed by the SDK to a module titles do not load
 *
 * They carry a ps5_ prefix: a title that defined libc's own names would
 * export them, which the title converter refuses. Each consumer binds the
 * standard names to these its own way (the title's link wraps, its core
 * loader's import table). umask is not here: the kernel a title talks to has
 * none, and an emulated mask that nothing applies would only mislead; code
 * that needs a file's mode sets it after creating the file.
 */
#ifndef PS5PLATFORM_LIBC_H
#define PS5PLATFORM_LIBC_H

#include <dirent.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct addrinfo;
struct if_nameindex;

struct tm *ps5_gmtime_r(const time_t *time, struct tm *result);
struct tm *ps5_localtime_r(const time_t *time, struct tm *result);

/* Not cryptographic: a splitmix64 generator seeded from the timestamp counter. */
uint32_t ps5_arc4random(void);
void ps5_arc4random_buf(void *buffer, size_t bytes);
uint32_t ps5_arc4random_uniform(uint32_t bound);

/* A writable filesystem with 16 GiB free: no query a title can make reports
 * the data partition's free space, and the callers (save-size checks) need
 * room. */
int ps5_statvfs(const char *path, struct statvfs *result);
int ps5_fstatvfs(int fd, struct statvfs *result);

/* Through utimes, with microsecond precision. */
int ps5_utimensat(int directory, const char *path, const struct timespec times[2], int flags);
int ps5_futimens(int fd, const struct timespec times[2]);

/* An absolute deadline becomes the interval still to go. */
int ps5_clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
                        struct timespec *remaining);

/* Name lookups and interface enumeration are refused as the callers expect. */
int ps5_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                    struct addrinfo **result);
void ps5_freeaddrinfo(struct addrinfo *info);
struct if_nameindex *ps5_if_nameindex(void);
void ps5_if_freenameindex(struct if_nameindex *list);

/* Directory streams through getdents. */
DIR *ps5_opendir(const char *path);
DIR *ps5_fdopendir(int fd);
struct dirent *ps5_readdir(DIR *directory);
void ps5_rewinddir(DIR *directory);
int ps5_dirfd(DIR *directory);
int ps5_closedir(DIR *directory);

/* The *at family, resolved against the path each directory descriptor was
 * opened with (recorded by ps5_openat and ps5_opendir, and checked against
 * the descriptor's device and inode before use). */
int ps5_openat(int directory, const char *name, int flags, ...);
int ps5_unlinkat(int directory, const char *name, int flags);
int ps5_fchmodat(int directory, const char *name, mode_t mode, int flags);
int ps5_fstatat(int directory, const char *name, struct stat *status, int flags);
int ps5_mkdirat(int directory, const char *name, mode_t mode);
int ps5_renameat(int from_directory, const char *from, int to_directory, const char *to);

/* For the host tests: the path of name relative to a directory's path. */
int ps5_join_path(const char *directory, const char *name, char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* PS5PLATFORM_LIBC_H */
