/*
 * PS5 Platform - the exported VideoOut functions a title presents with.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The payload SDK links libSceVideoOut (its stubs name every export) but its
 * headers declare none, so ps5vk (PS5_Vulkan/driver), PS5_RetroArch's display
 * and the demo renderers each declared their own. These are the functions a
 * Vulkan swapchain on the console's one display needs, in the form those
 * projects' console runs used; the comments state what those runs measured,
 * not what any other source says. The arguments named reserved were 0 in
 * every run.
 */
#ifndef PS5PLATFORM_VIDEOOUT_H
#define PS5PLATFORM_VIDEOOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sceVideoOutOpen's user, bus and index for the title's own display. */
#define PS5_VIDEO_OUT_USER_SYSTEM 0xff

/* sceVideoOutSetBufferAttribute2's pixel format for 8-bit SDR buffers the
 * display scans out as bytes B, G, R, A (PS5_Vulkan docs/HARDWARE_FINDINGS.md). */
#define PS5_VIDEO_OUT_PIXEL_FORMAT_B8G8R8A8_SDR UINT64_C(0x8000000000000000)

/* Tiling 0: 64 KiB tiles of 128x128 four-byte pixels, the colour block's
 * SW_64KB_R_X with no pipe or bank swizzle. ps5vk renders into them with that
 * mode, and the demo renderer writes the same tiles from the CPU
 * (PS5_Vulkan src/demo_renderer.cpp, tiled_byte_offset). */
#define PS5_VIDEO_OUT_TILING_64KB_R_X 0u

/* The opaque attribute sceVideoOutSetBufferAttribute2 fills, zeroed first. */
#define PS5_VIDEO_OUT_ATTRIBUTE_BYTES 80

/* One registered buffer: its first byte, which the CPU and the GPU share. */
struct ps5_video_out_buffer {
   void *data;
   void *metadata;
   void *reserved[2];
};

/* sceVideoOutGetFlipStatus fills 16 64-bit words. Word 3 holds the argument
 * of the latest flip shown (a GPU flip's marker in ps5vk's runs). */
#define PS5_VIDEO_OUT_FLIP_STATUS_WORDS 16
#define PS5_VIDEO_OUT_FLIP_STATUS_SHOWN_ARGUMENT 3

/* sceVideoOutSubmitFlip's mode for a flip at the next vblank. Mode 0 was
 * refused with 0x80290006 (PS5_RetroArch docs/FINDINGS.md). */
#define PS5_VIDEO_OUT_FLIP_VSYNC 1

/* VideoOut's busy result, which sceVideoOutUnregisterBuffers returned at
 * shutdown while a flip was still pending; ps5vk, ProsperoLight and
 * ps5-opengl carry on. */
#define PS5_VIDEO_OUT_ERROR_BUSY ((int)0x80290009)

/* The output-mode selectors: 15 halves the measured vblank period to
 * 8.3416 ms (119.88 Hz) where the title declares high-frame-rate support and
 * the display takes it, and 1 restores 16.6834 ms (PS5_Vulkan, evidence
 * m6-output-mode-120hz). Without the declaration the console refused 15
 * (0x80290016). */
#define PS5_VIDEO_OUT_MODE_HIGH_FRAME_RATE 15u
#define PS5_VIDEO_OUT_MODE_RESTORE 1u

int sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter);
int sceVideoOutClose(int32_t handle);
/* Rate 0: a flip may show at every vblank. */
int sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
void sceVideoOutSetBufferAttribute2(void *attribute, uint64_t pixel_format, uint32_t tiling,
                                    uint32_t width, uint32_t height, uint64_t reserved1,
                                    uint32_t reserved2, uint64_t reserved3);
/* Registers count buffers of one attribute as indices start to start + count - 1
 * of set set; returns 0. ps5vk registered five 32 MiB buffers, 2 MiB aligned. */
int sceVideoOutRegisterBuffers2(int32_t handle, int32_t set, int32_t start,
                                struct ps5_video_out_buffer *buffers, int32_t count,
                                void *attribute, int32_t reserved, void *option);
int sceVideoOutUnregisterBuffers(int32_t handle, int32_t set);
/* Queues a flip to a registered buffer; the argument is reported back in the
 * flip status once the flip shows. A CPU flip shows what the GPU sees of the
 * buffer at scan-out, so CPU writes must have been written back first
 * (PS5_RetroArch docs/FINDINGS.md). */
int sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index, uint32_t mode, int64_t argument);
int sceVideoOutGetFlipStatus(int32_t handle, uint64_t status[PS5_VIDEO_OUT_FLIP_STATUS_WORDS]);
/* Whether a queued flip has not shown yet: positive while one is pending. */
int sceVideoOutIsFlipPending(int32_t handle);
int sceVideoOutWaitVblank(int32_t handle);
/* Positive when the output supports the mode. */
int sceVideoOutIsOutputSupported(int32_t handle, uint32_t mode, const void *reserved1,
                                 const void *reserved2, const void *reserved3);
int sceVideoOutConfigureOutput(int32_t handle, uint32_t mode, const void *reserved1,
                               const void *reserved2, const void *reserved3);

#ifdef __cplusplus
}
#endif

#endif /* PS5PLATFORM_VIDEOOUT_H */
