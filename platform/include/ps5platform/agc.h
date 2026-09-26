/*
 * PS5 Platform - the exported AGC functions the GPU drivers call.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The payload SDK ships no AGC declarations, so ps5vk (PS5_Vulkan/driver) and
 * the RADV port (PS5_Mesa, src/amd/vulkan/winsys/ps5) would each declare their
 * own. These are the functions libSceAgc and libSceAgcDriver export that a
 * driver submitting its own PM4 needs, in the form the console accepted in
 * PS5_Vulkan's runs; the comments state what those runs measured, not what any
 * other source says.
 */
#ifndef PS5PLATFORM_AGC_H
#define PS5PLATFORM_AGC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version ps5vk initialises AGC with. sceAgcInit returns 0 once per
 * process; called from code a title loaded into anonymous memory (a libretro
 * core) it failed where the same call from the title's own executable
 * succeeded, so a driver calls it from the executable before any submission. */
#define PS5_AGC_INIT_VERSION 8u

int32_t sceAgcInit(uint32_t version);

/* A graphics submission: the PM4 words, which the GPU reads from GPU-visible
 * direct memory after their CPU cache lines have been written back, and their
 * count. A nonzero flag byte was refused on some firmware; drivers submit with
 * it clear. */
struct ps5_agc_submit_description {
   void *words;
   uint32_t word_count;
   uint8_t flag;
   uint8_t padding[3];
};

int32_t sceAgcDriverSubmitDcb(struct ps5_agc_submit_description *description);

/* Passed after a submission, it starts the submission promptly; without it the
 * console started each submission up to a refresh late (PS5_Vulkan R68). */
int32_t sceAgcSuspendPoint(void);

/* The wait-until-safe packet for a VideoOut buffer, written at *up. */
uint32_t sceAgcDriverGetWaitRenderingPacketSizeInDwords(void);
uint32_t sceAgcDriverWaitUntilSafeForRendering(uint32_t **up, uint32_t words, uint32_t reserved,
                                               uint32_t video, int buffer_index);

/* A command buffer as the AGC packet helpers take it: the words run from
 * bottom to top and are written at up. */
struct ps5_agc_command_buffer {
   uint32_t *bottom;
   uint32_t *top;
   uint32_t *up;
   uint32_t *down;
   uintptr_t callback;
   void *user_data;
   uint32_t reserved_dwords;
   uint32_t padding;
};

/* The flip packet for a VideoOut buffer; returns the start of the packet. */
uint32_t *sceAgcDcbSetFlip(struct ps5_agc_command_buffer *buffer, uint32_t video,
                           int buffer_index, uint32_t mode, int64_t marker);

#ifdef __cplusplus
}
#endif

#endif /* PS5PLATFORM_AGC_H */
