/*
 * PS5 Platform - the floating-point environment (include/ps5platform/fp.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ps5platform/fp.h"

void
ps5_fp_ieee(void)
{
   const unsigned mxcsr = 0x1f80;
   const unsigned short control = 0x37f;
   __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
   __asm__ volatile("fldcw %0" : : "m"(control));
}
