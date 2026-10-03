# The PS5 platform layer

This directory is the part of my fork of the payload SDK that the upstream SDK
does not have: one place for what every PS5 homebrew project of mine needs from
the console. It holds the kernel functions we call, declared once. It holds the
libc functions the console lacks or refuses. It lets a sandboxed title reach `/data` by asking the Lapy
daemon (`ps5platform/elevation.h`, `docs/ELEVATION.md`). It gives executable code a home in
direct memory, and it provides shared-memory objects with several views on
direct memory, with virtual-range reservations. And it gives a title's
allocations a heap in direct memory (`ps5platform/heap.h`), since libc's own
private heap runs out long before the title does: an arena for each allocating
thread, up to eight, usable through the `--wrap` flags (`src/heap_wrap.c`) or
directly beside a title's own allocator. The machine context, as the
console lays it out, is corrected in the SDK's own `sys/_ucontext.h`
(`include/freebsd`), so `uc->uc_mcontext.mc_rip` is the faulting instruction.

`make` builds `libps5platform.a`; `make install` puts it in `target/lib` and
its headers in `target/include/ps5platform`, as part of the SDK's own install.
`make test` runs the host unit tests.

## How projects consume it

PS5_RetroArch (and, through the title, its cores), PS5_Vulkan, PS5_vkQuake and
the templates each pin one revision of this fork. Their dependency setup
exports that revision with `git archive` and runs `platform/tools/setup-sdk.sh`
from the export. The script downloads the upstream v0.42 release, the one this
fork is based on, and checks it against its digest. It installs this revision's
headers and platform layer over the release, then records the revision in the
SDK directory's `.ps5-sdk-revision`.

The release's binaries (crt, libc, the stubs, libc++ and the host tools) are
kept as released rather than rebuilt here. Those are the binaries every
project has been validated with on the console. A rebuild with another
compiler is not byte-identical, and the fork changes none of them.

- `include/ps5platform/`: the headers.
- `src/`: the library, including the capability probe (`src/probe.c`).
  `src/regex/` is musl 1.2.5's regular-expression engine (TRE), under the MIT
  licence in `src/regex/COPYRIGHT.musl`; `src/regex.c` adapts it to the SDK's
  FreeBSD `<regex.h>`. `src/dlmalloc/` is dlmalloc 2.8.6 (MIT, `SOURCE`), the
  allocator under the title heap (`src/heap.c`).
- `tests/`: the host unit tests and the host model of the console's kernel.
- `docs/PROBE.md`: what the console measured, with `evidence/`.
