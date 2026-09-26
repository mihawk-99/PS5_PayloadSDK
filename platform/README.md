# PS5 Platform

One place for what every PS5 homebrew project of mine needs from the console
and the payload SDK does not give it: the kernel functions we call, declared
once; the machine context as the console lays it out; the libc functions the
console lacks or refuses; executable code in direct memory; and shared-memory
objects with several views, on direct memory, with virtual-range reservations.

It is consumed by PS5_RetroArch (and, through the title, its cores),
PS5_Vulkan, PS5_vkQuake and the templates, each of which pins a revision of
this repository and applies it in its own setup.

- docs/PROBE.md: what the console measured, with evidence/.
- src/probe.c: the capability probe that measured it.
