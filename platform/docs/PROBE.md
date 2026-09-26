# What the console measured

The capability probe (src/probe.c) establishes, on my console and in the
RetroArch title's own process, the memory behaviour this layer builds on. It
runs as a test aid of ../PS5_RetroArch: a test run uploads
`/app0/platform-probe.txt` (its words select the optional tests, `large` and
`jit`), the title runs the probe before anything else starts, writes every
result to its trace as one `platform-probe:` line, and ends the launch. I chose
the title over the Vulkan runner because the cores live in that process: its
flexible-memory budget, its `libkernel_web` imports, the driver's GPU window
and its core loader are the constraints the answers have to hold under.

Evidence: evidence/probe-2026-09-25 (the three runs' probe lines, and check.py,
which asserts every number quoted here). Console: system software 12.09
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
  offset: 48 bytes lie between `uc_sigmask` and the registers.
- **Reuse**: 200 cycles of reserve, allocate, map at a fixed address, run,
  unmap and release leave direct and flexible memory exactly where they were.
- **4 GiB of guest memory beside 1 GiB of code**: both mapped, every guest
  page and all 1,024 functions correct, flexible memory unchanged, and direct
  memory back to its starting size afterwards.

## The shared-memory JIT interface

`sceKernelJitCreateSharedMemory` is exported (under libkernel_web's JIT
libraries) and resolves, but returns 0x80020001 to the title: the interface is
not granted to it. Executable code goes through direct memory.
