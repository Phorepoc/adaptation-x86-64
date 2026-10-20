#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-1.0

# build_realx_test.sh -- IN-CONTAINER, hosted: build + run realx_test.cpp against the REAL
# xu_realx.cpp (unmodified; only <e32math.h> is the class-layout shim in realx_test_shim/), at
# -O2 and -O0, 64-bit and (if the image has the x32 runtime) not at all for x32 -- the target
# build of the same file is covered by euser_census_x32.sh. Oracle = the host's x87 long double.
#   host:  docker.exe run --rm -v <repo>:/repo -v <repo>/.phorepoc-x86-build:/out phorepoc-x86 \
#              bash /repo/tools/phorepoc-x86/port/x64user/build_realx_test.sh
# Bounded output; each run ends in `REALX OK` or `REALX FAIL n`.
set -u
cd /repo/tools/phorepoc-x86/port/x64user
OUT=/out/realx-test; mkdir -p "$OUT"
rc=0
for opt in -O2 -O0; do
  echo "=== realx_test $opt"
  g++ $opt -std=gnu++03 -w -fno-exceptions -fno-rtti -I realx_test_shim -c xu_realx.cpp -o "$OUT/xu_realx$opt.o" \
    && g++ $opt -w -frounding-math -I realx_test_shim -c realx_test.cpp -o "$OUT/realx_test$opt.o" \
    && g++ -o "$OUT/realx_test$opt" "$OUT/realx_test$opt.o" "$OUT/xu_realx$opt.o" -lm \
    || { echo "REALX FAIL build $opt"; rc=1; continue; }
  "$OUT/realx_test$opt" 2>&1 | head -60
  [ "${PIPESTATUS[0]}" = 0 ] || rc=1
done
exit $rc
