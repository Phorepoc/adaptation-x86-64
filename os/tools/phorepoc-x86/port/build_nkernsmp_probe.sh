#!/usr/bin/env bash
# build_nkernsmp_probe.sh -- IN-CONTAINER: SMP slice 2b probe. Try to compile the
# arch-NEUTRAL nkernsmp source (the SMP nanokernel: per-CPU TSubScheduler, SMP
# kernel lock, IPI reschedule) UNMODIFIED for amd64, against our amd64 port prelude.
# Mirrors build_nkern_probe.sh but for the nkernsmp/ tree + include/nkernsmp. Goal:
# learn how far the neutral core compiles and enumerate the backend symbol surface
# an amd64 SMP backend must satisfy (the nkernsmp/x86 backend is 32-bit, reference
# only). Runs inside the phorepoc-x86 Docker image (repo at /repo).
set -eu
cd /repo
EKA=baseline/kernelhwsrv/kernel/eka
PORT=tools/phorepoc-x86/port
OUT=/out/nkernsmp
mkdir -p "$OUT"

# -masm=intel: the baseline x86 __crash() (cpudefs.h) is asm("int 0xff") in Intel
# syntax (no AT&T '$'); cpudefs.h is pulled in by the TU's own chain (after our
# force-includes) so it can't be overridden -- assemble these TUs Intel-style. Our
# own arch header (nk_plat.h) avoids AT&T inline asm (barriers use __sync_synchronize).
CXX="gcc -m64 -ffreestanding -fno-pic -fno-stack-protector -nostdinc \
     -std=gnu++03 -fno-exceptions -fno-rtti -Wall -Wno-unused \
     -mno-red-zone -mgeneral-regs-only -fpermissive -masm=intel"

DEFS="-D__CPU_X86 -D__GCC32__ -D__EPOC32__ -D__SYMBIAN32__ -D__KERNEL_MODE__"
# nkernsmp has its OWN include root (include/nkernsmp). Our amd64 SMP arch headers
# (nk_plat.h, ncern.h) live in $PORT/x64smp and MUST be found before the baseline
# 32-bit x86 ones -- so x64smp goes first on the include path.
INCS="-I $PORT/x64smp -I $EKA/include -I $EKA/include/nkernsmp -I $PORT"
# port_prelude + e32def as usual, then relax_size_asserts.h LAST so it overrides
# e32def's __ASSERT_COMPILE (the 32-bit TSubScheduler/TScheduler size targets can't
# hold on amd64 -- see that header). Probe-only: it enumerates the backend surface.
PRE="-include $PORT/port_prelude.h -include e32def.h -include $PORT/x64smp/relax_size_asserts.h"

# The arch-neutral nkernsmp TUs (from nkern.mmp), in intended build order.
NEUTRAL="nklib.cpp:nkern nkern.cpp:nkernsmp nkerns.cpp:nkernsmp sched.cpp:nkernsmp \
         dfcs.cpp:nkernsmp nk_timer.cpp:nkernsmp nk_irq.cpp:nkernsmp nk_bal.cpp:nkernsmp"

echo "== compile arch-neutral nkernsmp TUs for amd64 (unmodified baseline source) =="
ok=0; fail=0
for spec in $NEUTRAL; do
    f="${spec%%:*}"; dir="${spec##*:}"
    printf "  %-14s " "$f"
    if $CXX $DEFS $INCS $PRE -c "$EKA/$dir/$f" -o "$OUT/${f%.cpp}.o" 2>"$OUT/${f%.cpp}.log"; then
        echo "OK"; ok=$((ok+1))
    else
        echo "FAIL"; grep -E "error:" "$OUT/${f%.cpp}.log" | sed 's/^/      /' | head -8; fail=$((fail+1))
    fi
done
echo "  -> $ok OK, $fail FAIL"

echo
echo "== backend symbol surface: undefined refs from the neutral TUs =="
# Relocatable link of whatever compiled, then list undefined symbols an amd64 SMP
# backend (ncsched/ncthrd/nccpu/ncirq/nctimer/atomics + entry) must supply.
if ls "$OUT"/*.o >/dev/null 2>&1; then
    ld -r -o "$OUT/neutral.o" "$OUT"/*.o 2>/dev/null || true
    nm -u "$OUT/neutral.o" 2>/dev/null | awk '{print $2}' | c++filt | sort -u | sed 's/^/    /' | head -80
fi
