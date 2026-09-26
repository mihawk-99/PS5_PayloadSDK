/*
 * PS5 Platform - the exported kernel functions this layer and its consumers call.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The payload SDK links every one of these (its libkernel stubs name them) but
 * its headers declare none, so each project used to declare its own. Each
 * function here is exported by the console's libkernel_web, the libkernel this
 * title family imports (../PS5_RetroArch/docs/PLATFORM_FIRMWARE_ANALYSIS.md records the
 * comparison), and the behaviour the comments state is what our own console
 * probe measured (src/probe.c, evidence/), not what any other source says.
 *
 * Signatures follow the public PS4 SDK's, which the console keeps.
 */
#ifndef PS5PLATFORM_KERNEL_H
#define PS5PLATFORM_KERNEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protection bits for sceKernelMapDirectMemory and sceKernelMprotect. */
#define PS5_KERNEL_PROT_CPU_READ 0x01
#define PS5_KERNEL_PROT_CPU_WRITE 0x02
#define PS5_KERNEL_PROT_CPU_EXEC 0x04
#define PS5_KERNEL_PROT_GPU_READ 0x10
#define PS5_KERNEL_PROT_GPU_WRITE 0x20

/* The fixed-address mapping flag (FreeBSD's MAP_FIXED value). */
#define PS5_KERNEL_MAP_FIXED 0x10

/* The direct-memory type the title's CPU-only allocations use. */
#define PS5_KERNEL_DIRECT_TYPE_CPU 12

/* Kernel pages are 16 KiB; direct memory is allocated in 64 KiB units. */
#define PS5_KERNEL_PAGE_SIZE ((size_t)0x4000)
#define PS5_KERNEL_DIRECT_ALIGNMENT ((size_t)0x10000)

/* Direct memory: physical allocations mapped at any number of addresses. */
int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                      size_t alignment, int memory_type, int64_t *physical_start);
int32_t sceKernelReleaseDirectMemory(int64_t start, size_t length);
int32_t sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                                 int64_t direct_start, size_t alignment);
int64_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end, size_t alignment,
                                           int64_t *physical_start, size_t *available);

/* Virtual memory. */
int32_t sceKernelMprotect(const void *address, size_t length, int protection);
int32_t sceKernelReserveVirtualRange(void **address, size_t length, int flags, size_t alignment);
int32_t sceKernelMunmap(void *address, size_t length);
int32_t sceKernelQueryMemoryProtection(void *address, void **start, void **end, uint32_t *protection);

/* Flexible memory: the title's budget for anonymous mappings and libc. */
int32_t sceKernelAvailableFlexibleMemorySize(size_t *available);
int32_t sceKernelConfiguredFlexibleMemorySize(size_t *configured);

/* The shared-memory JIT interface. */
int32_t sceKernelJitCreateSharedMemory(const char *name, size_t length, int max_protection, int *fd);
int32_t sceKernelJitCreateAliasOfSharedMemory(int fd, int max_protection, int *alias_fd);
int32_t sceKernelJitMapSharedMemory(int fd, int protection, void **address);

/* Time and identity. */
uint64_t sceKernelReadTsc(void);
uint64_t sceKernelGetTscFrequency(void);

/* The public PS4 SDK's version record: its size is set by the caller. */
struct ps5_kernel_sw_version {
   size_t size;
   char text[0x1c];
   uint32_t version;
};
int32_t sceKernelGetSystemSwVersion(struct ps5_kernel_sw_version *version);

#ifdef __cplusplus
}
#endif

#endif /* PS5PLATFORM_KERNEL_H */
