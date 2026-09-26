# What the console measured

The capability probe (src/probe.c) establishes, on my console and in the
RetroArch title's own process, the memory behaviour this layer builds on. It
runs as a test aid of ../PS5_RetroArch: a test run uploads
`/app0/platform-probe.txt` (its words select the optional tests, `large`,
`huge` and `jit`), the title runs the probe before anything else starts, writes every
result to its trace as one `platform-probe:` line, and ends the launch. I chose
the title over the Vulkan runner because the cores live in that process: its
flexible-memory budget, its `libkernel_web` imports, the driver's GPU window
and its core loader are the constraints the answers have to hold under.

Evidence: evidence/probe-2026-09-25 (three runs built with the upstream SDK
header) and evidence/probe-2026-09-25-huge (three runs built with this fork's
header: the refused 10 GiB reservation, the survey with 10 GiB mapped, and the
whole pool); each directory's check.py asserts every
number quoted here from its runs. Console: system software 12.09
(`sceKernelGetSystemSwVersion` reports 12.090.001).

## The pools

- **Direct memory: 12 GiB** (`sceKernelGetDirectMemorySize`), 11.94 GiB of it
  available when the title starts, and one allocation can take **11.87 GiB**.
- **Flexible memory: 448 MiB configured** (`sceKernelConfiguredFlexibleMemorySize`),
  403 MiB available when the title starts.

## Executable code in direct memory

- **Execute asked for at map time is refused**: `sceKernelMapDirectMemory` with
  read-execute or read-write-execute returns 0x80020016.
- **Map read-write, then add execute with `sceKernelMprotect`**: the code runs.
  Read-execute and **read-write-execute** are both granted, so code can be
  rewritten in place with no protection change at all.
- **A mapping asked for with no address lands in the GPU window**
  (0x2_0000_0000 - 0x2_FFFF_FFFF). Every mapping this layer makes carries an
  address.
- **Placement near an anchor**: a virtual range reserved at a hint is reserved
  there, and direct memory mapped at a fixed address inside it runs its code:
  five 512 MiB ranges within 2 GiB of the title's own code, outside the GPU
  window, all ran.
- **Rewriting**: 5,000 write/protect/execute cycles on one region and 6,400
  across 64 regions, every result right.
- **A protection change costs about 26 µs**, for one 16 KiB page and for 1 MiB
  alike. Toggling write and execute per rewrite is therefore expensive;
  read-write-execute, or two views of one allocation, avoids it.
- **Concurrent rewriting**: a page rewritten 3,000 times with per-page toggles
  while four threads ran its neighbour page (486 million calls), and code
  written through a read-write view while four threads ran it through a
  read-execute view of the same allocation (5,000 rewrites): no wrong result,
  no fault.
- **Faults**: a read of a reserved, unmapped address raises SIGSEGV with the
  right address; a handler that rewrites the faulting instruction through a
  writable view and returns runs the rewritten code. 200 sites, each faulted
  once, backpatched, and then run again without a fault; about 57 µs a fault.
- **The machine context**: with every general register loaded with a marker,
  each one sits exactly six 64-bit words later than the SDK's `mcontext_t`
  places it (rdi through r15 and rip), with `uc_mcontext` at the header's
  offset: 48 bytes lie between `uc_sigmask` and the registers. Built with
  this fork's `sys/_ucontext.h`, which declares those 48 bytes, the same test
  finds `uc_mcontext` at word 8 and rip at word 28, `mc_rip`'s own place: the
  header matches the console.
- **Reuse**: 200 cycles of reserve, allocate, map at a fixed address, run,
  unmap and release leave direct and flexible memory exactly where they were.
- **4 GiB of guest memory beside 1 GiB of code**: both mapped, every guest
  page and all 1,024 functions correct, flexible memory unchanged, and direct
  memory back to its starting size afterwards.

## More than 4 GiB at once

- **10 GiB in one allocation**: one 10 GiB direct allocation mapped read-write
  in one view, outside the GPU window. Every 64-bit word was written (0.68 s)
  and read back (1.01 s) with no error. The largest free direct block fell
  from 11.94 GiB to 1.94 GiB while it was held, and flexible memory did not
  change.
- **10 GiB in ten pieces**: ten 1 GiB allocations mapped at once, each
  wherever the kernel placed it near the hint, all outside the GPU window,
  every word right, flexible memory unchanged.
- **The whole pool**: all 11.94 GiB of free direct memory is one block. The
  largest allocation the 64 MiB search finds, 11.875 GiB, was mapped at
  0x10_0000_0000 and every word written (0.80 s) and read back (1.16 s) with
  no error. While it was held, 62 MiB of direct memory could still be
  allocated (the remainder below the search step) and flexible memory was
  unchanged at 403 MiB.
- Direct memory returned to its starting size after each.

So the CPU side is not limited to 4 GiB: a title can hold the whole direct
pool it is given, about 11.9 GiB, beside its 403 MiB of flexible memory. The
ceiling is the pool: 12 GiB, 66 MiB of which is already in use when the
probe starts. The 4 GiB limit that remains is the Vulkan driver's GPU
window, which is where GPU-visible memory has to live.

## Virtual space

What `sceKernelReserveVirtualRange` grants, measured with ranges of 1 to
16 GiB, each released at once:

- From 0x4_0000_0000 (the view area), and with no address, at most 15 GiB:
  something the kernel will not give up sits between 0x7_C400_0000 and
  0x8_0000_0000. At 0x6_0000_0000, at most 7 GiB.
- **A hint whose free space is too small is refused** (0x8002000c) rather
  than moved: the first 10 GiB run asked for 10 GiB at 0x6_0000_0000 and was
  refused (evidence/probe-2026-09-25-huge/reserve-refused.txt).
- A hint inside space already taken is moved upward: 16 GiB asked for at
  0x8_0000_0000 was placed at 0xF_E048_0000.
- **From 0x10_0000_0000 to 0x80_0000_0000, 16 GiB is reserved at the hint
  itself**, and nothing at or above 0x100_0000_0000 (1 TiB) is granted.

Ranges larger than the view area holds (guest-memory arenas above all) belong
at 0x10_0000_0000 and above.

## The shared-memory JIT interface

`sceKernelJitCreateSharedMemory` is exported (under libkernel_web's JIT
libraries) and resolves, but returns 0x80020001 to the title: the interface is
not granted to it. Executable code goes through direct memory.
