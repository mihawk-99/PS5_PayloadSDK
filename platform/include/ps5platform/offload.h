/*
 * PS5 Platform - writing files through the console's FTP server.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console lets a title write about 1.3 GiB at 242 MiB/s and then about
 * 2 MiB/s, however it writes and with however many threads; an FTP server on
 * the console, another process, is held back per connection (about 7 MiB/s)
 * and in all (about 22 MiB/s), and not by the title's budget (docs/PROBE.md,
 * "Sustained writes"). So once the title's burst is spent, a large file
 * written in order goes faster through the server, and several files at once
 * faster still.
 *
 * A stream appends to one file the title has created, from where the file
 * ends: its bytes are queued, a thread of its own sends them over a loopback
 * connection, and the server confirms them every 16 MiB. Until a piece is
 * confirmed the stream keeps it, and if the server fails the stream writes
 * what is left itself, so no byte is lost either way. The file is the title's
 * own the whole time: what the server writes lands in it (same inode).
 */
#ifndef PS5PLATFORM_OFFLOAD_H
#define PS5PLATFORM_OFFLOAD_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Finds the FTP server on the loopback and the folder it sees `local_root` as:
 * a marker file created in `local_root` is looked for in each entry of each
 * of `server_roots` (and in each root itself; none: where homebrew folders
 * are kept on the console's storage, extended storage and USB drives). The
 * answer is kept in `local_root`/.ps5-offload, so a later start checks it
 * instead of scanning.
 * Returns 0 when streams can be made; only the first call does the work. */
int ps5_offload_setup(const char *local_root, const char *const *server_roots, unsigned count);

/* Notes a write the caller made itself: `bytes` in `ms` milliseconds. Writes
 * of 1 MiB or more slower than 50 MiB/s mean the title's burst is spent. */
void ps5_offload_note_write(size_t bytes, double ms);

/* Whether large files should go through the server now: the burst was seen
 * spent in the last two minutes, and setup succeeded. */
bool ps5_offload_wanted(void);

struct ps5_offload;

/* Starts a stream to `local_path` (under the setup's `local_root`), a file
 * that is `at` bytes long, which the stream extends. NULL when it cannot. */
struct ps5_offload *ps5_offload_begin(const char *local_path, unsigned long long at);

/* Queues `size` bytes (copied), which follow what was queued before; waits
 * while 16 MiB are queued. Returns 0, or -1 once the stream has failed
 * beyond repair (its file could not be written either way). */
int ps5_offload_write(struct ps5_offload *stream, const void *bytes, size_t size);

/* Waits until every queued byte is in the file, and frees the stream.
 * Returns 0 when all of them are. */
int ps5_offload_end(struct ps5_offload *stream);

#ifdef __cplusplus
}
#endif

#endif
