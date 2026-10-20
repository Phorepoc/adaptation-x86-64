# adaptation-x86-64

The x86-64 platform adaptation of Phorepoc: the EKA2 kernel's `x64` port (a sibling of the 2010 `nkern/x86`, built for
the x32 ABI: amd64 CPU, 32-bit pointers) and the user-side port pieces every x86 build needs.

Device adaptations build on it: [adaptation-pc](https://github.com/Phorepoc/adaptation-pc) (QEMU, VirtualBox, real
PCs).

## Layout

```
os/tools/phorepoc-x86/port/    the x64 nanokernel / user port (x64user/ = the user-side ABI pieces)
os/tools/phorepoc-x86/ekern/   the kernel image build
os/emitter/                    kernel-side build steps the ROM recipe runs
```

`os/` is laid over [core](https://github.com/Phorepoc/core)'s `os/` at build time, so paths match core's.

## Building

With the [pdk](https://github.com/Phorepoc/pdk), name this checkout (and the device adaptation) in
`PHOREPOC_ADAPTATIONS`:

```sh
export PHOREPOC_ADAPTATIONS=../adaptation-x86-64:../adaptation-pc
./ph build ...
```

## Licence

Eclipse Public License 1.0 (see `LICENSE`), copyright The Phorepoc Authors (`AUTHORS`, `NOTICE`).

Contributions are welcome under the same terms: add yourself to `AUTHORS`.
