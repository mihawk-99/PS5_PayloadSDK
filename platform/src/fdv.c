/*
 * PS5 Platform - open files past the console's per-process limit (fdv.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A tracked file is a group: the path it was opened by, its flags, the
 * device and inode it had, and the descriptor numbers that refer to it (a
 * dup or a descriptor passed over a socket joins the group of the file it
 * is a copy of, because it shares the kernel's file description, offset
 * included). Parking replaces every number of a group with the placeholder
 * socket and remembers the offset; reopening does the reverse.
 *
 * The fast path (a call on a descriptor) is lock free: it pins the group
 * with an atomic increment and checks that the group is live. A parker
 * marks the group PARKING first and parks it only if no pin is held, with
 * sequentially consistent atomics on both sides, so a pin and a park cannot
 * both succeed. Everything else (opening, closing, parking, reopening) is
 * under one mutex.
 */
#define _GNU_SOURCE 1

#include "ps5platform/fdv.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_FDS 16384
#define MAX_GROUPS 8192
#define MAX_MEMBERS 6
#define PATH_LIMIT 1024
#define BATCH_LIMIT 64
#define DEFAULT_HIGH_WATER 192u
#define DEFAULT_BATCH 32u
#define LOWEST_HIGH_WATER 32u
#define LIMIT_RESERVE 8u

enum { LIVE, PARKING, PARKED, BROKEN };

struct group {
   atomic_int pins;
   atomic_int inflight; /* copies sent over a socket and not yet received */
   atomic_int state;
   atomic_ullong used;
   atomic_bool dying; /* released as soon as the last pin goes */
   bool in_use;
   bool keep;
   int next_free;
   int member_count;
   int members[MAX_MEMBERS];
   int flags;
   dev_t dev;
   ino_t ino;
   off_t position;
   char *path;
};

static struct {
   pthread_mutex_t lock;
   struct ps5_fdv_ops ops;
   atomic_bool ready;
   atomic_bool disabled;
   int placeholder;
   atomic_uint high_water;
   unsigned batch;
   atomic_uint live;
   atomic_int inflight_total;
   int groups_high; /* one past the highest group ever allocated */
   unsigned parked;
   int free_head;
   atomic_ullong clock;
   unsigned long parks, unparks, limit_hits, failures;
} fdv = { .lock = PTHREAD_MUTEX_INITIALIZER };

static atomic_uint slots[MAX_FDS]; /* descriptor -> group index + 1 */
static struct group groups[MAX_GROUPS];

static void
say(const char *format, ...)
{
   if (!fdv.ops.log)
      return;
   char line[256];
   va_list args;
   va_start(args, format);
   vsnprintf(line, sizeof(line), format, args);
   va_end(args);
   fdv.ops.log(line);
}

static unsigned
slot_get(int fd)
{
   return (unsigned)fd < MAX_FDS ? atomic_load(&slots[fd]) : 0;
}

static void
lock(void)
{
   pthread_mutex_lock(&fdv.lock);
}

static void
unlock(void)
{
   pthread_mutex_unlock(&fdv.lock);
}

static struct group *
group_alloc(void)
{
   if (fdv.free_head < 0)
      return NULL;
   struct group *g = &groups[fdv.free_head];
   fdv.free_head = g->next_free;
   if ((int)(g - groups) + 1 > fdv.groups_high)
      fdv.groups_high = (int)(g - groups) + 1;
   return g;
}

/* Called with the lock held: a group with no descriptors and no pins is
 * returned to the pool. */
static void
group_release_if_unused(struct group *g)
{
   if (g->member_count || atomic_load(&g->pins) || !g->in_use)
      return;
   if (atomic_load(&g->inflight))
      atomic_fetch_sub(&fdv.inflight_total, atomic_load(&g->inflight));
   atomic_store(&g->inflight, 0);
   free(g->path);
   g->path = NULL;
   g->in_use = false;
   atomic_store(&g->dying, false);
   atomic_store(&g->state, LIVE);
   g->next_free = fdv.free_head;
   fdv.free_head = (int)(g - groups);
}

/* Remove one descriptor from its group (the lock is held). */
static void
untrack_locked(int fd)
{
   unsigned index = slot_get(fd);
   if (!index)
      return;
   struct group *g = &groups[index - 1];
   atomic_store(&slots[fd], 0);
   for (int i = 0; i < g->member_count; i++) {
      if (g->members[i] != fd)
         continue;
      g->members[i] = g->members[--g->member_count];
      break;
   }
   int state = atomic_load(&g->state);
   if (state == LIVE || state == PARKING)
      atomic_fetch_sub(&fdv.live, 1);
   if (!g->member_count) {
      if (state == PARKED)
         fdv.parked--;
      if (atomic_load(&g->pins)) {
         atomic_store(&g->dying, true);
      } else {
         group_release_if_unused(g);
      }
   }
}

static void
track_locked(int fd, const char *path, int flags)
{
   struct stat status;
   if ((unsigned)fd >= MAX_FDS || path[0] != '/' || strlen(path) >= PATH_LIMIT)
      return;
   if (fdv.ops.fstat(fd, &status) != 0 || !S_ISREG(status.st_mode))
      return;
   if (slot_get(fd))
      untrack_locked(fd);
   struct group *g = group_alloc();
   if (!g)
      return;
   char *copy = strdup(path);
   if (!copy) {
      g->next_free = fdv.free_head;
      fdv.free_head = (int)(g - groups);
      return;
   }
   g->in_use = true;
   g->keep = false;
#ifdef O_EXLOCK
   if (flags & O_EXLOCK)
      g->keep = true;
#endif
#ifdef O_SHLOCK
   if (flags & O_SHLOCK)
      g->keep = true;
#endif
   g->member_count = 1;
   g->members[0] = fd;
   g->flags = flags & ~(O_CREAT | O_EXCL | O_TRUNC);
   g->dev = status.st_dev;
   g->ino = status.st_ino;
   g->position = 0;
   g->path = copy;
   atomic_store(&g->pins, 0);
   atomic_store(&g->inflight, 0);
   atomic_store(&g->dying, false);
   atomic_store(&g->used, atomic_fetch_add(&fdv.clock, 1));
   atomic_store(&g->state, LIVE);
   atomic_store(&slots[fd], (unsigned)(g - groups) + 1);
   atomic_fetch_add(&fdv.live, 1);
}

/* Park one group (the lock is held); true when it was parked. */
static bool
park_one_locked(struct group *g)
{
   struct stat status;

   atomic_store(&g->state, PARKING);
   if (atomic_load(&g->pins) != 0 || atomic_load(&g->inflight) != 0) {
      atomic_store(&g->state, LIVE);
      return false;
   }
   /* A descriptor closed behind our back may be some other file's number now:
    * every member must still be this file, or it is dropped. */
   for (int i = 0; i < g->member_count;) {
      if (fdv.ops.fstat(g->members[i], &status) == 0 && S_ISREG(status.st_mode) &&
          status.st_dev == g->dev && status.st_ino == g->ino) {
         i++;
         continue;
      }
      say("fdv: descriptor %d is no longer %s", g->members[i], g->path);
      int stale = g->members[i];
      atomic_store(&slots[stale], 0);
      g->members[i] = g->members[--g->member_count];
      atomic_fetch_sub(&fdv.live, 1);
   }
   if (!g->member_count) {
      atomic_store(&g->state, LIVE);
      group_release_if_unused(g);
      return false;
   }
   off_t position = fdv.ops.lseek(g->members[0], 0, SEEK_CUR);
   if (position < 0 || fdv.ops.fstat(g->members[0], &status) != 0 || status.st_nlink == 0) {
      g->keep = true; /* unlinked, or not seekable: it must stay open */
      atomic_store(&g->state, LIVE);
      return false;
   }
   for (int i = 0; i < g->member_count; i++) {
      int result;
      while ((result = fdv.ops.dup2(fdv.placeholder, g->members[i])) < 0 && errno == EINTR) {
      }
      if (result < 0) {
         /* dup2 onto a descriptor that is open cannot fail; if it does, the
          * numbers replaced so far are placeholders and the rest are not, and
          * the file fails loudly rather than reading from the wrong one */
         say("fdv: cannot park descriptor %d of %s: %s", g->members[i], g->path, strerror(errno));
         g->keep = true;
         g->position = position;
         atomic_fetch_sub(&fdv.live, (unsigned)g->member_count);
         fdv.parked++;
         fdv.failures++;
         atomic_store(&g->state, BROKEN);
         return false;
      }
   }
   g->position = position;
   atomic_fetch_sub(&fdv.live, (unsigned)g->member_count);
   fdv.parked++;
   fdv.parks++;
   atomic_store(&g->state, PARKED);
   return true;
}

/* Park the `want` least recently used idle groups (the lock is held). */
static unsigned
park_batch_locked(const struct group *except, unsigned want)
{
   struct {
      unsigned long long used;
      int index;
   } best[BATCH_LIMIT];
   unsigned count = 0, parked = 0;

   if (atomic_load(&fdv.disabled) || !want)
      return 0;
   if (want > BATCH_LIMIT)
      want = BATCH_LIMIT;
   for (int i = 0; i < fdv.groups_high; i++) {
      struct group *g = &groups[i];
      if (!g->in_use || g->keep || g == except || atomic_load(&g->state) != LIVE ||
          atomic_load(&g->pins) != 0 || atomic_load(&g->inflight) != 0)
         continue;
      unsigned long long used = atomic_load_explicit(&g->used, memory_order_relaxed);
      unsigned at = count;
      while (at > 0 && best[at - 1].used > used)
         at--;
      if (at >= want)
         continue;
      for (unsigned j = count < want ? count : want - 1; j > at; j--)
         best[j] = best[j - 1];
      best[at].used = used;
      best[at].index = i;
      if (count < want)
         count++;
   }
   for (unsigned i = 0; i < count; i++)
      if (park_one_locked(&groups[best[i].index]))
         parked++;
   return parked;
}

/* An open or a dup answered EMFILE: the limit is lower than the water mark
 * (the file count is not all that is open): learn it. */
static void
limit_hit_locked(void)
{
   unsigned live = atomic_load(&fdv.live);
   fdv.limit_hits++;
   if (live > LOWEST_HIGH_WATER + LIMIT_RESERVE && live - LIMIT_RESERVE < atomic_load(&fdv.high_water))
      atomic_store(&fdv.high_water, live - LIMIT_RESERVE);
}

static void
make_room_locked(const struct group *except)
{
   unsigned high = atomic_load(&fdv.high_water), live = atomic_load(&fdv.live);
   if (live >= high)
      park_batch_locked(except, live - high + fdv.batch);
}

/* Reopen a parked group under all of its numbers (the lock is held). */
static int
unpark_locked(struct group *g)
{
   struct stat status;
   int fd = -1, error = 0;

   make_room_locked(g);
   for (int tries = 0; tries < 4; tries++) {
      fd = fdv.ops.open(g->path, g->flags & ~(O_CREAT | O_EXCL | O_TRUNC), 0);
      if (fd >= 0 || errno != EMFILE)
         break;
      limit_hit_locked();
      if (!park_batch_locked(g, fdv.batch))
         break;
   }
   if (fd < 0) {
      error = errno;
      goto broken;
   }
   if (fdv.ops.fstat(fd, &status) != 0 || status.st_dev != g->dev || status.st_ino != g->ino) {
      say("fdv: %s is not the file that was parked", g->path);
      fdv.ops.close(fd);
      error = EIO;
      goto broken;
   }
   if (fdv.ops.lseek(fd, g->position, SEEK_SET) < 0) {
      error = errno;
      fdv.ops.close(fd);
      goto broken;
   }
   for (int i = 0; i < g->member_count; i++) {
      /* the number must still be our placeholder, not something opened since */
      if (fdv.ops.fstat(g->members[i], &status) != 0 || !S_ISSOCK(status.st_mode)) {
         say("fdv: descriptor %d of %s was closed while parked", g->members[i], g->path);
         int stale = g->members[i];
         atomic_store(&slots[stale], 0);
         g->members[i] = g->members[--g->member_count];
         i--;
         continue;
      }
      if (fdv.ops.dup2(fd, g->members[i]) < 0) {
         error = errno;
         fdv.ops.close(fd);
         goto broken;
      }
   }
   fdv.ops.close(fd);
   atomic_fetch_add(&fdv.live, (unsigned)g->member_count);
   fdv.parked--;
   fdv.unparks++;
   atomic_store(&g->used, atomic_fetch_add(&fdv.clock, 1));
   atomic_store(&g->state, LIVE);
   if (!g->member_count) {
      /* every number was closed while it was parked: nothing was reopened */
      group_release_if_unused(g);
      errno = EBADF;
      return -1;
   }
   return 0;

broken:
   fdv.failures++;
   say("fdv: cannot reopen %s: %s", g->path, strerror(error));
   atomic_store(&g->state, BROKEN);
   errno = error;
   return -1;
}

static int
placeholder_works(void)
{
   struct stat status;
   int probe = fdv.ops.open("/dev/null", O_RDONLY, 0);
   if (probe < 0)
      return 0;
   int ok = fdv.ops.dup2(fdv.placeholder, probe) >= 0 && fdv.ops.fstat(probe, &status) == 0 &&
            S_ISSOCK(status.st_mode);
   fdv.ops.close(probe);
   return ok;
}

int
ps5_fdv_init(const struct ps5_fdv_ops *ops, unsigned high_water, unsigned batch)
{
   if (atomic_load(&fdv.ready))
      return atomic_load(&fdv.disabled) ? -1 : 0;
   lock();
   if (atomic_load(&fdv.ready)) {
      unlock();
      return atomic_load(&fdv.disabled) ? -1 : 0;
   }
   fdv.ops = *ops;
   atomic_store(&fdv.high_water, high_water ? high_water : DEFAULT_HIGH_WATER);
   fdv.batch = batch ? batch : DEFAULT_BATCH;
   if (fdv.batch > BATCH_LIMIT)
      fdv.batch = BATCH_LIMIT;
   fdv.free_head = -1;
   for (int i = MAX_GROUPS - 1; i >= 0; i--) {
      groups[i].next_free = fdv.free_head;
      fdv.free_head = i;
   }
   fdv.placeholder = fdv.ops.placeholder ? fdv.ops.placeholder() : -1;
   atomic_store(&fdv.disabled, fdv.placeholder < 0 || !placeholder_works());
   if (atomic_load(&fdv.disabled))
      say("fdv: replacing a descriptor with a socket does not work here; nothing will be parked");
   atomic_store(&fdv.ready, true);
   unlock();
   return atomic_load(&fdv.disabled) ? -1 : 0;
}

int
ps5_fdv_open(const char *path, int flags, mode_t mode)
{
   int fd;

   if (!atomic_load(&fdv.ready))
      return -1;
   if (!atomic_load(&fdv.disabled) && atomic_load(&fdv.live) >= atomic_load(&fdv.high_water)) {
      lock();
      make_room_locked(NULL);
      unlock();
   }
   fd = fdv.ops.open(path, flags, mode);
   for (int tries = 0; fd < 0 && errno == EMFILE && tries < 4 && !atomic_load(&fdv.disabled); tries++) {
      lock();
      limit_hit_locked();
      unsigned parked = park_batch_locked(NULL, fdv.batch);
      unlock();
      if (!parked)
         break;
      fd = fdv.ops.open(path, flags, mode);
   }
   if (fd >= 0 && !atomic_load(&fdv.disabled) && !(flags & O_DIRECTORY)) {
      int saved = errno;
      lock();
      track_locked(fd, path, flags);
      unlock();
      errno = saved;
   }
   return fd;
}

int
ps5_fdv_close(int fd)
{
   if (atomic_load(&fdv.ready) && slot_get(fd)) {
      lock();
      untrack_locked(fd);
      unlock();
   }
   return fdv.ops.close(fd);
}

int
ps5_fdv_enter(int fd, ps5_fdv_pin *pin)
{
   *pin = 0;
   if (!atomic_load(&fdv.ready))
      return 0;
   unsigned index = slot_get(fd);
   if (!index)
      return 0;
   struct group *g = &groups[index - 1];

   atomic_fetch_add(&g->pins, 1);
   if (atomic_load(&g->state) == LIVE) {
      atomic_store_explicit(&g->used, atomic_fetch_add_explicit(&fdv.clock, 1, memory_order_relaxed),
                            memory_order_relaxed);
      *pin = index;
      return 0;
   }
   atomic_fetch_sub(&g->pins, 1);

   /* parked (or being parked): under the lock, bring it back and pin it before
    * anyone can park it again */
   lock();
   if (slot_get(fd) != index) {
      unlock();
      return 0; /* closed meanwhile: the call fails on its own */
   }
   int state = atomic_load(&g->state);
   if (state == PARKED && unpark_locked(g) != 0) {
      int error = errno;
      unlock();
      errno = error;
      return -1;
   }
   if (state == BROKEN) {
      unlock();
      errno = EIO;
      return -1;
   }
   if (slot_get(fd) != index) {
      unlock();
      errno = EBADF;
      return -1;
   }
   atomic_fetch_add(&g->pins, 1);
   atomic_store_explicit(&g->used, atomic_fetch_add_explicit(&fdv.clock, 1, memory_order_relaxed),
                         memory_order_relaxed);
   unlock();
   *pin = index;
   return 0;
}

void
ps5_fdv_leave(ps5_fdv_pin pin)
{
   if (!pin)
      return;
   struct group *g = &groups[pin - 1];
   if (atomic_fetch_sub(&g->pins, 1) == 1 && atomic_load(&g->dying)) {
      lock();
      group_release_if_unused(g);
      unlock();
   }
}

void
ps5_fdv_dup_note(int from, int to)
{
   if (!atomic_load(&fdv.ready))
      return;
   lock();
   if (slot_get(to))
      untrack_locked(to);
   unsigned index = slot_get(from);
   if (index && (unsigned)to < MAX_FDS) {
      struct group *g = &groups[index - 1];
      if (g->member_count < MAX_MEMBERS) {
         g->members[g->member_count++] = to;
         atomic_store(&slots[to], index);
         atomic_fetch_add(&fdv.live, 1);
      } else {
         g->keep = true; /* a copy we cannot follow: the file must stay open */
      }
   }
   unlock();
}

void
ps5_fdv_forget(int fd)
{
   if (!atomic_load(&fdv.ready) || !slot_get(fd))
      return;
   lock();
   untrack_locked(fd);
   unlock();
}

void
ps5_fdv_keep(ps5_fdv_pin pin)
{
   if (!pin)
      return;
   lock();
   groups[pin - 1].keep = true;
   unlock();
}

int
ps5_fdv_sending(int fd, ps5_fdv_pin *pin)
{
   if (ps5_fdv_enter(fd, pin) != 0)
      return -1;
   if (*pin) {
      atomic_fetch_add(&groups[*pin - 1].inflight, 1);
      atomic_fetch_add(&fdv.inflight_total, 1);
   }
   return 0;
}

void
ps5_fdv_sent(ps5_fdv_pin pin, int delivered)
{
   if (!pin)
      return;
   if (!delivered) {
      atomic_fetch_sub(&groups[pin - 1].inflight, 1);
      atomic_fetch_sub(&fdv.inflight_total, 1);
   }
   ps5_fdv_leave(pin);
}

int
ps5_fdv_received(int fd)
{
   struct stat status;
   struct group *match = NULL;
   int matches = 0, result = 0;

   if (!atomic_load(&fdv.ready) || atomic_load(&fdv.inflight_total) <= 0)
      return 0;
   lock();
   if ((unsigned)fd < MAX_FDS && fdv.ops.fstat(fd, &status) == 0 && S_ISREG(status.st_mode)) {
      for (int i = 0; i < fdv.groups_high; i++) {
         struct group *g = &groups[i];
         if (!g->in_use || atomic_load(&g->inflight) <= 0 || g->dev != status.st_dev || g->ino != status.st_ino)
            continue;
         match = g;
         matches++;
      }
      if (matches == 1 && match->member_count < MAX_MEMBERS) {
         if (slot_get(fd))
            untrack_locked(fd);
         match->members[match->member_count++] = fd;
         atomic_store(&slots[fd], (unsigned)(match - groups) + 1);
         atomic_fetch_add(&fdv.live, 1);
         atomic_fetch_sub(&match->inflight, 1);
         atomic_fetch_sub(&fdv.inflight_total, 1);
         result = 1;
      } else if (matches > 0) {
         /* two files of the same device and inode were in flight, or the group
          * is full: this copy cannot be told from the other's, so none of them
          * may be parked */
         for (int i = 0; i < fdv.groups_high; i++) {
            struct group *g = &groups[i];
            if (!g->in_use || atomic_load(&g->inflight) <= 0 || g->dev != status.st_dev || g->ino != status.st_ino)
               continue;
            g->keep = true;
            atomic_fetch_sub(&fdv.inflight_total, atomic_load(&g->inflight));
            atomic_store(&g->inflight, 0);
         }
      }
   }
   unlock();
   return result;
}

void
ps5_fdv_renamed(const char *from, const char *to)
{
   if (!atomic_load(&fdv.ready) || from[0] != '/' || to[0] != '/')
      return;
   size_t length = strlen(from);
   lock();
   for (int i = 0; i < fdv.groups_high; i++) {
      struct group *g = &groups[i];
      if (!g->in_use || strncmp(g->path, from, length) != 0 || (g->path[length] != 0 && g->path[length] != '/'))
         continue;
      size_t head = strlen(to), tail = strlen(g->path + length);
      char *name = head + tail < PATH_LIMIT ? malloc(head + tail + 1) : NULL;
      if (!name)
         continue;
      memcpy(name, to, head);
      memcpy(name + head, g->path + length, tail + 1);
      free(g->path);
      g->path = name;
   }
   unlock();
}

unsigned
ps5_fdv_make_room(void)
{
   if (!atomic_load(&fdv.ready))
      return 0;
   lock();
   limit_hit_locked();
   unsigned parked = park_batch_locked(NULL, fdv.batch);
   unlock();
   return parked;
}

unsigned
ps5_fdv_park_idle(unsigned count)
{
   if (!atomic_load(&fdv.ready))
      return 0;
   lock();
   unsigned parked = park_batch_locked(NULL, count);
   unlock();
   return parked;
}

void
ps5_fdv_get_stats(struct ps5_fdv_stats *stats)
{
   memset(stats, 0, sizeof(*stats));
   if (!atomic_load(&fdv.ready))
      return;
   lock();
   stats->live = atomic_load(&fdv.live);
   stats->parked = fdv.parked;
   stats->high_water = atomic_load(&fdv.high_water);
   stats->parks = fdv.parks;
   stats->unparks = fdv.unparks;
   stats->limit_hits = fdv.limit_hits;
   stats->failures = fdv.failures;
   stats->disabled = atomic_load(&fdv.disabled);
   unlock();
}
