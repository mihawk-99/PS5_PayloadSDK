# PS5_PayloadSDK

My fork of [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk), based on
its v0.42 release. It adds one thing the upstream SDK does not have: a **platform
layer** (`platform/`) that holds, in one place, what every one of my PS5
homebrew projects needs from the console and cannot take from the SDK as
released. [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) (the RADV
port and ps5vk), [PS5_RetroArch](https://github.com/mihawk-99/PS5_RetroArch),
[PS5_vkQuake](https://github.com/mihawk-99/PS5_vkQuake) and their titles each
pin one revision of this fork.

Everything below [the upstream SDK](#the-upstream-sdk) is upstream's own
documentation and applies unchanged.

## The platform layer

`platform/` builds `libps5platform.a` and installs it with its headers
(`include/ps5platform/`) as part of the SDK. What it provides, and what it
states about the console, rests on runs on the console
([PROBE.md](platform/docs/PROBE.md)):

| Header | What it gives a title |
| --- | --- |
| `kernel.h` | The exported kernel functions the layer and its consumers call, declared once. |
| `libc.h` | The libc functions the console lacks, refuses or faults in: `access` (refused to a title for every path), `statvfs`, `getpwuid_r`, `posix_fallocate`, `utimensat`/`futimens`, `clock_nanosleep`, `getaddrinfo`, the directory and `*at` families, `memfd_create`, `open_memstream`, `nl_langinfo`, FreeBSD's `xlocale` family, `regex` (musl's TRE), `backtrace`, `dladdr`, `__cxa_thread_atexit_impl`, and more. |
| `heap.h` | A heap in direct memory for the title's own allocations, since libc's private heap runs out long before the title does: dlmalloc mspaces, one for each allocating thread up to eight, so threads compiling shaders at once do not wait on each other. Titles take it through `--wrap=malloc` and its family, or call `ps5_heap_malloc` and the rest beside an allocator of their own. |
| `exec.h`, `shm.h` | Executable code in direct memory (for JITs), and shared-memory objects with several views, with virtual-range reservations. |
| `fp.h`, `context.h` | The floating-point state threads start with, and the machine context as the console lays it out (`uc_mcontext.mc_rip` is the faulting instruction). |
| `klog.h` | A title's standard error in the kernel log. |
| `agc.h`, `videoout.h` | The exported AGC and VideoOut functions the GPU drivers call. |
| `probe.h` | The console capability probe behind [PROBE.md](platform/docs/PROBE.md). |

Rules the layer keeps: it calls exported functions only, and what it states
about the console comes from its own probes, not from reading the system
software. `make -C platform test` runs its host unit tests against a host model
of the console's kernel.

## How projects consume it

A project pins a revision of this fork. Its dependency setup exports that
revision with `git archive` and runs `platform/tools/setup-sdk.sh` from the
export: the script downloads the upstream v0.42 release, checks its digest,
installs this revision's headers, compiler wrappers and platform layer over it,
and records the revision in the SDK directory's `.ps5-sdk-revision`. The
release's binaries (crt, libc, the stubs, libc++ and the host tools) are kept as
released: they are the binaries every project was validated with on the
console. More in [platform/README.md](platform/README.md).

Beside the platform layer, the fork changes the host compiler wrappers
(`host/bin/`), which assume IEEE denormals, and corrects the machine context in
the SDK's own `sys/_ucontext.h`.

## The upstream SDK

This is an SDK for developing payloads targeted at exploited PS5s. ELF loaders
known to work include:
- [ps5-payload-elfldr][elfldr]
- [ps5-payload-websrv][websrv]
- [bdj-ipv6-hen][bdj-ipv6-hen]
- [elfloader][elfloader] via [ps5-jar-loader]
- [remote_lua_loader][remote_lua_loader]

Several artifacts in this repository originate from the [PS5SDK][PS5SDK] project.

## Prerequisites
On Debian-flavored operating systems, you can invoke the following commands to
install dependencies used by the SDK.
```console
john@localhost:ps5-payload-dev/sdk$ sudo apt-get update && sudo apt-get upgrade # optional
john@localhost:ps5-payload-dev/sdk$ sudo apt-get install bash clang-18 lld-18 wget # required
john@localhost:ps5-payload-dev/sdk$ sudo apt-get install socat cmake meson pkg-config python3 python3-pyelftools # optional
```

If you are using Fedora, you can install dependencies as follows (tested with version 41):
```console
john@localhost:ps5-payload-dev/sdk$ sudo dnf install bash llvm-devel clang lld wget # required
john@localhost:ps5-payload-dev/sdk$ sudo dnf install socat cmake meson pkg-config python3 python3-pyelftools # optional
```

If you are using macOS, you can install them using the [Homebrew Package Manager][macos-brew] (tested with macOS Sequoia):
```console
john@localhost:ps5-payload-dev/sdk$ brew install llvm@18 wget # required
john@localhost:ps5-payload-dev/sdk$ export LLVM_CONFIG=/opt/homebrew/opt/llvm@18/bin/llvm-config # required
john@localhost:ps5-payload-dev/sdk$ brew install socat cmake meson python && pip3 install pyelftools # optional
```

## Quick-start
You can download a binary distribution of the SDK from [the latest release page][latest-rel],
then install it to your local storage, e.g,
```console
john@localhost:tmp$ wget https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip
john@localhost:tmp$ sudo unzip -d /opt ps5-payload-sdk.zip
```
Assuming you have all the prerequisites and you are on a POSIX system,
the binary distribution should work regardless of CPU architecture, e.g., x86_64, aarch64.

## Usage
```console
john@localhost:ps5-payload-dev/sdk$ export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
john@localhost:ps5-payload-dev/sdk$ make -C samples/hello_world
john@localhost:ps5-payload-dev/sdk$ export PS5_HOST=ps5; export PS5_PORT=9021
john@localhost:ps5-payload-dev/sdk$ make -C samples/hello_world test
```

## Building the SDK
```console
john@localhost:ps5-payload-dev/sdk$ sudo make DESTDIR=/opt/ps5-payload-sdk install
john@localhost:ps5-payload-dev/sdk$ export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
john@localhost:ps5-payload-dev/sdk$ sudo -E ./libcxx.sh # fetch, build, and install libcxx
```

## Adding new SCE Libs
If you have decrypted sprx files that you would like to interact with, you can
build stubs for them as follows (Make sure you have already installed the optional dependencies for your OS):
```console
john@localhost:ps5-payload-dev/sdk$ ln -s /path/to/sprx/libSceXYZ.sprx sce_stubs/libSceXYZ.sprx
john@localhost:ps5-payload-dev/sdk$ make -C sce_stubs stubs
john@localhost:ps5-payload-dev/sdk$ sudo make DESTDIR=/opt/ps5-payload-sdk install
```

## Reporting Bugs
If you encounter problems with the SDK, please [file a github issue][issues].
If you plan on sending pull requests which affect more than a few lines of code,
please file an issue before you start to work on you changes. This will allow us
to discuss the solution properly before you commit time and effort.

## License
Files in the folder include/freebsd are licenced under BSD licences.
Unless otherwhise explicitly stated inside a file, the rest are licensed under
the GPLv3+.

[issues]: https://github.com/ps5-payload-dev/sdk/issues/new
[latest-rel]: https://github.com/ps5-payload-dev/sdk/releases/latest
[elfldr]: https://github.com/ps5-payload-dev/elfldr
[websrv]: https://github.com/ps5-payload-dev/websrv
[bdj-ipv6-hen]: https://github.com/ps5-payload-dev/bdj-ipv6-hen
[remote_lua_loader]: https://github.com/shahrilnet/remote_lua_loader
[elfloader]: https://github.com/cryonumb/elfloader
[ps5-jar-loader]: https://github.com/hammer-83/ps5-jar-loader
[PS5SDK]: https://github.com/PS5Dev/PS5SDK
[macos-brew]: https://brew.sh
