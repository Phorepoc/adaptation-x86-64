#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

# build_atomics_test.sh -- IN-CONTAINER, hosted: build + run atomics_test.cpp against the REAL
# x64s_atomics.cpp (unmodified; only <e32def.h> is the shim in atomics_test_shim/), at -O2 and
# -O0. Semantics test, not a concurrency test -- the SMP proof is the kernel itself.
#   host:  docker.exe run --rm -v <repo>:/repo -v <repo>/.phorepoc-x86-build:/out phorepoc-x86 \
#              bash /repo/tools/phorepoc-x86/port/x64smp/build_atomics_test.sh
# Bounded output; each run ends in `ATOMICS OK` or `ATOMICS FAIL n`.
set -u
cd /repo/tools/phorepoc-x86/port/x64smp
OUT=/out/atomics-test; mkdir -p "$OUT"
rc=0
for opt in -O2 -O0; do
  echo "=== atomics_test $opt"
  g++ $opt -std=gnu++03 -w -fno-exceptions -fno-rtti -I atomics_test_shim \
      -c x64s_atomics.cpp -o "$OUT/x64s_atomics$opt.o" \
    && g++ $opt -w -std=gnu++03 -I atomics_test_shim \
      -c atomics_test.cpp -o "$OUT/atomics_test$opt.o" \
    && g++ -o "$OUT/atomics_test$opt" "$OUT/atomics_test$opt.o" "$OUT/x64s_atomics$opt.o" \
    || { echo "ATOMICS FAIL build $opt"; rc=1; continue; }
  "$OUT/atomics_test$opt" 2>&1 | head -40
  [ "${PIPESTATUS[0]}" = 0 ] || rc=1
done
exit $rc
