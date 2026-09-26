/*
 * PS5 Platform - the machine context as the console lays it out.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console's ucontext_t has 48 bytes between uc_sigmask and uc_mcontext
 * that FreeBSD's does not, so every register a signal handler reads lies six
 * 64-bit words later than the payload SDK's header says (docs/PROBE.md).
 * This SDK's sys/_ucontext.h carries the difference; with it,
 * uc->uc_mcontext.mc_rip is the faulting instruction and no consumer carries
 * an offset of its own. Including this header refuses to build against an
 * upstream SDK, whose header lacks it.
 */
#ifndef PS5PLATFORM_CONTEXT_H
#define PS5PLATFORM_CONTEXT_H

#if defined(__PROSPERO__)
#include <stddef.h>
#include <ucontext.h>

#ifdef __cplusplus
static_assert(offsetof(ucontext_t, uc_mcontext) == 64,
              "this is not the PS5 fork of the payload SDK: uc_mcontext is misplaced");
#else
_Static_assert(offsetof(ucontext_t, uc_mcontext) == 64,
               "this is not the PS5 fork of the payload SDK: uc_mcontext is misplaced");
#endif
#endif

#endif /* PS5PLATFORM_CONTEXT_H */
