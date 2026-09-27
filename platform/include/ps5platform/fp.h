/*
 * PS5 Platform - the floating-point environment.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A title starts with MXCSR 0x9fe0: flush-to-zero and denormals-are-zero on
 * (docs/PROBE.md, "Numbers and the floating-point state"), where Linux,
 * FreeBSD and Windows start at 0x1f80. Code written for those computes with
 * denormals and gets zeros instead: the Vulkan CTS's reference intervals for
 * double-precision builtins did.
 */
#ifndef PS5PLATFORM_FP_H
#define PS5PLATFORM_FP_H

#ifdef __cplusplus
extern "C" {
#endif

/* The IEEE state every other x86-64 system starts in: MXCSR 0x1f80 (round to
 * nearest, denormals kept, every exception masked) and the x87 control word
 * 0x37f. A title's startup code calls it before its constructors; threads
 * created through the platform's pthread_create start with their creator's
 * MXCSR (src/threads.c). */
void ps5_fp_ieee(void);

#ifdef __cplusplus
}
#endif

#endif /* PS5PLATFORM_FP_H */
