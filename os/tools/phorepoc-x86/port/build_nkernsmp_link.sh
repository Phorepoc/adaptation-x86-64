#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-1.0

# build_nkernsmp_link.sh -- IN-CONTAINER: SMP slice 2b step 2 LINK probe. Compile
# the arch-NEUTRAL nkernsmp core UNMODIFIED for amd64 AND our amd64 SMP backend
# (x64smp/x64s_*), then FULL-link to prove the backend symbol surface is CLOSED
# (zero unresolved). The SMP analogue of build_nkern_probe.sh's link self-check.
# Runs inside the phorepoc-x86 Docker image (repo at /repo).
set -eu
cd /repo
EKA=baseline/kernelhwsrv/kernel/eka
PORT=tools/phorepoc-x86/port
SMP=$PORT/x64smp
# x32 (long mode, 32-bit pointers) is THE Path C ABI (user decision 2026-09-20): EKA2's
# compile-time size/offset asserts stay ON. ABI=-m64 keeps the LP64 fallback building
# (needs -fpermissive + relax_size_asserts.h).
ABI="${ABI:--mx32}"
OUT="${NOUT:-/out/nkernsmp}"
if [ "$ABI" = "-mx32" ]; then PERMISSIVE=""; LDEMU="-m elf32_x86_64"; RELAX=""
else PERMISSIVE="-fpermissive"; LDEMU=""; RELAX="-include tools/phorepoc-x86/port/x64smp/relax_size_asserts.h"; fi
mkdir -p "$OUT"

# Neutral TUs: -masm=intel (baseline __crash() is intel asm, see build_nkernsmp_probe.sh).
NCXX="gcc $ABI -ffreestanding -fno-pic -fno-stack-protector -nostdinc \
      -std=gnu++03 -fno-exceptions -fno-rtti -fno-lifetime-dse -fno-strict-aliasing -fno-delete-null-pointer-checks -Wall -Wno-unused \
      -mno-red-zone -mgeneral-regs-only $PERMISSIVE -masm=intel"
# Backend TUs: default (AT&T) asm -- our inline asm is AT&T.
BCXX="gcc $ABI -ffreestanding -fno-pic -fno-stack-protector -nostdinc \
      -std=gnu++03 -fno-exceptions -fno-rtti -fno-lifetime-dse -fno-strict-aliasing -fno-delete-null-pointer-checks -Wall -Wno-unused \
      -mno-red-zone -mgeneral-regs-only $PERMISSIVE"

DEFS="-D__CPU_X86 -D__GCC32__ -D__EPOC32__ -D__SYMBIAN32__ -D__KERNEL_MODE__"
INCS="-I $SMP -I $EKA/include -I $EKA/include/nkernsmp -I $PORT"
PRE="-include $PORT/port_prelude.h -include e32def.h $RELAX"

NEUTRAL="nklib.cpp:nkern nkern.cpp:nkernsmp nkerns.cpp:nkernsmp sched.cpp:nkernsmp \
         dfcs.cpp:nkernsmp nk_timer.cpp:nkernsmp nk_irq.cpp:nkernsmp nk_bal.cpp:nkernsmp"

echo "== compile arch-neutral nkernsmp TUs =="
for spec in $NEUTRAL; do
    f="${spec%%:*}"; dir="${spec##*:}"
    printf "  %-14s " "$f"
    if $NCXX $DEFS $INCS $PRE -c "$EKA/$dir/$f" -o "$OUT/${f%.cpp}.o" 2>"$OUT/${f%.cpp}.log"; then
        echo "OK"
    else
        echo "FAIL"; grep -E "error:" "$OUT/${f%.cpp}.log" | sed 's/^/      /' | head -8; exit 1
    fi
done

echo "== compile amd64 SMP backend =="
for b in x64s_atomics x64s_ncutils x64s_ncsched; do
    printf "  %-14s " "$b.cpp"
    if $BCXX $DEFS $INCS $PRE -c "$SMP/$b.cpp" -o "$OUT/$b.o" 2>"$OUT/$b.log"; then
        echo "OK"
    else
        echo "FAIL"; grep -E "error:" "$OUT/$b.log" | sed 's/^/      /' | head -20; exit 1
    fi
done
printf "  %-14s " "x64_ctxsw.S"
gcc $ABI -c "$PORT/x64_ctxsw.S" -o "$OUT/x64_ctxsw.o" && echo "OK"

echo
echo "== FULL link (neutral core + amd64 SMP backend) -- check unresolved =="
BACKEND="$OUT/x64s_atomics.o $OUT/x64s_ncutils.o $OUT/x64s_ncsched.o $OUT/x64_ctxsw.o"
NEUTRALO="$OUT/nklib.o $OUT/nkern.o $OUT/nkerns.o $OUT/sched.o $OUT/dfcs.o \
          $OUT/nk_timer.o $OUT/nk_irq.o $OUT/nk_bal.o"
if ld $LDEMU -o "$OUT/nkernsmp.elf" -e _ZN10TScheduler10RescheduleEv \
      $NEUTRALO $BACKEND 2>"$OUT/link.log"; then
    echo "  LINK OK -- SMP symbol surface is CLOSED (zero unresolved)"
else
    echo "  UNRESOLVED symbols remain:"
    grep -E "undefined reference" "$OUT/link.log" | sed 's/.*undefined reference to //' \
        | sort -u | c++filt | sed 's/^/    /' | head -60
fi
