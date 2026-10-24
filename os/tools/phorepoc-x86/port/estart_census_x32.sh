#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

# estart_census_x32.sh -- IN-CONTAINER: compile census of ESTART (the base startup process) for
# the Path C target (amd64 CPU, x32 ABI). UNMODIFIED baseline source, the TU list of
# estart/estart.mmp. Sibling of efile_census_x32.sh: compile-only, never stops on failure,
# bounded output. The point is to SIZE the port before committing to it.
#   LINK=1: keep objects, ld -r them and list the UNDEFINED symbols = ESTART's import surface,
#           and publish the compile line (flags.txt) for emitter/build_estart_x32.py.
#   host:  docker run --rm -v <repo>:/repo -v <repo>/.phorepoc-x86-build:/out phorepoc-x86 \
#              bash /repo/tools/phorepoc-x86/port/estart_census_x32.sh
#
# ESTART is tiny next to EFILE -- two TUs, ~2700 lines -- and is an EXE over the same three DLLs
# (euser/efsrv/hal), so the builder shape is build_efile_x32.py with a different root set.
# estart.mmp also lists domaincli.lib, which we do NOT have: -DSYMBIAN_EXCLUDE_ESTART_DOMAIN_MANAGER
# is a stock build-variant macro that guards BOTH the <domainmanager.h> include (estart.cpp:26)
# and the two use sites (2098, 2246), so the dependency disappears without touching the source.
# Same -iquote/-D shims as the EFILE census, and for the same reasons (written out there):
#   -iquote for the fileserver's own inc/ (the KERNEL's common.h otherwise shadows it)
#   -DSYMBIAN_ENABLE_64_BIT_FILE_SERVER_API (variant-wide in the real build)
#   -fno-access-control (f32 headers make members private that friends then name)
# item 1.5.
set -u
cd /repo
EKA=baseline/kernelhwsrv/kernel/eka
FS=baseline/kernelhwsrv/userlibandfileserver/fileserver
PORT=tools/phorepoc-x86/port
OUT=/out/estart-census; mkdir -p "$OUT"
LINK="${LINK:-0}"; OBJ="$OUT/obj"; [ "$LINK" = 1 ] && { rm -rf "$OBJ"; mkdir -p "$OBJ"; }

# Identical user-side compile line to the efile census. -D__EXE__: estart.mmp is TARGETTYPE exe.
CXX="gcc -mx32 -ffreestanding -fno-pic -fno-stack-protector -nostdinc -std=gnu++03 \
     -fno-exceptions -fno-rtti -fno-lifetime-dse -fstack-reuse=named_vars -fno-strict-aliasing -fno-delete-null-pointer-checks -w -masm=intel -O2 -fno-builtin -fno-tree-loop-distribute-patterns \
     -fshort-wchar -fno-access-control"
DEFS="-D__CPU_X86 -D__GCC32__ -D__EPOC32__ -D__SYMBIAN32__ -D_UNICODE -D__EXE__ \
      -DSYMBIAN_ENABLE_64_BIT_FILE_SERVER_API -DSYMBIAN_EXCLUDE_ESTART_DOMAIN_MANAGER \
      ${ESTART_DEBUG:+-D_DEBUG} ${EXTRA_DEFS:-}"
QUOTE="-iquote $FS/inc -iquote $FS/estart -iquote $FS/sfsrv"
INCS="-I toolchain/overlay -I $PORT/x64user -I $FS/estart -I $FS/inc -I $FS/sfsrv \
      -I $EKA/include -I $EKA/euser -I $EKA/common \
      -I baseline/kernelhwsrv/halservices/hal/inc -I toolchain/kinc -I toolchain/generated -I $PORT"
PRE="-include $PORT/port_prelude.h -include e32def.h"
[ "$LINK" = 1 ] && echo "$CXX $DEFS $QUOTE $INCS $PRE" > "$OUT/flags.txt"

TUS="$FS/estart/estart.cpp $FS/estart/estartmain.cpp"

pass=0; fail=0; : > "$OUT/failed.txt"; : > "$OUT/errors.txt"
for tu in $TUS; do
  [ -f "$tu" ] || { echo "MISSING $tu" >> "$OUT/failed.txt"; fail=$((fail+1)); continue; }
  o=/dev/null; [ "$LINK" = 1 ] && o="$OBJ/$(basename "$tu" | tr "." "_").o"
  if $CXX $DEFS $QUOTE $INCS $PRE -c "$tu" -o "$o" 2>"$OUT/one.err"; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "$(basename "$tu")" >> "$OUT/failed.txt"
    { echo "=== $(basename "$tu")"; grep -m6 "error" "$OUT/one.err" | cut -c1-220; } >> "$OUT/errors.txt"
  fi
done
echo "=== estart census -mx32: $pass OK, $fail FAIL ==="
head -10 "$OUT/failed.txt"
echo "--- first errors (full list: $OUT/errors.txt)"; head -60 "$OUT/errors.txt"

if [ "$LINK" = 1 ]; then
  ld -m elf32_x86_64 -r -o "$OUT/estart.rel.o" "$OBJ"/*.o 2>"$OUT/ld.log" || head -5 "$OUT/ld.log"
  nm -u "$OUT/estart.rel.o" | c++filt | sed 's/^ *U //' | sort -u > "$OUT/undefined.txt"
  echo "=== estart relocatable link: $(wc -l < "$OUT/undefined.txt") UNDEFINED -> $OUT/undefined.txt ==="
  size "$OUT/estart.rel.o" | tail -1
fi
