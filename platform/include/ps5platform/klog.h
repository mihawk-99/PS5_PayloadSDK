/*
 * PS5 Platform - a title's standard error in klog.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Nothing reads a title's standard error on the console, so a library's
 * messages there (the Vulkan driver's, a CTS's) are lost. After
 * ps5_klog_capture_stderr, what is written to standard error goes into a
 * pipe, and a thread writes each line that arrives on it to klog (libkernel's
 * sceKernelDebugOutText), with the prefix given. A title may not dup2 (the
 * console refuses it with EPERM), so there the stream stderr is moved to the
 * pipe rather than descriptor 2: everything written through stderr arrives,
 * a raw write() to descriptor 2 does not.
 */
#ifndef PS5PLATFORM_KLOG_H
#define PS5PLATFORM_KLOG_H

#ifdef __cplusplus
extern "C" {
#endif

int sceKernelDebugOutText(int channel, const char *text);

/* Returns 0, or -1 with errno when the pipe or the thread cannot be made
 * (standard error is then unchanged). Calling it again does nothing. */
int ps5_klog_capture_stderr(const char *prefix);

#ifdef __cplusplus
}
#endif

#endif /* PS5PLATFORM_KLOG_H */
