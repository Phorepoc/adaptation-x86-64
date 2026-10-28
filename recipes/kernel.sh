#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

# recipes/kernel.sh -- build the x86-64 (x32) kernel and the bootable GRUB ISO that carries the ROM
# "ph image rom" made. Runs on Linux inside the toolchain container ("ph image recipe kernel -b <board>" does that).
#
#   1. the kernel include shims: the headers the release's build exports to a relocated path (heap_hybrid.h, hal_int.h)
#      or generates (exec_*.h, by the release's own genexec.pl over execs.txt) -> $BUILD/x86/kinc
#   2. the kernel scripts of the x86-64 adaptation (tools/phorepoc-x86/ekern/make_ekern.sh, SECONDARY=rom): nkernSMP,
#      ekern and the flexible memory model from the Symbian source, our amd64 backend, boot stub, link, GRUB ISO
#
# Paths: PHOREPOC_OS_ROOT / PHOREPOC_ADAPTATIONS (the OS tree, merged), PHOREPOC_SYMBIAN (the source),
# PHOREPOC_BUILD (all output: the ROM from $BUILD/rom, the kernel and ISO in $BUILD/x86/ekern).
# Settings of the kernel build (make_ekern.sh): DBG=0|1 (default 1), SCREEN=WxH (default 360x640), SCALE=n, TRACE=<hex>, KCLEAN=1.
set -eu
PDK="${PHOREPOC_PDK:?run it with ph image recipe kernel -b <board>}"
export PHOREPOC_CORE="${PHOREPOC_CORE:-$PDK/../core}"
export PHOREPOC_OS_ROOT="${PHOREPOC_OS_ROOT:-$PHOREPOC_CORE/os}"
export PHOREPOC_SYMBIAN="${PHOREPOC_SYMBIAN:-$PHOREPOC_CORE/symbian}"
export PHOREPOC_BUILD="${PHOREPOC_BUILD:-$PHOREPOC_CORE/build}"
OS_ROOT="$PHOREPOC_OS_ROOT"
[ -z "${PHOREPOC_ADAPTATIONS:-}" ] || OS_ROOT="$(python3 "$PDK/tools/emitter/paths.py")"
SYM10="$PHOREPOC_SYMBIAN/symbian-2010"
KHS="$SYM10/oss.FCL.sf.os.kernelhwsrv"
KOUT="$PHOREPOC_BUILD/x86"
KROM="$PHOREPOC_BUILD/rom/romsect.bin"

[ -d "$KHS/kernel/eka" ] || { echo "no kernel source at $KHS (ph source fetch)" >&2; exit 1; }
[ -f "$OS_ROOT/tools/phorepoc-x86/ekern/make_ekern.sh" ] || { echo "no x86 kernel scripts in $OS_ROOT/tools/phorepoc-x86" >&2; exit 1; }
[ -f "$KROM" ] || { echo "no ROM at $KROM (ph image rom first)" >&2; exit 1; }
mkdir -p "$KOUT"

echo "[kinc] kernel include shims"
KINC="$KOUT/kinc"
rm -rf "$KINC"; mkdir -p "$KINC/kernel"
echo "#include \"$KHS/kernel/eka/include/heap_hybrid.h\"" > "$KINC/kernel/heap_hybrid.h"
echo "#include \"$KHS/halservices/hal/inc/hal_int.h\"" > "$KINC/kernel/hal_int.h"
# genexec.pl pipes its input through `cpp -I- -I. -lang-c++` (flags of a 2004 GCC): a stand-in maps them to g++
SHIM="$(mktemp -d)"; trap 'rm -rf "$SHIM"' EXIT
printf '#!/bin/sh\nargs=""\nfor a in "$@"; do case "$a" in -I-|-lang-c++) ;; *) args="$args $a";; esac; done\nexec g++ -E -x c++ $args\n' > "$SHIM/cpp"
chmod +x "$SHIM/cpp"
( cd "$KHS/kernel/eka/kernel" && PATH="$SHIM:$PATH" perl genexec.pl -i execs.txt \
    -e "$KINC/kernel/exec_enum.h" -u "$KINC/kernel/exec_user.h" -k "$KINC/kernel/exec_kernel.h" )

export KVIEW="$OS_ROOT" KOUT KHS KINC KROM
export KQEMU="$SYM10/oss.FCL.sf.adapt.Qemu"
export SECONDARY=rom
# a release image traces panics only (the default also prints every ROM page it maps); the phone screen, pixel-doubled
export TRACE="${TRACE:-0x80000000}" SCREEN="${SCREEN:-360x640}"
bash "$OS_ROOT/tools/phorepoc-x86/ekern/make_ekern.sh" > "$KOUT/kernel.log" 2>&1 \
  && echo "kernel ok: $(ls -l "$KOUT/ekern/phorepoc-ekern.iso" | awk '{print $5, $9}')" \
  || { echo "KERNEL FAILED:"; tail -15 "$KOUT/kernel.log"; exit 1; }
