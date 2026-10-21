#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

# build_sched_test.sh -- IN-CONTAINER: cooperative scheduling on the REAL EKA2
# nkern (amd64). Builds the neutral nkern + amd64 backend (via the probe), then
# compiles sched_test.cpp against the same headers and links it hosted so it RUNS
# (cooperative yields need no timer/IDT; glibc runs the TheScheduler global ctor
# and resolves printf). Run inside the phorepoc-x86 image:
#   docker run --rm -v <repo>:/repo -v <build>:/out phorepoc-x86 \
#     bash /repo/tools/phorepoc-x86/port/build_sched_test.sh
set -eu
cd /repo
EKA=baseline/kernelhwsrv/kernel/eka
PORT=tools/phorepoc-x86/port
OUT=/out/nkern

# 1. Ensure the neutral nkern + backend objects exist.
bash "$PORT/build_nkern_probe.sh" >/dev/null

# 2. Compile the test against the EKA2 headers (freestanding, like the kernel).
CXX="gcc -m64 -ffreestanding -fno-pic -fno-stack-protector -nostdinc \
     -std=gnu++03 -fno-exceptions -fno-rtti -Wall -Wno-unused \
     -mno-red-zone -mgeneral-regs-only -fpermissive"
DEFS="-D__CPU_X86 -D__GCC32__ -D__EPOC32__ -D__SYMBIAN32__ -D__KERNEL_MODE__"
INCS="-I $EKA/include -I $EKA/include/nkern -I $PORT/x64 -I $PORT"
PRE="-include $PORT/port_prelude.h -include e32def.h"
$CXX $DEFS $INCS $PRE -c "$PORT/sched_test.cpp" -o "$OUT/sched_test.o"

# 3. Link HOSTED (plain gcc pulls glibc for printf + runs .init_array ctors).
gcc -m64 -no-pie -o /tmp/sched_test \
    "$OUT/sched_test.o" \
    "$OUT"/nklib.o "$OUT"/nk_timer.o "$OUT"/dfcs.o "$OUT"/sched.o \
    "$OUT"/nkern.o "$OUT"/nkerns.o \
    "$OUT"/x64_atomics.o "$OUT"/x64_ncutils.o "$OUT"/x64_ncsched.o "$OUT"/x64_ctxsw.o

# 4. Run.
/tmp/sched_test
