# Upstream contributions

One entry per problem. Update an entry instead of adding a duplicate; recheck live state before acting.

## Platform heap in a payload: direct memory from the application's pool

- **Upstream:** `ps5-payload-dev/sdk`, branch `master` (maintainer John Törnblom).
- **Problem:** in a payload (no direct memory budget of its own) `sceKernelAllocateDirectMemory` is refused, so the platform
  layer's heap, executable-memory allocator, shared memory and thread stacks failed there; the heap fell back to libc's private
  heap (`platform/src/heap.c`, `platform/src/placement.c`: `ps5p_allocate_direct`).
- **Owner and branch:** this fork's `main`, commit `c32d2be` (pushed to `mihawk-99/PS5_PayloadSDK`).
- **Searched:** 2026-10-24 22:33 UTC. The tree of `upstream/master` (`git ls-tree`, `git grep`): no `platform/` directory and no heap;
  `sceKernelAllocateDirectMemoryForApp` is already in `sce_stubs/libkernel.c` and `libkernel_sys.c`. `gh pr list --state all`
  (60 PRs) and `gh issue list --state all` filtered for heap, memory, alloc, direct: #72 (closed, "include: declare exported libkernel
  memory and thread APIs", a header addition that was declined) and #48 (merged, unrelated). Searches for "ForApp", "AllocateDirectMemory
  payload", "direct memory budget" returned nothing.
- **Disposition: outside upstream's scope, no PR.** The platform layer (`platform/`, `ps5platform/*`) is a local addition of this fork,
  absent from upstream; the behaviour lives in that layer, and upstream already ships the function the fix calls. A PR adding the
  platform layer would be a different, much larger proposal; none is planned. Not related to #72's declined header additions.
- **Evidence:** `make test` in `platform/`: 320 of 320 checks (two new, a fake kernel in payload mode); `make` builds the console library
  with `-Werror`. Hardware: PS5 Pro, firmware 10.01, a Wine helper's GPU client; browser helper resident memory across the start-up
  jump 588-664 to 247-278 MiB, libc heap 400 to 6 MiB in use (consumer repository's docs/PLAN.md, runs `fin1-2026-10-24-01` and `fin2-2026-10-24-01`).
- **Follow-up, same layer:** `9d04878` lowers the heap's granularity from 16 to 2 MiB (direct memory is committed whole when mapped); also
  local to `platform/`, so the same disposition applies.
- **Console proof of both commits (2026-10-24, PS5 Pro, firmware 10.01, consumer pinned to `9d04878`, its own import patch off):** Steam Big Picture
  peak system RSS 945-964 MiB before, 642 MiB after, libc heap 6 MiB in use; Isaac 60 fps, peak 673 MiB (672 MiB on libc's heap, 754 MiB with
  16 MiB segments).
