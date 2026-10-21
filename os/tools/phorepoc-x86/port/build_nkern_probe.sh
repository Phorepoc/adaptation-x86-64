#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

# build_nkern_probe.sh -- IN-CONTAINER: compile the arch-NEUTRAL EKA2 nkern
# source UNMODIFIED for a freestanding x86_64 target, against our amd64 port
# prelude + arch backend. The real "portable kernel on amd64" work, growing one
# neutral TU at a time. Runs inside the phorepoc-x86 Docker image (repo at /repo).
set -eu
cd /repo
EKA=baseline/kernelhwsrv/kernel/eka
PORT=tools/phorepoc-x86/port
# ABI=-mx32 builds the same set under the x32 ABI (long mode, 32-bit pointers):
# EKA2's 32-bit type model is then simply true, so no -fpermissive is needed.
ABI="${ABI:--mx32}"
OUT="${NKOUT:-/out/nkern}"
if [ "$ABI" = "-mx32" ]; then PERMISSIVE=""; LDEMU="-m elf32_x86_64"; else PERMISSIVE="-fpermissive"; LDEMU=""; fi
mkdir -p "$OUT"

# amd64, freestanding, no host libc/headers; C++03 like the original.
CXX="gcc $ABI -ffreestanding -fno-pic -fno-stack-protector -nostdinc \
     -std=gnu++03 -fno-exceptions -fno-rtti -Wall -Wno-unused \
     -mno-red-zone -mgeneral-regs-only $PERMISSIVE"
# -fpermissive: EKA2 declares placement 'operator new(TUint=32bit, TAny*)', but the
# x86_64 C++ ABI wants size_t(64bit) first. Size is ignored for placement new, so this
# is safe. It flags EKA2's ILP32 assumptions -- a real amd64-port concern to track.

# Symbian include roots + arch/toolchain selection macros.
DEFS="-D__CPU_X86 -D__GCC32__ -D__EPOC32__ -D__SYMBIAN32__ -D__KERNEL_MODE__"
INCS="-I $EKA/include -I $EKA/include/nkern -I $PORT/x64 -I $PORT"
PRE="-include $PORT/port_prelude.h -include e32def.h"

# The arch-neutral nkern TUs, in intended build order. Grown as each compiles.
NEUTRAL="nklib.cpp nk_timer.cpp dfcs.cpp sched.cpp nkern.cpp nkerns.cpp"

echo "== compile arch-neutral nkern TUs for amd64 (unmodified baseline source) =="
for f in $NEUTRAL; do
    printf "  %-14s " "$f"
    if $CXX $DEFS $INCS $PRE -c "$EKA/nkern/$f" -o "$OUT/${f%.cpp}.o" 2>"$OUT/${f%.cpp}.log"; then
        echo "OK"
    else
        echo "FAIL"; sed 's/^/      /' "$OUT/${f%.cpp}.log" | grep -E "error:" | head; exit 1
    fi
done

echo "== compile amd64 arch backend =="
for b in x64_atomics x64_ncutils x64_ncsched; do
    printf "  %-14s " "$b.cpp"
    if $CXX $DEFS $INCS $PRE -c "$PORT/$b.cpp" -o "$OUT/$b.o" 2>"$OUT/$b.log"; then
        echo "OK"
    else
        echo "FAIL"; grep -E "error:" "$OUT/$b.log" | sed 's/^/      /' | head; exit 1
    fi
done
printf "  %-14s " "x64_ctxsw.S"
gcc $ABI -c "$PORT/x64_ctxsw.S" -o "$OUT/x64_ctxsw.o" && echo "OK"

echo
echo "== link the amd64 nanokernel (neutral core + amd64 backend), check for unresolved =="
# Relocatable link (-r): proves the symbol surface is CLOSED without needing a
# board/entry yet. Any remaining 'undefined' would print here.
BACKEND="$OUT/x64_atomics.o $OUT/x64_ncutils.o $OUT/x64_ncsched.o $OUT/x64_ctxsw.o"
ld $LDEMU -r -o "$OUT/nkern.o" "$OUT"/nklib.o "$OUT"/nk_timer.o "$OUT"/dfcs.o \
      "$OUT"/sched.o "$OUT"/nkern.o "$OUT"/nkerns.o $BACKEND 2>/dev/null || true
# The real test: a full link. Undefined symbols abort it and are listed.
if ld $LDEMU -o "$OUT/nkern.elf" -e _ZN10TScheduler10RescheduleEv \
      "$OUT"/nklib.o "$OUT"/nk_timer.o "$OUT"/dfcs.o "$OUT"/sched.o \
      "$OUT"/nkern.o "$OUT"/nkerns.o $BACKEND 2>"$OUT/link.log"; then
    echo "  LINK OK -- symbol surface is CLOSED (zero unresolved)"
else
    echo "  UNRESOLVED symbols remain:"
    grep -E "undefined reference" "$OUT/link.log" | sed 's/^/    /' | c++filt | sort -u | head -40
fi

echo
echo "== objects =="; ls -l "$OUT"/*.o
echo "== symbols in nklib.o (proof real EKA2 nkern code compiled for amd64) =="
nm "$OUT/nklib.o" | grep -iE ' T | W ' | head -20
