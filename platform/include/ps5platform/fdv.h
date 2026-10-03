/*
 * PS5 Platform - open files past the console's per-process limit.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A title can hold about 256 files open at once, whatever getrlimit says.
 * Measured on the console (2026-10-03, PS5_Proton's Wine process): open()
 * fails with EMFILE after 249 successes for any path (a /data file,
 * /dev/null, a directory, a sandbox mount), the same through the kernel's own
 * sceKernelOpen, while getrlimit(RLIMIT_NOFILE) reports 13952, setrlimit
 * changes nothing, and dup() and socket() go past 12,000 descriptors. Moving
 * each descriptor to a high number does not help: it is the count of files
 * opened by path. A program that opens more at once (the Java Minecraft
 * launches with a few hundred jars and region files; Wine's server holds
 * one descriptor per Windows handle) fails with "too many open files".
 *
 * This module lifts the limit for the files it is told about. When a title is
 * near the limit it parks the files that have been idle longest: it remembers
 * the file's path, flags and position, and replaces every descriptor number
 * that refers to it with a socket, which holds the number and costs nothing
 * against the limit. The next call that uses one of the numbers reopens the
 * file, seeks back, and puts it under every one of its numbers again. The
 * numbers a program holds never change, so nothing it keeps (a table of
 * descriptors, a cache of handles) goes stale.
 *
 * Everything that takes a descriptor must go through ps5_fdv_enter and
 * ps5_fdv_leave, or the call finds a socket where the file was and fails; a
 * consumer wraps the libc functions it uses (src/fdv.c's host test shows the
 * pattern, PS5_Proton's wine/ps5/pw_wine_fdv_libc.c is the first consumer).
 * Only regular files that were opened by absolute path through
 * ps5_fdv_open are tracked; every other descriptor passes through untouched
 * and costs one array lookup per call. Until the limit is near, nothing is
 * parked and a call costs the lookup, an atomic increment and a decrement.
 *
 * A file is never parked while a call is in flight on it, while it carries a
 * lock (ps5_fdv_keep), while it is unlinked, or when any descriptor of it is
 * not known to the module; reopening checks that the path still names the
 * same device and inode, and fails with EIO when it does not, never reading
 * another file.
 */
#ifndef PS5PLATFORM_FDV_H
#define PS5PLATFORM_FDV_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the module needs from the kernel; the console's are libc's own calls
 * (a consumer that wraps libc passes the __real_ ones), the host tests' are a
 * model with a limit. placeholder returns a descriptor that is not a file
 * (a socket) and stays open for the life of the process. */
struct ps5_fdv_ops {
   int (*open)(const char *path, int flags, mode_t mode);
   int (*close)(int fd);
   int (*dup2)(int from, int to);
   off_t (*lseek)(int fd, off_t offset, int whence);
   int (*fstat)(int fd, struct stat *status);
   int (*placeholder)(void);
   void (*log)(const char *line); /* optional */
};

struct ps5_fdv_stats {
   unsigned live;        /* descriptors of tracked files that are open */
   unsigned parked;      /* tracked files that are parked */
   unsigned high_water;  /* live descriptors above which files are parked */
   unsigned long parks;
   unsigned long unparks;
   unsigned long limit_hits; /* EMFILE answers that made it park files */
   unsigned long failures;   /* files that could not be reopened */
   int disabled;             /* nonzero: nothing is parked */
};

/* One pin: a call in flight on a tracked file; 0 for a descriptor that is
 * not tracked. */
typedef uint32_t ps5_fdv_pin;

/* high_water: live descriptors above which idle files are parked, 0 for the
 * default (192); batch: how many files one round parks, 0 for 32. Checks
 * that replacing a descriptor with a socket works; -1 (and nothing is ever
 * parked) when it does not. */
int ps5_fdv_init(const struct ps5_fdv_ops *ops, unsigned high_water, unsigned batch);

/* open() for an absolute path, with EMFILE answered by parking idle files and
 * trying again. The file is tracked when it is a regular file. */
int ps5_fdv_open(const char *path, int flags, mode_t mode);

/* close(), forgetting the descriptor first. */
int ps5_fdv_close(int fd);

/* Before a call that uses fd: reopens its file if it is parked and pins it
 * until ps5_fdv_leave. 0 with *pin set, or -1 with errno (the file cannot be
 * reopened: EIO when it has changed, else the open's error). */
int ps5_fdv_enter(int fd, ps5_fdv_pin *pin);
void ps5_fdv_leave(ps5_fdv_pin pin);

/* A new descriptor of the file `from` refers to (after dup, F_DUPFD, dup2):
 * `to` is tracked with it. The caller still holds the pin on `from`. */
void ps5_fdv_dup_note(int from, int to);
/* `fd` is about to be replaced by dup2: forget it. */
void ps5_fdv_forget(int fd);

/* The pinned file must never be parked (it holds a lock, is registered with a
 * kqueue, is wrapped in a FILE). */
void ps5_fdv_keep(ps5_fdv_pin pin);

/* Passing a descriptor over a socket (SCM_RIGHTS), which gives the receiver
 * the kernel's copy of the file, sharing its offset. Before the send, like
 * ps5_fdv_enter but the file also counts as in flight, so it is not parked
 * until the copy has arrived; afterwards, ps5_fdv_sent with whether the send
 * succeeded (this leaves the pin). The receiver calls ps5_fdv_received with
 * each descriptor it gets: when it is a copy of a tracked file that is in
 * flight it joins that file (1); a file in flight under two names of the same
 * device and inode cannot be told apart, and neither is parked afterwards
 * (0 for those, and for descriptors that are not tracked files). */
int ps5_fdv_sending(int fd, ps5_fdv_pin *pin);
void ps5_fdv_sent(ps5_fdv_pin pin, int delivered);
int ps5_fdv_received(int fd);

/* A path was renamed (after the call succeeded): tracked files under it are
 * reopened by the new name. */
void ps5_fdv_renamed(const char *from, const char *to);

/* An operation failed with EMFILE: park idle files. How many were parked. */
unsigned ps5_fdv_make_room(void);

/* Park up to count idle files now (tests, and a consumer that knows better). */
unsigned ps5_fdv_park_idle(unsigned count);

void ps5_fdv_get_stats(struct ps5_fdv_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* PS5PLATFORM_FDV_H */
