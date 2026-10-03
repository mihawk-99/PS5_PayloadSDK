# /data for a sandboxed title

A title starts able to see only its own sandbox: `/app0` (its folder) and `/download0`. Everything
my homebrew titles keep on `/data` (Proton's prefixes and games, a core's saves) is out of its
reach until something makes the process privileged enough. `ps5platform/elevation.h` is the
title's half of that: a small client for the [PS5-Lapy-JB-Daemon](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon)
(mpereiraesaa; credit for the owned-root design, the daemon, the donor transaction and the
cooperative protocol belongs to that project, to Arksama / Team PHU and to its contributors).
The client is derived from the one `ps5-native-app-boilerplate` adopted in its pull request 4
(Copyright (C) 2026 BlackBearReloaded, GPL-3.0-or-later), ported to C for this layer. This
repository contains no kernel code and no helper ELF, and never will: the earlier helper, which
published root-vnode pointers itself, and the copy of Lapy's transaction that was built into each
app, are gone, and there is no fallback to either.

## The title's side

```c
#include <ps5platform/elevation.h>

enum ps5_elevation_status status = ps5_elevation_request(PS5_ELEVATION_FILESYSTEM);
if (status != PS5_ELEVATION_OK) {
   /* /data is not usable. Say why (ps5_elevation_status_name) and go on without it, or stop;
    * never fall back on another way of elevating. */
}
```

Call it once, in single-threaded startup, before any worker thread exists. It does what Lapy's
contract asks, in this order:

1. It opens `/download0/lapy_owned_result` for writing now, because that path may not be visible
   after the root changes.
2. It calls `seteuid(geteuid())`, which gives the process the private credential shape Lapy
   needs.
3. It writes `{"PID":<pid>}` to `/download0/.elevate_proc.<pid>` and renames that over
   `/download0/elevate_proc`, so the daemon never sees a half-written request.
4. It polls, every 50 ms for at most ten seconds, by creating, writing, seeking, reading,
   comparing and removing `/data/.lapy_probe_<pid>`.
5. It writes `DATA_OK=<0|1> OPEN_ERRNO=<n>` through the descriptor opened in step 1.
6. It returns `PS5_ELEVATION_OK` only when the probe passed.

The request disappearing is not taken as success: it shows the daemon read the request, and the
path may look different after the root transition. The real `/data` round trip is the proof. A
missing daemon is `PS5_ELEVATION_TIMEOUT`, and nothing privileged may follow. `downloadDataSize`
must be positive in the title's `sce_sys/param.json`. If Lapy reports `daemon_held`, reboot the
console before closing or retrying the title.

`ps5_elevation_request_with` takes the contract's paths and limits as a structure; a host test
has no `/download0` or `/data`, and uses it with a model of the daemon (`tests/test_elevation.c`).
A host test establishes only this client's side of the contract: nothing about the kernel or the
firmware.

## The daemon

I run one upstream Lapy owned-root daemon, built from a pinned upstream commit, separately from
every title; titles never carry its source or its binary. Its two modes work with the same client:

* **one-shot**: built with `tools/build_owned_daemon.py --title <TITLE_ID> --require-client-result`
  and sent to elfldr before the title is launched. One invocation serves one request, checks the
  client's result, waits for the title to exit and checks that the root counter returned to its
  baseline. It is the mode to qualify a firmware with.
* **resident service**: built with `--service` and sent to elfldr once per boot. It watches every
  `PPSA*` sandbox and serves each title that asks. Exactly one daemon runs.

Upstream documents completed console validation on firmware 12.02. Other firmware is
experimental even when its layout checks pass, and a daemon that rejects a target is never worked
around: it is fixed upstream. The boilerplate's own record of a 50-cycle run on 6.02 (every
cycle functionally correct, but donor processes faulted in user mode, which fails upstream's strict
criterion) is why elevation is not switched on by default for other firmware.

## Moving a title to it

1. Delete the helper ELF (`sandbox-elevator.elf`) and the client, protocol and build script that
   went with it.
2. Link `libps5platform.a` and call `ps5_elevation_request` once at startup.
3. Run the daemon (one-shot or resident) before launching.
4. Record the Lapy commit, the payload SDK revision, the loader, the firmware and the daemon's
   ELF hash with each release.
