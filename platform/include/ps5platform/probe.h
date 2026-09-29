/*
 * PS5 Platform - the console capability probe.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Establishes, on the console and in the calling process, the memory
 * behaviour this layer builds on: the direct-memory pool, executable code in
 * direct memory, its placement, rewriting, fault recovery, protection costs
 * and accounting. Every result is one line through the caller's log; nothing
 * is printed per frame and nothing is left mapped when it returns.
 */
#ifndef PS5PLATFORM_PROBE_H
#define PS5PLATFORM_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ps5_probe_log_fn)(void *context, const char *line);

/* The large test: 4 GiB of guest memory beside 1 GiB of executable code. */
#define PS5_PROBE_LARGE 0x1u
/* The shared-memory JIT interface, which may not be granted to a title. */
#define PS5_PROBE_JIT_API 0x2u
/* The huge test: where the kernel grants large virtual ranges, then 10 GiB
 * mapped and every word of it written and read back at once, as one
 * allocation and as ten 1 GiB allocations. */
#define PS5_PROBE_HUGE 0x4u
/* The full test: the largest direct allocation mapped, every word of it
 * written and read back, and what can still be allocated while it is held. */
#define PS5_PROBE_FULL 0x8u

/* Runs every test and returns how many checks failed. */
int ps5_platform_probe(ps5_probe_log_fn log, void *context, unsigned flags);

/* The file test (src/probe_files.c): 256 MiB written in a file of `directory`
 * in chunks of 100 KiB, 1 MiB and 16 MiB, timed to the last write and after
 * fsync, read back and compared, then removed; once more with O_DIRECT, for
 * its answer only; then through stdio with the stream's own buffer and with
 * 1 MiB and 4 MiB ones. Returns how many checks failed. */
int ps5_platform_probe_files(ps5_probe_log_fn log, void *context, const char *directory);

/* The sustained write test (src/probe_files.c): `mib` MiB written in one file
 * of `directory`, 16 MiB at a time, with O_DIRECT, then buffered with an
 * fsync() after every 256 MiB, then buffered alone, each timed per 256 MiB
 * and stopped after `seconds`. The 256 MiB the file test writes fit in the
 * kernel's write cache; a game's package does not. Returns how many checks
 * failed (a write that fails). */
int ps5_platform_probe_writes(ps5_probe_log_fn log, void *context, const char *directory, unsigned mib,
                              unsigned seconds);

/* The write route test (src/probe_files.c): in each of `directories`, `mib`
 * MiB written in one file with write(), 16 MiB at a time, then through
 * MAP_SHARED mappings of 64 MiB with no write() at all, then with write()
 * again, each timed per 256 MiB and stopped after `seconds` (the last after a
 * third of it), fsync() after the last. A title's write() runs
 * at full speed for a burst and then at about 2 MiB/s while another process
 * writes the same folder faster; this says whether the route or the path
 * (a title's /app0 against the folder's own path) is what is held back.
 * Returns how many checks failed (a write that fails). */
int ps5_platform_probe_write_routes(ps5_probe_log_fn log, void *context, const char *const *directories,
                                    unsigned count, unsigned mib, unsigned seconds);

/* The offload test (src/probe_files.c): `mib` MiB appended through the FTP
 * server on 127.0.0.1:`port` (0: the first the loopback scan finds,
 * include/ps5platform/ftp.h) to a file of `directory`, named
 * `server_directory` by the server (NULL: what _fstatfs() says `directory` is
 * mounted from), half to a file the server creates and the caller reads back
 * after, half appended to a file the caller has created and holds open (its
 * first 16 MiB written with write()), which must stay the same file; each
 * timed per 256 MiB and stopped after half of `seconds`. "/dev/null" as the
 * server's folder measures the route alone. Returns how many checks failed. */
int ps5_platform_probe_ftp_offload(ps5_probe_log_fn log, void *context, const char *directory,
                                   const char *server_directory, unsigned port, unsigned mib,
                                   unsigned seconds);

/* The thread test (src/probe_threads.c): the stack size a fresh attribute
 * object reports, the calling thread's stack, and the stacks a thread created
 * with no attributes and one asking for 2 MiB run on, read back from inside
 * each. Returns how many checks failed. */
int ps5_platform_probe_threads(ps5_probe_log_fn log, void *context);

#ifdef __cplusplus
}
#endif

#endif /* PS5PLATFORM_PROBE_H */
