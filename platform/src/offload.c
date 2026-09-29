/*
 * PS5 Platform - writing files through the console's FTP server
 * (include/ps5platform/offload.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ps5platform/offload.h"
#include "ps5platform/ftp.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OFFLOAD_PIECE (16u << 20) /* what the server confirms at a time */
#define OFFLOAD_QUEUE (16u << 20) /* queued at most before the writer waits */
#define OFFLOAD_BLOCK (1u << 20)
#define OFFLOAD_NAMES 512

static pthread_mutex_t offload_lock = PTHREAD_MUTEX_INITIALIZER;
static int offload_state; /* 0: not set up, 1: ready, -1: failed */
static unsigned offload_port;
static char offload_local[512], offload_server[512];
static double offload_spent_at = -1e9;

static double
offload_now(void)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

struct offload_names {
   unsigned count;
   char *names[OFFLOAD_NAMES];
};

static void
offload_collect(void *context, const char *name, int directory)
{
   struct offload_names *names = context;
   if (directory && names->count < OFFLOAD_NAMES)
      names->names[names->count++] = strdup(name);
}

/* Whether the server sees the marker in `folder`. */
static int
offload_marker_in(struct ps5_ftp *ftp, const char *folder, const char *marker)
{
   char path[1024];
   if (snprintf(path, sizeof(path), "%s/%s", folder, marker) >= (int)sizeof(path))
      return 0;
   return ps5_ftp_size(ftp, path) >= 0;
}

/* The folder, among `roots` and the folders in them, where the server sees
 * the marker; into `found`. */
static int
offload_search(struct ps5_ftp *ftp, const char *const *roots, unsigned count, const char *marker, char *found,
               size_t size)
{
   for (unsigned i = 0; i < count; ++i) {
      if (offload_marker_in(ftp, roots[i], marker)) {
         snprintf(found, size, "%s", roots[i]);
         return 1;
      }
      struct offload_names names = {0};
      /* The names first: the control connection is the listing's until it ends. */
      ps5_ftp_list(ftp, roots[i], offload_collect, &names);
      int hit = 0;
      for (unsigned n = 0; n < names.count; ++n) {
         char folder[1024];
         if (!hit && snprintf(folder, sizeof(folder), "%s/%s", roots[i], names.names[n]) < (int)sizeof(folder) &&
             offload_marker_in(ftp, folder, marker)) {
            snprintf(found, size, "%s", folder);
            hit = 1;
         }
         free(names.names[n]);
      }
      if (hit)
         return 1;
   }
   return 0;
}

int
ps5_offload_setup(const char *local_root, const char *const *server_roots, unsigned count)
{
   pthread_mutex_lock(&offload_lock);
   if (offload_state) {
      const int ready = offload_state == 1 ? 0 : -1;
      pthread_mutex_unlock(&offload_lock);
      return ready;
   }
   offload_state = -1;
   /* Where homebrew folders are kept, on the console's storage, its extended
    * storage and USB drives. */
   static const char *const homebrew[] = {"/data/homebrew", "/mnt/ext0/homebrew", "/mnt/ext1/homebrew",
                                          "/mnt/usb0/homebrew", "/mnt/usb1/homebrew"};
   if (!server_roots || !count) {
      server_roots = homebrew;
      count = sizeof(homebrew) / sizeof(homebrew[0]);
   }
   snprintf(offload_local, sizeof(offload_local), "%s", local_root);
   char marker[96], marker_path[640], cache_path[640];
   snprintf(marker, sizeof(marker), "ps5-offload-%ld-%ld.marker", (long)getpid(), (long)time(NULL));
   snprintf(marker_path, sizeof(marker_path), "%s/%s", local_root, marker);
   snprintf(cache_path, sizeof(cache_path), "%s/.ps5-offload", local_root);
   const int made = open(marker_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
   if (made < 0) {
      pthread_mutex_unlock(&offload_lock);
      return -1;
   }
   close(made);

   /* What the last start found, checked; else a scan and a search. */
   unsigned port = 0, cached_port = 0;
   char server[512] = {0}, cached_server[512] = {0};
   FILE *cache = fopen(cache_path, "r");
   if (cache) {
      if (fscanf(cache, "%u %511[^\n]", &cached_port, cached_server) != 2)
         cached_port = 0;
      fclose(cache);
   }
   struct ps5_ftp ftp;
   int found = 0;
   if (cached_port && ps5_ftp_open(&ftp, cached_port) == 0) {
      port = cached_port;
      snprintf(server, sizeof(server), "%s", cached_server);
      found = offload_marker_in(&ftp, server, marker) ||
              offload_search(&ftp, server_roots, count, marker, server, sizeof(server));
      ps5_ftp_close(&ftp);
   }
   if (!found) {
      port = ps5_ftp_find_local(1, 65535, 150);
      if (port && ps5_ftp_open(&ftp, port) == 0) {
         found = offload_search(&ftp, server_roots, count, marker, server, sizeof(server));
         ps5_ftp_close(&ftp);
      }
   }
   if (found && (port != cached_port || strcmp(server, cached_server)) && (cache = fopen(cache_path, "w"))) {
      fprintf(cache, "%u %s\n", port, server);
      fclose(cache);
   }
   unlink(marker_path);
   if (found) {
      offload_port = port;
      snprintf(offload_server, sizeof(offload_server), "%s", server);
      offload_state = 1;
   }
   pthread_mutex_unlock(&offload_lock);
   return found ? 0 : -1;
}

void
ps5_offload_note_write(size_t bytes, double ms)
{
   if (bytes < OFFLOAD_BLOCK || ms <= 0 || (double)bytes / 1048576.0 / (ms / 1000.0) >= 50.0)
      return;
   pthread_mutex_lock(&offload_lock);
   offload_spent_at = offload_now();
   pthread_mutex_unlock(&offload_lock);
}

bool
ps5_offload_wanted(void)
{
   pthread_mutex_lock(&offload_lock);
   const bool wanted = offload_now() - offload_spent_at < 120.0;
   pthread_mutex_unlock(&offload_lock);
   return wanted;
}

struct offload_block {
   struct offload_block *next;
   size_t size;
   char bytes[];
};

struct ps5_offload {
   pthread_t thread;
   pthread_mutex_t lock;
   pthread_cond_t changed;
   struct offload_block *head, *tail;
   size_t queued;
   bool ending, failed;
   unsigned long long at;
   char local_path[640], server_path[1152];
};

static void
offload_free(struct offload_block *block)
{
   while (block) {
      struct offload_block *next = block->next;
      free(block);
      block = next;
   }
}

/* Writes the blocks from `at` with pwrite() itself. */
static bool
offload_write_local(struct ps5_offload *stream, int *fd, struct offload_block *block, unsigned long long at)
{
   if (*fd < 0 && (*fd = open(stream->local_path, O_WRONLY)) < 0)
      return false;
   for (; block; at += block->size, block = block->next)
      for (size_t done = 0; done < block->size;) {
         const ssize_t wrote = pwrite(*fd, block->bytes + done, block->size - done, (off_t)(at + done));
         if (wrote <= 0)
            return false;
         done += (size_t)wrote;
      }
   return true;
}

static void *
offload_run(void *argument)
{
   struct ps5_offload *stream = argument;
   struct ps5_ftp ftp;
   bool through = ps5_ftp_open(&ftp, offload_port) == 0;
   int data = -1, local = -1;
   /* The piece the server has not confirmed yet, which the stream keeps. */
   struct offload_block *piece = NULL, *piece_tail = NULL;
   size_t piece_size = 0;
   unsigned long long piece_at = stream->at;
   for (;;) {
      pthread_mutex_lock(&stream->lock);
      while (!stream->head && !stream->ending)
         pthread_cond_wait(&stream->changed, &stream->lock);
      struct offload_block *block = stream->head;
      if (block) {
         stream->head = block->next;
         if (!stream->head)
            stream->tail = NULL;
         stream->queued -= block->size;
         block->next = NULL;
         pthread_cond_broadcast(&stream->changed);
      }
      pthread_mutex_unlock(&stream->lock);
      const bool last = !block;
      if (block) {
         /* A piece starts where the server's file ends. */
         if (through && data < 0)
            through = ps5_ftp_size(&ftp, stream->server_path) == (long long)piece_at &&
                      (data = ps5_ftp_append_begin(&ftp, stream->server_path)) >= 0;
         if (through && ps5_ftp_send(data, block->bytes, block->size) != 0)
            through = false;
         if (piece_tail)
            piece_tail->next = block;
         else
            piece = block;
         piece_tail = block;
         piece_size += block->size;
      }
      if (through && data >= 0 && (piece_size >= OFFLOAD_PIECE || last)) {
         const int confirmed = ps5_ftp_transfer_end(&ftp, data);
         data = -1;
         if (confirmed == 0) {
            offload_free(piece);
            piece = piece_tail = NULL;
            piece_at += piece_size;
            piece_size = 0;
         } else
            through = false;
      }
      if (!through && piece) {
         /* The server failed: what it did not confirm, and all that follows,
          * the stream writes itself. */
         if (data >= 0) {
            close(data);
            data = -1;
         }
         if (!offload_write_local(stream, &local, piece, piece_at)) {
            pthread_mutex_lock(&stream->lock);
            stream->failed = true;
            pthread_mutex_unlock(&stream->lock);
         }
         offload_free(piece);
         piece = piece_tail = NULL;
         piece_at += piece_size;
         piece_size = 0;
      }
      if (last)
         break;
   }
   if (ftp.control >= 0)
      ps5_ftp_close(&ftp);
   if (local >= 0)
      close(local);
   return NULL;
}

struct ps5_offload *
ps5_offload_begin(const char *local_path, unsigned long long at)
{
   pthread_mutex_lock(&offload_lock);
   const bool ready = offload_state == 1;
   const size_t root = strlen(offload_local);
   pthread_mutex_unlock(&offload_lock);
   if (!ready || strncmp(local_path, offload_local, root) || (local_path[root] != '/' && local_path[root]))
      return NULL;
   struct ps5_offload *stream = calloc(1, sizeof(*stream));
   if (!stream)
      return NULL;
   stream->at = at;
   snprintf(stream->local_path, sizeof(stream->local_path), "%s", local_path);
   snprintf(stream->server_path, sizeof(stream->server_path), "%s%s", offload_server, local_path + root);
   pthread_mutex_init(&stream->lock, NULL);
   pthread_cond_init(&stream->changed, NULL);
   pthread_attr_t attributes;
   pthread_attr_init(&attributes);
   pthread_attr_setstacksize(&attributes, 256u << 10);
   const int started = pthread_create(&stream->thread, &attributes, offload_run, stream);
   pthread_attr_destroy(&attributes);
   if (started != 0) {
      pthread_cond_destroy(&stream->changed);
      pthread_mutex_destroy(&stream->lock);
      free(stream);
      return NULL;
   }
   return stream;
}

int
ps5_offload_write(struct ps5_offload *stream, const void *bytes, size_t size)
{
   const char *at = bytes;
   while (size) {
      const size_t take = size < OFFLOAD_BLOCK ? size : OFFLOAD_BLOCK;
      struct offload_block *block = malloc(sizeof(*block) + take);
      if (!block)
         return -1;
      block->next = NULL;
      block->size = take;
      memcpy(block->bytes, at, take);
      pthread_mutex_lock(&stream->lock);
      while (stream->queued >= OFFLOAD_QUEUE && !stream->failed)
         pthread_cond_wait(&stream->changed, &stream->lock);
      if (stream->failed) {
         pthread_mutex_unlock(&stream->lock);
         free(block);
         return -1;
      }
      if (stream->tail)
         stream->tail->next = block;
      else
         stream->head = block;
      stream->tail = block;
      stream->queued += take;
      pthread_cond_broadcast(&stream->changed);
      pthread_mutex_unlock(&stream->lock);
      at += take;
      size -= take;
   }
   return 0;
}

int
ps5_offload_end(struct ps5_offload *stream)
{
   pthread_mutex_lock(&stream->lock);
   stream->ending = true;
   pthread_cond_broadcast(&stream->changed);
   pthread_mutex_unlock(&stream->lock);
   pthread_join(stream->thread, NULL);
   const int result = stream->failed ? -1 : 0;
   offload_free(stream->head);
   pthread_cond_destroy(&stream->changed);
   pthread_mutex_destroy(&stream->lock);
   free(stream);
   return result;
}
