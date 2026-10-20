#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-1.0

# hal_census_x32.sh -- IN-CONTAINER: compile HAL (the user-side HAL client, hal.dll) for the
# Path C target (amd64 CPU, x32 ABI). Sibling of euser_census_x32.sh / efsrv_census_x32.sh.
#   LINK=1: keep objects + publish the compile line (flags.txt) for emitter/build_hal_x32.py.
#
# hal.dll is built the way the real baseport builds it (hal.mmp):
#   * the REAL halservices/hal/src/{hal_main,userhal}.cpp, unmodified
#   * two GENERATED tables, produced by the REAL generator on THIS board's config:
#       perl halcfg.pl    hal_data.h  values.hda -> hal_values.cpp   (attribute defaults)
#       perl halcfg.pl -x hal_data.h  config.hcf -> hal_config.cpp   (attribute -> handler)
# Only those two text files (tools/phorepoc-x86/hal/) are board-specific -- every handler they
# name lives in the generic userhal.cpp, so nothing else about HAL is architecture-dependent.
# EFILE imports HAL::Get (halu.def ordinal 1); sf_cache_man/sf_ldr/sf_utl ask it for
# EMemoryPageSize / ECPUSpeed / EPowerGood.
# docs/phorepoc/x86-64-port.md X4.11.
set -u
cd /repo
EKA=baseline/kernelhwsrv/kernel/eka
HAL=baseline/kernelhwsrv/halservices/hal
BOARD=tools/phorepoc-x86/hal
PORT=tools/phorepoc-x86/port
OUT=/out/hal-census; mkdir -p "$OUT"
LINK="${LINK:-0}"; OBJ="$OUT/obj"; [ "$LINK" = 1 ] && { rm -rf "$OBJ"; mkdir -p "$OBJ"; }

perl "$HAL/halcfg.pl"    "$HAL/inc/hal_data.h" "$BOARD/values.hda" "$OUT/hal_values.cpp" || exit 1
perl "$HAL/halcfg.pl" -x "$HAL/inc/hal_data.h" "$BOARD/config.hcf" "$OUT/hal_config.cpp" || exit 1
echo "generated: $(wc -l < "$OUT/hal_values.cpp") + $(wc -l < "$OUT/hal_config.cpp") lines"

CXX="gcc -mx32 -ffreestanding -fno-pic -fno-stack-protector -nostdinc -std=gnu++03 \
     -fno-exceptions -fno-rtti -fno-lifetime-dse -fstack-reuse=named_vars -fno-strict-aliasing -fno-delete-null-pointer-checks -w -masm=intel -O2 -fno-builtin -fno-tree-loop-distribute-patterns \
     -fshort-wchar"
DEFS="-D__CPU_X86 -D__GCC32__ -D__EPOC32__ -D__SYMBIAN32__ -D_UNICODE -D__DLL__ ${EXTRA_DEFS:-}"
INCS="-I toolchain/overlay -I $PORT/x64user -I $HAL/inc -I $EKA/include -I $EKA/euser -I $EKA/common \
      -I toolchain/kinc -I toolchain/generated -I $PORT"
PRE="-include $PORT/port_prelude.h -include e32def.h"
[ "$LINK" = 1 ] && echo "$CXX $DEFS $INCS $PRE" > "$OUT/flags.txt"

pass=0; fail=0; : > "$OUT/failed.txt"; : > "$OUT/errors.txt"
for src in "$HAL/src/hal_main.cpp" "$HAL/src/userhal.cpp" "$OUT/hal_values.cpp" "$OUT/hal_config.cpp"; do
  tu="$(basename "$src")"
  [ -f "$src" ] || { echo "MISSING $tu" >> "$OUT/failed.txt"; fail=$((fail+1)); continue; }
  o=/dev/null; [ "$LINK" = 1 ] && o="$OBJ/$(echo "$tu" | tr "." "_").o"
  if $CXX $DEFS $INCS $PRE -c "$src" -o "$o" 2>"$OUT/one.err"; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "$tu" >> "$OUT/failed.txt"
    { echo "=== $tu"; grep -m4 "error" "$OUT/one.err" | cut -c1-220; } >> "$OUT/errors.txt"
  fi
done
echo "=== hal census -mx32: $pass OK, $fail FAIL ==="
head -20 "$OUT/failed.txt"
echo "--- first errors (full list: $OUT/errors.txt)"; head -40 "$OUT/errors.txt"

if [ "$LINK" = 1 ]; then
  ld -m elf32_x86_64 -r -o "$OUT/hal.rel.o" "$OBJ"/*.o 2>"$OUT/ld.log" || head -5 "$OUT/ld.log"
  nm -u "$OUT/hal.rel.o" | c++filt | sed 's/^ *[Uw] //' | sort -u > "$OUT/undefined.txt"
  echo "=== hal relocatable link: $(wc -l < "$OUT/undefined.txt") UNDEFINED -> $OUT/undefined.txt ==="
  size "$OUT/hal.rel.o" | tail -1
fi
