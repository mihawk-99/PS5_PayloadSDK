/*
 * PS5 Platform - open files past the console's per-process limit (src/fdv.c).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The module runs against a model of the console's kernel: a descriptor
 * table over open file descriptions (shared by dup, with one offset), at
 * most `limit` of them files (open fails with EMFILE beyond that), sockets
 * unlimited, as measured on the console. Every call a consumer makes on a
 * descriptor goes through ps5_fdv_enter and ps5_fdv_leave, as in a wrapper;
 * the model fails any call that reaches a placeholder socket where a file
 * should be. Each case runs in its own process, since the module is a
 * singleton. The last case runs the same module over the host's real
 * kernel, without a limit, with parking forced.
 */
#define _GNU_SOURCE 1

#include "ps5platform/fdv.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition)                                                                        \
   do {                                                                                         \
      if (!(condition)) {                                                                       \
         fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);          \
         _exit(1);                                                                              \
      }                                                                                         \
   } while (0)

/* ---- the model ---- */

#define MODEL_FDS 4096
#define MODEL_NODES 512
#define NODE_BYTES 64

struct node {
   char path[64];
   char data[NODE_BYTES];
   ino_t ino;
   nlink_t links;
   bool exists;
};

struct description {
   int refs;
   int node;
   off_t position;
   bool socket;
};

static struct node nodes[MODEL_NODES];
static struct description descriptions[MODEL_FDS];
static int table[MODEL_FDS];
static pthread_mutex_t model_lock = PTHREAD_MUTEX_INITIALIZER;
static int limit;
static int live_files, most_live_files, misuse;
static ino_t next_ino;

static int
free_slot(void)
{
   for (int fd = 0; fd < MODEL_FDS; fd++)
      if (table[fd] < 0)
         return fd;
   return -1;
}

static int
new_description(int node, bool socket)
{
   for (int i = 0; i < MODEL_FDS; i++) {
      if (descriptions[i].refs)
         continue;
      descriptions[i] = (struct description){ .refs = 1, .node = node, .socket = socket };
      return i;
   }
   abort();
}

static void
drop(int fd)
{
   struct description *d = &descriptions[table[fd]];
   table[fd] = -1;
   if (--d->refs == 0 && !d->socket)
      live_files--;
}

static int
model_open(const char *path, int flags, mode_t mode)
{
   (void)flags;
   (void)mode;
   pthread_mutex_lock(&model_lock);
   int node = -1;
   for (int i = 0; i < MODEL_NODES; i++)
      if (nodes[i].exists && strcmp(nodes[i].path, path) == 0)
         node = i;
   int result = -1;
   if (node < 0) {
      errno = ENOENT;
   } else if (live_files >= limit) {
      errno = EMFILE;
   } else {
      int fd = free_slot();
      table[fd] = new_description(node, false);
      live_files++;
      if (live_files > most_live_files)
         most_live_files = live_files;
      result = fd;
   }
   pthread_mutex_unlock(&model_lock);
   return result;
}

static int
model_close(int fd)
{
   pthread_mutex_lock(&model_lock);
   int result = -1;
   if ((unsigned)fd < MODEL_FDS && table[fd] >= 0) {
      drop(fd);
      result = 0;
   } else {
      errno = EBADF;
   }
   pthread_mutex_unlock(&model_lock);
   return result;
}

static int
model_dup2(int from, int to)
{
   pthread_mutex_lock(&model_lock);
   int result = -1;
   if ((unsigned)from >= MODEL_FDS || (unsigned)to >= MODEL_FDS || table[from] < 0) {
      errno = EBADF;
   } else {
      if (from != to) {
         if (table[to] >= 0)
            drop(to);
         table[to] = table[from];
         descriptions[table[to]].refs++;
      }
      result = to;
   }
   pthread_mutex_unlock(&model_lock);
   return result;
}

static int
model_dup(int from)
{
   pthread_mutex_lock(&model_lock);
   int result = -1;
   if ((unsigned)from >= MODEL_FDS || table[from] < 0) {
      errno = EBADF;
   } else {
      int fd = free_slot();
      table[fd] = table[from];
      descriptions[table[fd]].refs++;
      result = fd;
   }
   pthread_mutex_unlock(&model_lock);
   return result;
}

static off_t
model_lseek(int fd, off_t offset, int whence)
{
   pthread_mutex_lock(&model_lock);
   off_t result = -1;
   if ((unsigned)fd >= MODEL_FDS || table[fd] < 0) {
      errno = EBADF;
   } else if (descriptions[table[fd]].socket) {
      errno = ESPIPE;
   } else {
      struct description *d = &descriptions[table[fd]];
      off_t base = whence == SEEK_CUR ? d->position : 0;
      d->position = base + offset;
      result = d->position;
   }
   pthread_mutex_unlock(&model_lock);
   return result;
}

static int
model_fstat(int fd, struct stat *status)
{
   pthread_mutex_lock(&model_lock);
   int result = -1;
   if ((unsigned)fd >= MODEL_FDS || table[fd] < 0) {
      errno = EBADF;
   } else {
      struct description *d = &descriptions[table[fd]];
      memset(status, 0, sizeof(*status));
      status->st_dev = 7;
      if (d->socket) {
         status->st_mode = S_IFSOCK | 0600;
         status->st_ino = 100000 + (ino_t)(d - descriptions);
         status->st_nlink = 1;
      } else {
         status->st_mode = S_IFREG | 0644;
         status->st_ino = nodes[d->node].ino;
         status->st_nlink = nodes[d->node].links;
         status->st_size = NODE_BYTES;
      }
      result = 0;
   }
   pthread_mutex_unlock(&model_lock);
   return result;
}

static int
model_placeholder(void)
{
   pthread_mutex_lock(&model_lock);
   int fd = free_slot();
   table[fd] = new_description(-1, true);
   pthread_mutex_unlock(&model_lock);
   return fd;
}

/* What a consumer's read() wrapper does, against the model: the descriptor
 * must hold the file when the call is made. */
static ssize_t
model_read(int fd, char *buffer, size_t bytes)
{
   ps5_fdv_pin pin;
   if (ps5_fdv_enter(fd, &pin) != 0)
      return -1;
   pthread_mutex_lock(&model_lock);
   ssize_t result = -1;
   if ((unsigned)fd >= MODEL_FDS || table[fd] < 0) {
      errno = EBADF;
   } else if (descriptions[table[fd]].socket) {
      misuse++; /* a call reached a placeholder: the wrapper missed it */
      errno = ENOTCONN;
   } else {
      struct description *d = &descriptions[table[fd]];
      size_t left = d->position < NODE_BYTES ? (size_t)(NODE_BYTES - d->position) : 0;
      size_t n = bytes < left ? bytes : left;
      memcpy(buffer, nodes[d->node].data + d->position, n);
      d->position += (off_t)n;
      result = (ssize_t)n;
   }
   pthread_mutex_unlock(&model_lock);
   ps5_fdv_leave(pin);
   return result;
}

static off_t
model_rewind(int fd)
{
   ps5_fdv_pin pin;
   if (ps5_fdv_enter(fd, &pin) != 0)
      return -1;
   off_t result = model_lseek(fd, 0, SEEK_SET);
   ps5_fdv_leave(pin);
   return result;
}

static bool
model_is_socket(int fd)
{
   pthread_mutex_lock(&model_lock);
   bool socket = descriptions[table[fd]].socket;
   pthread_mutex_unlock(&model_lock);
   return socket;
}

static const struct ps5_fdv_ops model_ops = {
   .open = model_open,
   .close = model_close,
   .dup2 = model_dup2,
   .lseek = model_lseek,
   .fstat = model_fstat,
   .placeholder = model_placeholder,
};

/* A fresh model with `count` files of recognisable data. */
static void
model_reset(int new_limit, int count)
{
   memset(nodes, 0, sizeof(nodes));
   memset(descriptions, 0, sizeof(descriptions));
   for (int i = 0; i < MODEL_FDS; i++)
      table[i] = -1;
   limit = new_limit;
   live_files = most_live_files = misuse = 0;
   next_ino = 10;
   nodes[0] = (struct node){ .path = "/dev/null", .ino = 1, .links = 1, .exists = true };
   for (int i = 1; i <= count; i++) {
      nodes[i].exists = true;
      nodes[i].ino = next_ino++;
      nodes[i].links = 1;
      snprintf(nodes[i].path, sizeof(nodes[i].path), "/data/file-%03d", i);
      for (int j = 0; j < NODE_BYTES; j++)
         nodes[i].data[j] = (char)('a' + (i * 7 + j) % 26);
   }
}

static char
expect(int node, off_t offset)
{
   return nodes[node].data[offset];
}

/* ---- cases ---- */

static void
start(int new_limit, int count, unsigned high_water, unsigned batch)
{
   model_reset(new_limit, count);
   CHECK(ps5_fdv_init(&model_ops, high_water, batch) == 0);
}

static void
case_beyond_the_limit(void)
{
   enum { COUNT = 200 };
   int fds[COUNT + 1];
   off_t position[COUNT + 1] = { 0 };

   start(24, COUNT, 16, 4);
   for (int i = 1; i <= COUNT; i++) {
      char path[64];
      snprintf(path, sizeof(path), "/data/file-%03d", i);
      fds[i] = ps5_fdv_open(path, O_RDONLY, 0);
      CHECK(fds[i] >= 0);
   }
   CHECK(most_live_files <= 24);
   /* every file in turn, six times, in a stride that defeats recency */
   for (int round = 0; round < 6; round++) {
      for (int k = 0; k < COUNT; k++) {
         int i = 1 + (k * 37 + round * 11) % COUNT;
         char byte[8];
         CHECK(model_read(fds[i], byte, 5) == 5);
         for (int j = 0; j < 5; j++)
            CHECK(byte[j] == expect(i, position[i] + j));
         position[i] += 5;
      }
   }
   CHECK(misuse == 0);
   CHECK(most_live_files <= 24);
   struct ps5_fdv_stats stats;
   ps5_fdv_get_stats(&stats);
   CHECK(stats.parks > 0 && stats.unparks > 0 && stats.failures == 0);
   for (int i = 1; i <= COUNT; i++)
      CHECK(ps5_fdv_close(fds[i]) == 0);
   ps5_fdv_get_stats(&stats);
   CHECK(stats.live == 0 && stats.parked == 0);
   CHECK(live_files == 1 || live_files == 0); /* the init probe is closed: none */
}

static void
case_learns_the_limit(void)
{
   enum { COUNT = 150 };
   int fds[COUNT + 1];

   start(60, COUNT, 1000, 4); /* the water mark is above the real limit */
   for (int i = 1; i <= COUNT; i++) {
      char path[64];
      snprintf(path, sizeof(path), "/data/file-%03d", i);
      fds[i] = ps5_fdv_open(path, O_RDONLY, 0);
      CHECK(fds[i] >= 0);
   }
   struct ps5_fdv_stats stats;
   ps5_fdv_get_stats(&stats);
   CHECK(stats.limit_hits > 0);
   CHECK(stats.high_water < 60);
   CHECK(most_live_files <= 60);
   char byte[4];
   CHECK(model_read(fds[1], byte, 4) == 4 && byte[0] == expect(1, 0));
}

static void
case_dup_shares_the_position(void)
{
   start(100, 4, 0, 0);
   int a = ps5_fdv_open("/data/file-001", O_RDONLY, 0);
   CHECK(a >= 0);
   ps5_fdv_pin pin;
   CHECK(ps5_fdv_enter(a, &pin) == 0);
   int b = model_dup(a);
   ps5_fdv_dup_note(a, b);
   ps5_fdv_leave(pin);

   char byte[8];
   CHECK(model_read(a, byte, 3) == 3 && byte[2] == expect(1, 2));
   CHECK(ps5_fdv_park_idle(10) == 1);
   CHECK(model_is_socket(a) && model_is_socket(b));
   CHECK(live_files == 0);
   /* the other number continues where the first stopped */
   CHECK(model_read(b, byte, 3) == 3 && byte[0] == expect(1, 3) && byte[2] == expect(1, 5));
   CHECK(!model_is_socket(a) && !model_is_socket(b));
   CHECK(model_read(a, byte, 1) == 1 && byte[0] == expect(1, 6));
   CHECK(live_files == 1);
   /* closing one number leaves the file under the other, parked or not */
   CHECK(ps5_fdv_close(a) == 0);
   CHECK(ps5_fdv_park_idle(10) == 1 && model_is_socket(b));
   CHECK(model_read(b, byte, 1) == 1 && byte[0] == expect(1, 7));
   CHECK(ps5_fdv_close(b) == 0);
   CHECK(misuse == 0);
}

static void
case_keep_and_pins(void)
{
   start(100, 4, 0, 0);
   int a = ps5_fdv_open("/data/file-001", O_RDONLY, 0);
   int b = ps5_fdv_open("/data/file-002", O_RDONLY, 0);
   int c = ps5_fdv_open("/data/file-003", O_RDONLY, 0);
   ps5_fdv_pin pin_a, pin_b;

   CHECK(ps5_fdv_enter(a, &pin_a) == 0);
   CHECK(ps5_fdv_enter(b, &pin_b) == 0);
   ps5_fdv_keep(pin_b);
   ps5_fdv_leave(pin_b);
   /* a is pinned, b is kept: only c can be parked */
   CHECK(ps5_fdv_park_idle(10) == 1);
   CHECK(!model_is_socket(a) && !model_is_socket(b) && model_is_socket(c));
   ps5_fdv_leave(pin_a);
   CHECK(ps5_fdv_park_idle(10) == 1);
   CHECK(model_is_socket(a) && !model_is_socket(b));
   CHECK(ps5_fdv_park_idle(10) == 0);
}

static void
case_least_recently_used_goes_first(void)
{
   start(100, 4, 0, 0);
   int fds[4];
   char byte[1];
   for (int i = 0; i < 4; i++)
      fds[i] = ps5_fdv_open(i == 0 ? "/data/file-001" : i == 1 ? "/data/file-002" : i == 2 ? "/data/file-003" : "/data/file-004",
                            O_RDONLY, 0);
   CHECK(model_read(fds[0], byte, 1) == 1); /* 0 is now the most recent */
   CHECK(ps5_fdv_park_idle(2) == 2);
   CHECK(!model_is_socket(fds[0]) && !model_is_socket(fds[3]));
   CHECK(model_is_socket(fds[1]) && model_is_socket(fds[2]));
}

static void
case_rename_and_replacement(void)
{
   start(100, 4, 0, 0);
   strcpy(nodes[1].path, "/old/dir/file");
   int a = ps5_fdv_open("/old/dir/file", O_RDONLY, 0);
   CHECK(a >= 0);
   CHECK(ps5_fdv_park_idle(1) == 1);
   /* the directory is renamed under it */
   strcpy(nodes[1].path, "/new/dir/file");
   ps5_fdv_renamed("/old/dir", "/new/dir");
   char byte[2];
   CHECK(model_read(a, byte, 2) == 2 && byte[0] == expect(1, 0));
   CHECK(ps5_fdv_close(a) == 0);

   /* a different file under the name is never read in its place */
   int b = ps5_fdv_open("/data/file-002", O_RDONLY, 0);
   CHECK(ps5_fdv_park_idle(1) == 1);
   nodes[2].ino = next_ino++;
   CHECK(model_read(b, byte, 1) == -1 && errno == EIO);
   CHECK(model_read(b, byte, 1) == -1 && errno == EIO);
   struct ps5_fdv_stats stats;
   ps5_fdv_get_stats(&stats);
   CHECK(stats.failures == 1);
   CHECK(ps5_fdv_close(b) == 0);

   /* a file that is gone cannot be reopened, and says why */
   int c = ps5_fdv_open("/data/file-003", O_RDONLY, 0);
   CHECK(ps5_fdv_park_idle(1) == 1);
   nodes[3].exists = false;
   CHECK(model_read(c, byte, 1) == -1 && errno == ENOENT);
}

static void
case_unlinked_stays_open(void)
{
   start(100, 2, 0, 0);
   int a = ps5_fdv_open("/data/file-001", O_RDONLY, 0);
   nodes[1].links = 0;
   CHECK(ps5_fdv_park_idle(1) == 0);
   CHECK(!model_is_socket(a));
   char byte[1];
   CHECK(model_read(a, byte, 1) == 1);
}

static void
case_descriptors_received(void)
{
   start(100, 6, 0, 0);
   int a = ps5_fdv_open("/data/file-001", O_RDONLY, 0);
   char byte[8];
   CHECK(model_read(a, byte, 2) == 2);

   /* the sender marks the file in flight, the kernel copies the description,
    * the receiver's number joins the file */
   ps5_fdv_pin pin;
   CHECK(ps5_fdv_sending(a, &pin) == 0 && pin);
   int received = model_dup(a);
   ps5_fdv_sent(pin, 1);
   CHECK(ps5_fdv_park_idle(10) == 0); /* in flight: not parked */
   CHECK(ps5_fdv_received(received) == 1);
   CHECK(ps5_fdv_park_idle(10) == 1);
   CHECK(model_is_socket(a) && model_is_socket(received));
   CHECK(model_read(received, byte, 2) == 2 && byte[0] == expect(1, 2));
   CHECK(model_read(a, byte, 1) == 1 && byte[0] == expect(1, 4));

   /* a copy nothing was sent for is not tracked */
   int b = ps5_fdv_open("/data/file-002", O_RDONLY, 0);
   int stray = model_dup(b);
   CHECK(ps5_fdv_received(stray) == 0);

   /* a send that failed leaves the file where it was */
   CHECK(ps5_fdv_sending(b, &pin) == 0);
   ps5_fdv_sent(pin, 0);
   CHECK(ps5_fdv_park_idle(10) == 2 && model_is_socket(a) && model_is_socket(b));

   /* two files of one device and inode in flight cannot be told apart: neither
    * is parked from then on */
   int x = ps5_fdv_open("/data/file-003", O_RDONLY, 0);
   int y = ps5_fdv_open("/data/file-003", O_RDONLY, 0);
   ps5_fdv_pin pin_y;
   CHECK(ps5_fdv_sending(x, &pin) == 0 && ps5_fdv_sending(y, &pin_y) == 0);
   int copy_x = model_dup(x), copy_y = model_dup(y);
   ps5_fdv_sent(pin, 1);
   ps5_fdv_sent(pin_y, 1);
   CHECK(ps5_fdv_received(copy_x) == 0);
   CHECK(ps5_fdv_received(copy_y) == 0);
   CHECK(ps5_fdv_park_idle(10) == 0);
   CHECK(!model_is_socket(x) && !model_is_socket(y));
   struct ps5_fdv_stats stats;
   ps5_fdv_get_stats(&stats);
   CHECK(!stats.disabled);
   CHECK(misuse == 0);
}

static void
case_closed_while_parked(void)
{
   start(100, 3, 0, 0);
   int a = ps5_fdv_open("/data/file-001", O_RDONLY, 0);
   CHECK(ps5_fdv_park_idle(1) == 1);
   CHECK(ps5_fdv_close(a) == 0);
   struct ps5_fdv_stats stats;
   ps5_fdv_get_stats(&stats);
   CHECK(stats.parked == 0 && stats.live == 0);
   /* the number is free again and may be a different file */
   int b = ps5_fdv_open("/data/file-002", O_RDONLY, 0);
   char byte[1];
   CHECK(model_read(b, byte, 1) == 1 && byte[0] == expect(2, 0));
   CHECK(misuse == 0);
}

static void
case_rewind_while_parked(void)
{
   start(100, 2, 0, 0);
   int a = ps5_fdv_open("/data/file-001", O_RDONLY, 0);
   char byte[8];
   CHECK(model_read(a, byte, 6) == 6);
   CHECK(ps5_fdv_park_idle(1) == 1);
   CHECK(model_rewind(a) == 0);
   CHECK(model_read(a, byte, 2) == 2 && byte[0] == expect(1, 0));
}

/* Threads reading their own files, with the limit far below the files in use
 * and a thread parking as fast as it can. */
struct stress {
   int first, step, rounds;
   int fds[512];
   off_t position[512];
   int failed;
};

static void *
stress_thread(void *argument)
{
   struct stress *s = argument;
   unsigned seed = (unsigned)s->first * 7919u;
   int count = 0;
   for (int node = s->first; node <= 300; node += s->step)
      count++;
   for (int round = 0; round < s->rounds; round++) {
      int slot = (int)(rand_r(&seed) % (unsigned)count);
      int node = s->first + slot * s->step;
      if (s->position[slot] + 4 > NODE_BYTES) {
         if (model_rewind(s->fds[slot]) != 0)
            s->failed++;
         s->position[slot] = 0;
      }
      char byte[4];
      if (model_read(s->fds[slot], byte, 4) != 4) {
         s->failed++;
         continue;
      }
      for (int j = 0; j < 4; j++)
         if (byte[j] != expect(node, s->position[slot] + j))
            s->failed++;
      s->position[slot] += 4;
   }
   return NULL;
}

static void *
parker_thread(void *argument)
{
   atomic_int *stop = argument;
   while (!atomic_load(stop))
      ps5_fdv_park_idle(16);
   return NULL;
}

static void
case_threads(void)
{
   enum { THREADS = 6 };
   struct stress work[THREADS];
   pthread_t threads[THREADS], parker;
   atomic_int stop = 0;

   start(40, 300, 30, 8);
   memset(work, 0, sizeof(work));
   for (int t = 0; t < THREADS; t++) {
      work[t].first = 1 + t;
      work[t].step = THREADS;
      work[t].rounds = 30000;
      int slot = 0;
      for (int node = work[t].first; node <= 300; node += THREADS, slot++) {
         char path[64];
         snprintf(path, sizeof(path), "/data/file-%03d", node);
         work[t].fds[slot] = ps5_fdv_open(path, O_RDONLY, 0);
         CHECK(work[t].fds[slot] >= 0);
      }
   }
   CHECK(pthread_create(&parker, NULL, parker_thread, &stop) == 0);
   for (int t = 0; t < THREADS; t++)
      CHECK(pthread_create(&threads[t], NULL, stress_thread, &work[t]) == 0);
   for (int t = 0; t < THREADS; t++)
      pthread_join(threads[t], NULL);
   atomic_store(&stop, 1);
   pthread_join(parker, NULL);
   for (int t = 0; t < THREADS; t++)
      CHECK(work[t].failed == 0);
   CHECK(misuse == 0);
   CHECK(most_live_files <= 40);
   struct ps5_fdv_stats stats;
   ps5_fdv_get_stats(&stats);
   CHECK(stats.parks > 100 && stats.failures == 0);
}

/* ---- the same module over the host's own kernel ---- */

static int
host_open(const char *path, int flags, mode_t mode)
{
   return open(path, flags, mode);
}

static int
host_placeholder(void)
{
   return socket(AF_UNIX, SOCK_STREAM, 0);
}

/* The most one round parks is 64; everything idle goes by repeating it. */
static unsigned
park_all(void)
{
   unsigned total = 0, round;
   while ((round = ps5_fdv_park_idle(64)) != 0)
      total += round;
   return total;
}

static void
case_host_kernel(void)
{
   enum { COUNT = 300 };
   static const struct ps5_fdv_ops ops = {
      .open = host_open,
      .close = close,
      .dup2 = dup2,
      .lseek = lseek,
      .fstat = fstat,
      .placeholder = host_placeholder,
   };
   char directory[] = "/tmp/ps5-fdv-XXXXXX";
   int fds[COUNT];
   char path[128];

   CHECK(mkdtemp(directory));
   CHECK(ps5_fdv_init(&ops, 0, 0) == 0);
   for (int i = 0; i < COUNT; i++) {
      snprintf(path, sizeof(path), "%s/f%03d", directory, i);
      int fd = open(path, O_WRONLY | O_CREAT, 0644);
      CHECK(fd >= 0);
      char text[40];
      int length = snprintf(text, sizeof(text), "file %03d: 0123456789", i);
      CHECK(write(fd, text, (size_t)length) == length);
      close(fd);
      fds[i] = ps5_fdv_open(path, O_RDWR, 0);
      CHECK(fds[i] >= 0);
   }
   struct ps5_fdv_stats stats;
   ps5_fdv_get_stats(&stats);
   CHECK(stats.parks > 0); /* past the default water mark, files were parked */
   CHECK(park_all() > 0);

   for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < COUNT; i++) {
         ps5_fdv_pin pin;
         char expected[32], got[16];
         CHECK(ps5_fdv_enter(fds[i], &pin) == 0);
         CHECK(read(fds[i], got, 5) == 5);
         ps5_fdv_leave(pin);
         snprintf(expected, sizeof(expected), "file %03d: 0123456789", i);
         CHECK(memcmp(got, expected + pass * 5, 5) == 0);
      }
      CHECK(park_all() > 0);
   }
   /* a copy of a descriptor shares the offset, parked or not */
   ps5_fdv_pin pin;
   CHECK(ps5_fdv_enter(fds[7], &pin) == 0);
   int copy = dup(fds[7]);
   ps5_fdv_dup_note(fds[7], copy);
   ps5_fdv_leave(pin);
   CHECK(park_all() > 0);
   struct stat status;
   CHECK(fstat(copy, &status) == 0 && S_ISSOCK(status.st_mode));
   char got[4];
   CHECK(ps5_fdv_enter(copy, &pin) == 0);
   CHECK(read(copy, got, 4) == 4);
   ps5_fdv_leave(pin);
   CHECK(memcmp(got, "0123", 4) == 0); /* bytes 10..13, after two passes of five */
   CHECK(ps5_fdv_enter(fds[7], &pin) == 0);
   CHECK(fstat(fds[7], &status) == 0 && S_ISREG(status.st_mode));
   CHECK(lseek(fds[7], 0, SEEK_CUR) == 14);
   ps5_fdv_leave(pin);

   for (int i = 0; i < COUNT; i++) {
      CHECK(ps5_fdv_close(fds[i]) == 0);
      snprintf(path, sizeof(path), "%s/f%03d", directory, i);
      unlink(path);
   }
   CHECK(ps5_fdv_close(copy) == 0);
   rmdir(directory);
}

/* ---- the runner ---- */

static void
run(const char *name, void (*body)(void))
{
   fflush(stdout);
   pid_t child = fork();
   if (child == 0) {
      body();
      _exit(0);
   }
   int status = 0;
   waitpid(child, &status, 0);
   if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      fprintf(stderr, "FAILED: %s\n", name);
      exit(1);
   }
   printf("ok   %s\n", name);
}

int
main(void)
{
   run("beyond the limit", case_beyond_the_limit);
   run("learns the limit", case_learns_the_limit);
   run("dup shares the position", case_dup_shares_the_position);
   run("keep and pins", case_keep_and_pins);
   run("least recently used goes first", case_least_recently_used_goes_first);
   run("rename and replacement", case_rename_and_replacement);
   run("unlinked stays open", case_unlinked_stays_open);
   run("descriptors received", case_descriptors_received);
   run("closed while parked", case_closed_while_parked);
   run("rewind while parked", case_rewind_while_parked);
   run("threads", case_threads);
   run("host kernel", case_host_kernel);
   printf("fdv: all passed\n");
   return 0;
}
