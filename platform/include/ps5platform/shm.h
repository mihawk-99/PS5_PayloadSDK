/*
 * PS5 Platform - shared-memory objects on direct memory, and virtual ranges.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An emulator's arena: one object of guest memory mapped at several addresses
 * (mirrors, a fastmem window), inside a reserved virtual range. Every view of
 * a POSIX shared-memory object is charged to the title's flexible budget; one
 * direct-memory allocation can be mapped any number of times and is charged
 * to nothing but the direct pool (docs/PROBE.md). This is Dolphin's MemArena
 * on the console, generalised so LRPS2's memshm backend and PPSSPP's arena
 * are the same code.
 */
#ifndef PS5PLATFORM_SHM_H
#define PS5PLATFORM_SHM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ps5_shm {
   int64_t direct_start;
   size_t bytes; /* rounded up to the 64 KiB direct-memory unit */
};

/* View protection. Execute is granted by a protection change after the map
 * (execute at map time is refused). */
#define PS5_SHM_READ 0x1
#define PS5_SHM_WRITE 0x2
#define PS5_SHM_EXEC 0x4

/* View placement: exactly at the address, replacing what is there (a part of
 * a reservation); otherwise the address is a hint, or 0 for anywhere outside
 * the GPU window. */
#define PS5_SHM_FIXED 0x1u
/* Unmapping: leave the range reserved, as an arena's hole. */
#define PS5_SHM_KEEP_RESERVED 0x2u

#define PS5_SHM_NO_PLACE ((int)0x80ff0101)
#define PS5_SHM_BAD_REQUEST ((int)0x80ff0102)

int ps5_shm_create(size_t bytes, struct ps5_shm *shm);
/* Releases the memory; every view must be unmapped first. */
void ps5_shm_destroy(struct ps5_shm *shm);

int ps5_shm_map(const struct ps5_shm *shm, size_t offset, size_t bytes, void *address,
                int protection, unsigned flags, void **view);
int ps5_shm_unmap(void *view, size_t bytes, unsigned flags);

/* A reserved range: address space nothing else is given, outside the GPU
 * window, at the hint when it is free. */
int ps5_vrange_reserve(size_t bytes, void *hint, size_t alignment, void **base);
int ps5_vrange_release(void *base, size_t bytes);

struct ps5_shm_stats {
   uint64_t objects, object_bytes;
   uint64_t views, view_bytes;
   uint64_t ranges, range_bytes;
};
void ps5_shm_live(struct ps5_shm_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* PS5PLATFORM_SHM_H */
