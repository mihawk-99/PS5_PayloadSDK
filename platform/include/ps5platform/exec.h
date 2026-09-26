/*
 * PS5 Platform - executable code in direct memory.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A JIT's code cache out of the direct-memory pool instead of the title's
 * small flexible budget. What it rests on was measured on the console
 * (docs/PROBE.md): direct memory mapped read-write and then given execute with
 * sceKernelMprotect runs, read-execute and read-write-execute alike; execute
 * asked for at map time is refused; a mapping with no address lands in the
 * Vulkan driver's GPU window, so every region here is placed; and a protection
 * change costs about 26 microseconds, so the default region is read, write and
 * execute at once and needs no change to be rewritten.
 */
#ifndef PS5PLATFORM_EXEC_H
#define PS5PLATFORM_EXEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Placement: one of these (or PS5_EXEC_AT, below), or none for anywhere
 * outside the GPU window. */
#define PS5_EXEC_NEAR 0x1u  /* within +/-2 GiB of request.anchor */
#define PS5_EXEC_FIXED 0x2u /* exactly at request.address */
/* A separate read-write view of the same memory: code is written through
 * write_view and runs from base, which is then read-execute only. */
#define PS5_EXEC_DUAL_VIEW 0x4u
/* Start read-write, not executable: the caller toggles with ps5_exec_protect
 * (write-xor-execute callers). */
#define PS5_EXEC_TOGGLED 0x8u
/* Exactly at request.address, and only if that range is free: nothing already
 * mapped there is replaced, unlike PS5_EXEC_FIXED. For a caller that tries
 * candidate addresses in turn (LRPS2's code area, placed beside its main
 * memory near the core's code); PS5_EXEC_NO_PLACE when the range is taken. */
#define PS5_EXEC_AT 0x10u

struct ps5_exec_request {
   size_t bytes;
   uintptr_t address; /* PS5_EXEC_FIXED or _AT: the address; otherwise a hint, or 0 */
   uintptr_t anchor;  /* PS5_EXEC_NEAR: the code that must reach this region */
   unsigned flags;
};

struct ps5_exec_region {
   void *base;       /* where the code runs */
   void *write_view; /* where it is written: base unless PS5_EXEC_DUAL_VIEW */
   size_t bytes;     /* rounded up to the 64 KiB direct-memory unit */
   int64_t direct_start;
   unsigned flags;
};

/* Allocates a region. Returns 0, or the kernel's result for the step that
 * failed, or PS5_EXEC_NO_PLACE when no address satisfies the placement; on
 * failure nothing is left allocated, mapped or reserved. */
#define PS5_EXEC_NO_PLACE ((int)0x80ff0001)
#define PS5_EXEC_BAD_REQUEST ((int)0x80ff0002)
int ps5_exec_alloc(const struct ps5_exec_request *request, struct ps5_exec_region *region);

/* PS5_EXEC_TOGGLED regions: [offset, offset + bytes) of base, rounded out to
 * 16 KiB pages, becomes read-write (writable) or read-execute. */
int ps5_exec_protect(const struct ps5_exec_region *region, size_t offset, size_t bytes, bool writable);

/* Unmaps and releases a region; a zeroed region is ignored. */
void ps5_exec_free(struct ps5_exec_region *region);

/* The regions live now, and their bytes. */
void ps5_exec_live(uint64_t *regions, uint64_t *bytes);

/* The form a JIT's own allocator has -- a size in, a pointer out, the pointer
 * back to free -- for the cores' AllocateExecutableMemory and the like. The
 * region is read, write and execute, within +/-2 GiB of the anchor when one is
 * given (the core's own code, so calls between the two use 32-bit
 * displacements), and anywhere outside the GPU window otherwise. The layer
 * keeps the region, so the caller holds only its address. NULL on failure. */
void *ps5_exec_allocate(size_t bytes, uintptr_t anchor);
/* Frees what ps5_exec_allocate returned. 0, or PS5_EXEC_BAD_REQUEST for an
 * address it did not return. */
int ps5_exec_release(void *base);

#ifdef __cplusplus
}
#endif

#endif /* PS5PLATFORM_EXEC_H */
