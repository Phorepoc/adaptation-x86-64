# adaptation-x86-64

This is the port of the Symbian kernel to 64-bit x86 processors. The 2010 release only ran on ARM phones and on an old
32-bit x86 emulator. We took that emulator's kernel code and made a sibling of it for modern CPUs.

It uses the x32 ABI: the processor runs in 64-bit mode, but pointers stay 32 bits wide. Symbian was written around
32-bit pointers, so this lets its code run unchanged at full 64-bit speed.

On its own this repository doesn't make a bootable system. A device repository builds on top of it, and for now that's
[adaptation-pc](https://github.com/Phorepoc/adaptation-pc), which covers QEMU, VirtualBox and real PCs.

## What's in here

```
os/tools/phorepoc-x86/port/    the kernel port, plus the user-side pieces in x64user/
os/tools/phorepoc-x86/ekern/   the scripts that build the kernel image and the bootable ISO
os/emitter/                    kernel build steps the ROM recipe runs
```

At build time `os/` is laid over [core](https://github.com/Phorepoc/core)'s `os/`, so the paths line up with core's.

## Building

You don't build this repository by itself. Check it out next to core, the [pdk](https://github.com/Phorepoc/pdk) and
adaptation-pc, and build the PC image as the pdk README describes. `-b pc` brings this repository in.

## License

Code we wrote is Apache-2.0. Symbian files we've changed are EPL-2.0, with GPL-2.0-or-later as a Secondary License, and
Symbian files we use unchanged stay EPL-1.0. The license texts are in `LICENSES/`.

Contributions are welcome under the same terms. Add your name to `AUTHORS` in your first pull request.
