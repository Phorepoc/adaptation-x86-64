#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

# efile_census_x32.sh -- IN-CONTAINER: compile census of EFILE (the F32 file server) for the
# Path C target (amd64 CPU, x32 ABI). UNMODIFIED baseline source, the TU list of sfile/efile.mmp.
# Sibling of euser_census_x32.sh / ekern_census_x32.sh: compile-only, never stops on failure,
# bounded output. The point is to SIZE the port before committing to it.
#   LINK=1: keep objects, ld -r them and list the UNDEFINED symbols = EFILE's import surface,
#           and publish the compile line (flags.txt) for emitter/build_efile_x32.py.
#   host:  docker.exe run --rm -v <repo>:/repo -v <repo>/.phorepoc-x86-build:/out phorepoc-x86 \
#              bash /repo/tools/phorepoc-x86/port/efile_census_x32.sh
#
# The TU list and every include/define shim below come from the ARM side, which already proves
# this component compiles unmodified (tools/kernel/efile_compile_check.sh, 48/48 -- Track-B M7;
# the reasoning for each shim is written out there and is architecture-independent):
#   -iquote for the fileserver's own inc/ (else the KERNEL's common.h shadows it and its
#      e32panic.h TBasePanic collides with sf_std.h's TFsFault enumerators)
#   -DSYMBIAN_ENABLE_64_BIT_FILE_SERVER_API (variant-wide in the real build; KCurrentPosition64)
#   -fno-access-control (sf_func.h makes AddFsToCompositeMountL private unless __GCC32__, and
#      sf_std.h then befriends it -- modern g++ rejects naming a private member in a friend decl)
#   sfat32/ on the path for <filesystem_fat.h>; halservices for <hal.h>
# ARCH DELTA vs the ARM list: kernel/byte_pair.cpp (the portable C++ #else arm) replaces
# kernel/arm/byte_pair.cia, which is hand-written ARM naked asm.
set -u
cd /repo
EKA=baseline/kernelhwsrv/kernel/eka
FS=baseline/kernelhwsrv/userlibandfileserver/fileserver
PORT=tools/phorepoc-x86/port
OUT=/out/efile-census; mkdir -p "$OUT"
LINK="${LINK:-0}"; OBJ="$OUT/obj"; [ "$LINK" = 1 ] && { rm -rf "$OBJ"; mkdir -p "$OBJ"; }

# Same user-side compile line as the euser census (SSE allowed, red zone fine, freestanding),
# with EFILE's own shims appended. -D__EXE__: efile.mmp is TARGETTYPE exexp.
CXX="gcc -mx32 -ffreestanding -fno-pic -fno-stack-protector -nostdinc -std=gnu++03 \
     -fno-exceptions -fno-rtti -fno-lifetime-dse -fstack-reuse=named_vars -fno-strict-aliasing -fno-delete-null-pointer-checks -w -masm=intel -O2 -fno-builtin -fno-tree-loop-distribute-patterns \
     -fshort-wchar -fno-access-control"
DEFS="-D__CPU_X86 -D__GCC32__ -D__EPOC32__ -D__SYMBIAN32__ -D_UNICODE -D__EXE__ \
      -DSYMBIAN_ENABLE_64_BIT_FILE_SERVER_API -DSYMBIAN_DISTINCT_LOCALE_MODEL ${EFILE_DEBUG:+-D_DEBUG} ${EXTRA_DEFS:-}"
# SYMBIAN_DISTINCT_LOCALE_MODEL (X4.72, variant-wide on S^3): the loader's LoadLocale (sf_ldr.cpp) otherwise takes the
# old-model path and calls a new-model locale DLL's missing FAT-utility export -- the loader thread jumped to 0
# -iquote is searched before every -I for "quote" includes: the fileserver's own headers win.
QUOTE="-iquote tools/phorepoc-x86/port/f32 -iquote $FS/inc -iquote $FS/sfile -iquote $FS/sfsrv -iquote $FS/ftrace"
INCS="-I tools/phorepoc-x86/port/f32 -I toolchain/overlay -I $PORT/x64user -I $FS/sfsrv -I $FS/sfile -I $FS/inc -I $FS/ftrace -I $FS/sfat32 \
      -I $EKA/include -I $EKA/euser -I $EKA/common \
      -I baseline/kernelhwsrv/halservices/hal/inc -I toolchain/kinc -I toolchain/generated -I $PORT"
PRE="-include $PORT/port_prelude.h -include e32def.h"
[ "$LINK" = 1 ] && echo "$CXX $DEFS $QUOTE $INCS $PRE" > "$OUT/flags.txt"

TUS="$FS/sfsrv/cl_file.cpp $FS/sfsrv/cl_cli.cpp $FS/sfsrv/cl_dir.cpp
$FS/sfile/sf_amh.cpp $FS/sfile/sf_dat.cpp $FS/sfile/sf_debug.cpp $FS/sfile/sf_dir.cpp
$FS/sfile/sf_drv.cpp $FS/sfile/sf_file.cpp $FS/sfile/sf_fmt.cpp
$FS/sfile/sf_ldr.cpp $FS/sfile/sf_main.cpp $FS/sfile/sf_nbs.cpp $FS/sfile/sf_ses.cpp
$FS/sfile/sf_svr.cpp $FS/sfile/sf_sys.cpp $FS/sfile/sf_utl.cpp $FS/sfile/sf_mnt.cpp
$FS/sfile/sf_raw.cpp $FS/sfile/sf_prel.cpp $FS/sfile/sf_disk.cpp
$FS/sfile/sf_request.cpp $FS/sfile/sf_ops.cpp $FS/sfile/sf_notify.cpp
$FS/sfile/sf_thread.cpp $FS/sfile/sf_local.cpp $FS/sfile/sf_obj.cpp
$FS/sfile/sf_ext.cpp $FS/sfile/sf_hash.cpp
$FS/sfile/sf_plugin.cpp $FS/sfile/sf_plugin_ops.cpp $FS/sfile/sf_plugin_man.cpp
$FS/sfile/Sf_plugin_util.cpp
$FS/sfile/sf_cache_man.cpp $FS/sfile/sf_cache_client.cpp
$FS/sfile/sf_file_cache.cpp
$FS/sfile/sf_memory_man.cpp $FS/sfile/sf_memory_client.cpp
$FS/sfile/sf_plugin_shim.cpp
$FS/sfile/sf_notifier_handlers.cpp
$FS/sfile/sf_pool.cpp
tools/phorepoc-x86/port/f32/sf_lepoc.cpp $FS/sfile/sf_inflate.cpp $FS/sfile/sf_cache.cpp
$FS/sfile/sf_decomp.cpp $FS/sfile/sf_pgcompr.cpp
$EKA/kernel/byte_pair.cpp
$FS/srom/sr_rom.cpp"

pass=0; fail=0; : > "$OUT/failed.txt"; : > "$OUT/errors.txt"
for tu in $TUS; do
  [ -f "$tu" ] || { echo "MISSING $tu" >> "$OUT/failed.txt"; fail=$((fail+1)); continue; }
  o=/dev/null; [ "$LINK" = 1 ] && o="$OBJ/$(basename "$tu" | tr "." "_").o"
  if $CXX $DEFS $QUOTE $INCS $PRE -c "$tu" -o "$o" 2>"$OUT/one.err"; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "$(basename "$tu")" >> "$OUT/failed.txt"
    { echo "=== $(basename "$tu")"; grep -m4 "error" "$OUT/one.err" | cut -c1-220; } >> "$OUT/errors.txt"
  fi
done
echo "=== efile census -mx32: $pass OK, $fail FAIL ==="
head -40 "$OUT/failed.txt"
echo "--- first errors (full list: $OUT/errors.txt)"; head -60 "$OUT/errors.txt"

if [ "$LINK" = 1 ]; then
  ld -m elf32_x86_64 -r -o "$OUT/efile.rel.o" "$OBJ"/*.o 2>"$OUT/ld.log" || head -5 "$OUT/ld.log"
  nm -u "$OUT/efile.rel.o" | c++filt | sed 's/^ *U //' | sort -u > "$OUT/undefined.txt"
  echo "=== efile relocatable link: $(wc -l < "$OUT/undefined.txt") UNDEFINED -> $OUT/undefined.txt ==="
  size "$OUT/efile.rel.o" | tail -1
fi
