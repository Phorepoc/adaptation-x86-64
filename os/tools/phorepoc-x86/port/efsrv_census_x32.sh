#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

# efsrv_census_x32.sh -- IN-CONTAINER: compile census of EFSRV (the F32 CLIENT library, efsrv.dll)
# for the Path C target (amd64 CPU, x32 ABI). UNMODIFIED baseline source, the TU list of
# sfsrv/efsrv.mmh. Sibling of euser_census_x32.sh / efile_census_x32.sh.
#   LINK=1: keep objects + publish the compile line (flags.txt) for emitter/build_efsrv_x32.py.
#   host:  docker run --rm -v <repo>:/repo -v <repo>/.phorepoc-x86-build:/out phorepoc-x86 \
#              bash /repo/tools/phorepoc-x86/port/efsrv_census_x32.sh
#
# efsrv is what EVERY F32 client links against (RFs/RFile/RDir/TParse...); EFILE itself imports
# 39 of its ordinals. It is pure client-side C++ -- no asm, no arch branch -- so unlike euser
# there is nothing here that needs an amd64 sibling.
# efsrv.mmh: TARGETTYPE dll, UID 0x1000008d 0x100039e4, DEFFILE efsrv.def.
set -u
cd /repo
EKA=baseline/kernelhwsrv/kernel/eka
FS=baseline/kernelhwsrv/userlibandfileserver/fileserver
PORT=tools/phorepoc-x86/port
OUT=/out/efsrv-census; mkdir -p "$OUT"
LINK="${LINK:-0}"; OBJ="$OUT/obj"; [ "$LINK" = 1 ] && { rm -rf "$OBJ"; mkdir -p "$OBJ"; }

# The euser census's user-side compile line + the fileserver component's shims (the rationale
# for each is in tools/kernel/efile_compile_check.sh; they are architecture-independent).
CXX="gcc -mx32 -ffreestanding -fno-pic -fno-stack-protector -nostdinc -std=gnu++03 \
     -fno-exceptions -fno-rtti -fno-lifetime-dse -fstack-reuse=named_vars -fno-strict-aliasing -fno-delete-null-pointer-checks -w -masm=intel -O2 -fno-builtin -fno-tree-loop-distribute-patterns \
     -fshort-wchar -fno-access-control"
DEFS="-D__CPU_X86 -D__GCC32__ -D__EPOC32__ -D__SYMBIAN32__ -D_UNICODE -D__DLL__ \
      -DSYMBIAN_ENABLE_64_BIT_FILE_SERVER_API ${EXTRA_DEFS:-}"
QUOTE="-iquote $FS/inc -iquote $FS/sfsrv -iquote $FS/ftrace"
INCS="-I toolchain/overlay -I $PORT/x64user -I $FS/sfsrv -I $FS/sfsrv/traces -I $FS/inc -I $FS/ftrace \
      -I $EKA/include -I $EKA/euser -I $EKA/common \
      -I baseline/kernelhwsrv/halservices/hal/inc -I toolchain/kinc -I toolchain/generated -I $PORT"
# Linux: the source keeps some headers in mixed case (F32plugin.h); the build's casefix links answer the lower-case name
[ -d /out/casefix ] && INCS="$INCS -idirafter /out/casefix"
PRE="-include $PORT/port_prelude.h -include e32def.h"
[ "$LINK" = 1 ] && echo "$CXX $DEFS $QUOTE $INCS $PRE" > "$OUT/flags.txt"

TUS="cl_cdir.cpp cl_cli.cpp cl_debug.cpp cl_dir.cpp
cl_drive.cpp cl_entry.cpp cl_file.cpp cl_find.cpp
cl_fman.cpp cl_fmt.cpp cl_fraw.cpp cl_ftext.cpp
cl_main.cpp cl_parse.cpp cl_scan.cpp cl_insecure.cpp
cl_plugin.cpp cl_blockmap.cpp cl_clamp.cpp
cl_shim.cpp cl_notification.cpp"

pass=0; fail=0; : > "$OUT/failed.txt"; : > "$OUT/errors.txt"
for tu in $TUS; do
  src="$FS/sfsrv/$tu"
  [ -f "$src" ] || { echo "MISSING $tu" >> "$OUT/failed.txt"; fail=$((fail+1)); continue; }
  o=/dev/null; [ "$LINK" = 1 ] && o="$OBJ/$(echo "$tu" | tr "." "_").o"
  if $CXX $DEFS $QUOTE $INCS $PRE -c "$src" -o "$o" 2>"$OUT/one.err"; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "$tu" >> "$OUT/failed.txt"
    { echo "=== $tu"; grep -m4 "error" "$OUT/one.err" | cut -c1-220; } >> "$OUT/errors.txt"
  fi
done
echo "=== efsrv census -mx32: $pass OK, $fail FAIL ==="
head -40 "$OUT/failed.txt"
echo "--- first errors (full list: $OUT/errors.txt)"; head -60 "$OUT/errors.txt"

if [ "$LINK" = 1 ]; then
  ld -m elf32_x86_64 -r -o "$OUT/efsrv.rel.o" "$OBJ"/*.o 2>"$OUT/ld.log" || head -5 "$OUT/ld.log"
  nm -u "$OUT/efsrv.rel.o" | c++filt | sed 's/^ *U //' | sort -u > "$OUT/undefined.txt"
  echo "=== efsrv relocatable link: $(wc -l < "$OUT/undefined.txt") UNDEFINED -> $OUT/undefined.txt ==="
  size "$OUT/efsrv.rel.o" | tail -1
fi
