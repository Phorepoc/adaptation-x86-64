#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

# euser_census_x32.sh -- IN-CONTAINER: compile census of EUSER (the user library) for the Path C
# target (amd64 CPU, x32 ABI). UNMODIFIED baseline source, the TU list of euser.mmh with the X86
# branch selected. Sibling of ekern_census_x32.sh: compile-only, never stops on failure, bounded
# output. The point is to SIZE the userland port: which TUs are free, which need an amd64 sibling
# (expected: the IA-32 .cia asm files -- cdecl stack arguments do not exist under SysV).
#   LINK=1: keep objects, ld -r them and list the UNDEFINED symbols = the euser backend surface.
#   host:  docker.exe run --rm -v <repo>:/repo -v <repo>/.phorepoc-x86-build:/out phorepoc-x86 \
#              bash /repo/tools/phorepoc-x86/port/euser_census_x32.sh
# IA-32 asm (.cia) TUs are replaced by amd64 siblings in port/x64user/ (@-prefixed below):
#   uc_exec.cia -> xu_exec.cpp (+ shadowed u32exec.h)   uc_trp/uc_utl/uc_i64.cia -> xu_utl.cpp
#   atomics.cia -> x64smp/x64s_atomics.cpp (shared with the kernel)   cmem.cia -> generic common/mem.cpp
#   uc_realx.cia (3348 lines of TRealX asm) -> xu_realx.cpp (portable integer C++, hosted test realx_test.cpp)
#   uc_seh.cpp = Win32 SEH, n/a.
set -u
cd /repo
EKA=baseline/kernelhwsrv/kernel/eka
PORT=tools/phorepoc-x86/port
OUT=/out/euser-census; mkdir -p "$OUT"
LINK="${LINK:-0}"; OBJ="$OUT/obj"; [ "$LINK" = 1 ] && { rm -rf "$OBJ"; mkdir -p "$OBJ"; }

# User side: SSE is allowed (no -mgeneral-regs-only), red zone is fine, still freestanding.
CXX="gcc -mx32 -ffreestanding -fno-pic -fno-stack-protector -nostdinc -std=gnu++03 \
     -fno-exceptions -fno-rtti -fno-lifetime-dse -fstack-reuse=named_vars -fno-strict-aliasing -fno-delete-null-pointer-checks -w -masm=intel -O2 -fno-builtin -fno-tree-loop-distribute-patterns -fshort-wchar"
# SYMBIAN_DISTINCT_LOCALE_MODEL (X4.72): variant-wide on S^3 (Symbian_OS.hrh) -- every other component compiles with
# it; without it euser's three-DLL TExtendedLocale::LoadLocale is a stub returning KErrNotSupported and SSM's
# language selection (ssmlangselcmd) failed the boot
DEFS="-D__CPU_X86 -D__GCC32__ -D__EPOC32__ -D__SYMBIAN32__ -D_UNICODE -D__DLL__ -DSYMBIAN_DISTINCT_LOCALE_MODEL ${EXTRA_DEFS:-}"
# port/x64user first: amd64 siblings of user-side arch headers (u32exec.h exec stubs)
# (-idirafter baseline: the Symbian root, for <kernelhwsrv/...> includes of our port files, as every component has it)
INCS="-idirafter baseline -I toolchain/overlay -I $PORT/x64user -I $EKA/include -I $EKA/euser -I $EKA/euser/unicode -I $EKA/common \
      -I baseline/kernelhwsrv/halservices/hal/inc -I toolchain/kinc -I toolchain/generated -I $PORT"
PRE="-include $PORT/port_prelude.h -include e32def.h"
# LINK=1: publish the exact compile line so emitter/build_euser_x32.py builds its own TUs
# (xu_dll.cpp, the unimpl stubs) with the SAME flags instead of a drifting copy of them.
[ "$LINK" = 1 ] && echo "$CXX $DEFS $INCS $PRE" > "$OUT/flags.txt"

TUS="cbase/ub_act.cpp cbase/ub_array.cpp cbase/ub_bma.cpp cbase/ub_buf.cpp cbase/ub_circ.cpp cbase/ub_cln.cpp
cbase/ub_cons.cpp cbase/ub_dtim.cpp cbase/ub_obj.cpp cbase/ub_svr.cpp cbase/ub_polsvr.cpp cbase/ub_tim.cpp
cbase/ub_utl.cpp cbase/ub_tque.cpp
us_evnt.cpp locmapping.cpp us_exec.cpp us_func.cpp us_graph.cpp us_hand.cpp us_ksvr.cpp @x64user/xu_des.cpp
us_lex16.cpp us_lex8.cpp us_loc.cpp us_mes.cpp us_parse.cpp us_power.cpp us_property.cpp us_que.cpp us_ref.cpp
us_regn.cpp us_test.cpp us_time.cpp us_trp.cpp us_utl.cpp us_mqueue.cpp us_encode.cpp us_decode.cpp
us_secure.cpp us_htab.cpp us_rwlock.cpp us_shbuf.cpp us_gcc.cpp
../common/array.cpp ../common/des8.cpp ../common/des16.cpp ../common/alloc.cpp ../common/heap_hybrid.cpp
../common/debugfunction.cpp @x64user/xu_mem.cpp ../common/secure.cpp ../common/gcchelp.cpp
maths/um_asin.cpp maths/um_atan.cpp maths/um_dtor.cpp maths/um_exp.cpp maths/um_frac.cpp maths/um_int.cpp
maths/um_ln.cpp maths/um_log.cpp maths/um_mod.cpp maths/um_pow.cpp maths/um_pow10.cpp maths/um_rand.cpp
maths/um_rtod.cpp maths/um_sin.cpp maths/um_spec.cpp maths/um_sqrt.cpp maths/um_tan.cpp maths/um_utl.cpp
epoc/up_lib.cpp epoc/up_utl.cpp
@x64user/xu_exec.cpp @x64user/xu_utl.cpp @x64user/xu_realx.cpp @x64smp/x64s_atomics.cpp
epoc/x86/uc_exec.cpp epoc/x86/uc_i64.cpp epoc/x86/uc_realx.cpp epoc/x86/uc_trp.cpp epoc/x86/uc_utl.cpp
unicode/collate.cpp unicode/unicode.cpp unicode/unitable.cpp unicode/Compare.cpp"

pass=0; fail=0; : > "$OUT/failed.txt"; : > "$OUT/errors.txt"
for tu in $TUS; do
  src="$EKA/euser/$tu"; case "$tu" in @*) src="$PORT/${tu#@}";; esac
  tuflags=""; case "$tu" in */des8.cpp|*/des16.cpp) tuflags="-DPORT_FOFF_DYNAMIC";; esac
  [ -f "$src" ] || { echo "MISSING $tu" >> "$OUT/failed.txt"; fail=$((fail+1)); continue; }
  o=/dev/null; [ "$LINK" = 1 ] && o="$OBJ/$(echo "$tu" | tr "/.@" "___").o"
  lang=""; case "$tu" in *.cia) lang="-x c++ -D__CIA__";; esac
  if $CXX $DEFS $INCS $PRE $tuflags $lang -c "$src" -o "$o" 2>"$OUT/one.err"; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "$tu" >> "$OUT/failed.txt"
    { echo "=== $tu"; grep -m4 "error" "$OUT/one.err" | cut -c1-220; } >> "$OUT/errors.txt"
  fi
done
echo "=== euser census -mx32: $pass OK, $fail FAIL ==="
head -40 "$OUT/failed.txt"
echo "--- first errors (full list: $OUT/errors.txt)"; head -60 "$OUT/errors.txt"

if [ "$LINK" = 1 ]; then
  ld -m elf32_x86_64 -r -o "$OUT/euser.rel.o" "$OBJ"/*.o 2>"$OUT/ld.log" || head -5 "$OUT/ld.log"
  nm -u "$OUT/euser.rel.o" | c++filt | sed 's/^ *U //' | sort -u > "$OUT/undefined.txt"
  echo "=== euser relocatable link: $(wc -l < "$OUT/undefined.txt") UNDEFINED -> $OUT/undefined.txt ==="
  size "$OUT/euser.rel.o" | tail -1
fi
