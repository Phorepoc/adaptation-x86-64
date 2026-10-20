#!/usr/bin/env bash
# ekern_census_x32.sh -- IN-CONTAINER: compile census of the FULL EKA2 kernel (ekern) for
# the Path C target (amd64 CPU, x32 ABI), the way tools/kernel/ekern_compile_check.sh did
# for ARM Track B. UNMODIFIED baseline source; same TU list as ekern.mmp + memmodel.mmp with
# the X86 branch selected. Compile-only (no link): the point is to SIZE the port -- which
# TUs are free, which need an x64 sibling. Never stops on failure; bounded output.
#   MM=multiple|flexible (default multiple = what Track B boots on ARM)   SMP=1|0 (default 1)
#   ABI=-m64: the LP64 "canary" census -- same TUs, 64-bit pointers (-fpermissive + relaxed size
#           asserts, as the LP64 nanokernel build needs). Its failures = where EKA2 assumes ILP32.
#   LINK=1: keep objects (only the TUs ekern.mmp really lists), ld -r them with the nkernsmp
#           set from make_smp.sh and print the UNDEFINED symbols = the ekern backend surface.
set -u
cd /repo
EKA=baseline/kernelhwsrv/kernel/eka
PORT=tools/phorepoc-x86/port
MM="${MM:-multiple}"; SMP="${SMP:-1}"
ABI="${ABI:--mx32}"
if [ "$ABI" = "-m64" ]; then SFX="-lp64"; ABIX="-fpermissive"; RELAX="-include $PORT/x64smp/relax_size_asserts.h"; else SFX=""; ABIX=""; RELAX=""; fi
OUT="/out/ekern-census-$MM$SFX"; mkdir -p "$OUT"
# LP64: -fpermissive turns "cast loses precision" into a WARNING, so warnings are the real signal there.
if [ "$ABI" = "-m64" ]; then WARN="-Wno-all -Wno-invalid-offsetof -Wno-return-local-addr -Wno-deprecated -Wno-attributes"; else WARN="-w"; fi

# Our x64 nk_plat.h must win over the IA-32 one, so the port dir comes first; the baseline
# x86 dir stays on the path for headers we have no sibling for yet (apic.h).
if [ "$SMP" = 1 ]; then
  NK="-I $PORT/x64smp -I $EKA/include/nkernsmp -I $EKA/include/nkernsmp/x86"; SMPDEF="-D__SMP__"
else
  NK="-I $PORT/x64 -I $EKA/include/nkern"; SMPDEF=""
fi

if [ "$MM" = flexible ]; then
  MMDEF="-D__MEMMODEL_FLEXIBLE__ -DMM_FLEXIBLE"
  # port/x64mm first: long-mode page-table geometry (mmboot.h) + 64-bit TPde/TPte (mmtypes.h)
  MMINC="-I $PORT/x64mm -I $EKA/include/memmodel/epoc/flexible -I $EKA/include/memmodel/epoc/flexible/x86 \
         -I $EKA/memmodel/epoc/flexible -I $EKA/memmodel/epoc/flexible/mmu -I $EKA/memmodel/epoc/flexible/mmu/x86 \
         -I $EKA/include/memmodel/epoc/mmubase -I $EKA/memmodel/epoc/mmubase"
  MMTUS="$(cd $EKA && ls memmodel/epoc/flexible/*.cpp memmodel/epoc/flexible/mmu/*.cpp \
           memmodel/epoc/flexible/mmu/x86/*.cpp memmodel/epoc/flexible/x86/*.cpp \
           memmodel/epoc/mmubase/kblockmap.cpp memmodel/epoc/mmubase/ramalloc.cpp memmodel/epoc/mmubase/defragbase.cpp 2>/dev/null)"
else
  MMDEF="-D__MEMMODEL_MULTIPLE__ -DMM_MULTIPLE"
  MMINC="-I $EKA/include/memmodel/epoc/multiple -I $EKA/include/memmodel/epoc/multiple/x86 \
         -I $EKA/include/memmodel/epoc/mmubase -I $EKA/memmodel/epoc/multiple \
         -I $EKA/memmodel/epoc/multiple/x86 -I $EKA/memmodel/epoc/mmubase"
  MMTUS="$(cd $EKA && ls memmodel/epoc/mmubase/*.cpp memmodel/epoc/multiple/*.cpp \
           memmodel/epoc/multiple/x86/*.cpp 2>/dev/null)"
fi

CXX="gcc $ABI $ABIX -ffreestanding -fno-pic -fno-stack-protector -nostdinc -std=gnu++03 \
     -fno-exceptions -fno-rtti -fno-lifetime-dse -fstack-reuse=named_vars -fno-strict-aliasing -fno-delete-null-pointer-checks $WARN -mno-red-zone -mgeneral-regs-only -masm=intel -O2"
# DBG=1: debug kernel (-D_DEBUG) -- EKA2's __KTRACE_OPT boot tracing + __NK_ASSERT_DEBUG become live.
DBGDEF=""; [ "${DBG:-0}" = 1 ] && DBGDEF="-D_DEBUG"
DEFS="-D__CPU_X86 -D__GCC32__ -D__EPOC32__ -D__SYMBIAN32__ -D__KERNEL_MODE__ -D__IN_KERNEL__ \
      -D_UNICODE $DBGDEF $SMPDEF $MMDEF -DX64_FAIL_DLLDATA=${FAIL_DLLDATA:-0}"
# toolchain/kinc = Track B's staging dir for headers the real build exports to a relocated
# path (<kernel/heap_hybrid.h>) or GENERATES (exec_enum.h from execs.txt). Arch-neutral.
INCS="$NK -I $EKA/include -I $EKA/include/kernel -I $EKA/include/kernel/x86 \
      -I $EKA/include/memmodel/epoc $MMINC -I $EKA/memmodel/epoc -I $EKA/klib -I $EKA/common \
      -I baseline/kernelhwsrv/halservices/hal/inc -I toolchain/kinc -I toolchain/kinc/kernel -I $PORT"
PRE="-include $PORT/port_prelude.h -include e32def.h $RELAX"

TUS="$(cd $EKA && ls kernel/*.cpp kernel/x86/*.cpp klib/*.cpp klib/x86/*.cpp 2>/dev/null)
common/alloc.cpp common/array.cpp common/des8.cpp common/mem.cpp common/heap_hybrid.cpp
common/debugfunction.cpp common/secure.cpp
$(cd $EKA && ls memmodel/epoc/*.cpp)
$MMTUS"

LINK="${LINK:-0}"; OBJ="$OUT/obj"; [ "$LINK" = 1 ] && { rm -rf "$OBJ"; mkdir -p "$OBJ"; }
pass=0; fail=0; : > "$OUT/failed.txt"; : > "$OUT/errors.txt"; : > "$OUT/warnings.txt"
for tu in $TUS; do
  # not in ekern.mmp (crash logger / debug monitor variants / EABI-only helper)
  case "$tu" in kernel/crash*|kernel/smon*|kernel/kdebug.cpp|kernel/array_eabi.cpp|kernel/x86/cmon*) continue;; esac
  o=/dev/null
  if [ "$LINK" = 1 ]; then
    # other binaries' entry points / exstart.exe / the monitor / a .cpp the mmp does not list
    case "$tu" in kernel/x86/*_entry*.cpp|kernel/exstart.cpp|*monitor.cpp|memmodel/epoc/multiple/x86/xsched.cpp) continue;; esac
    o="$OBJ/$(echo "$tu" | tr '/' '_').o"
  fi
  log="$OUT/$(echo "$tu" | tr '/' '_').log"
  XD=""
  # Kern::Printf must reach the serial port before any variant exists (xk_entry.cpp DebugOutputChar)
  case "$tu" in kernel/x86/cutils.cpp) XD="-D__EARLY_DEBUG__";;
    # debug-only code reaches a private member from a non-friend (older compilers let it through)
    memmodel/epoc/flexible/mmu/mpager.cpp) XD="-fno-access-control";; esac
  src="$EKA/$tu"
  # flexible model: our long-mode sibling replaces the IA-32 arch MMU file (marked-delta copy)
  [ "$tu" = memmodel/epoc/flexible/mmu/x86/xmmu.cpp ] && src="$PORT/x64mm/xmmu.cpp"
  # X4.34: CreateDataBssStackArea pins an EXE's statics at KX64ExeDataBase (marked-delta copy)
  [ "$tu" = memmodel/epoc/flexible/mprocess.cpp ] && src="$PORT/x64mm/mprocess.cpp"
  # X4.36: the common allocator keeps out of that pinned window (marked-delta copy)
  [ "$tu" = memmodel/epoc/flexible/mmu/maddressspace.cpp ] && src="$PORT/x64mm/maddressspace.cpp"
  # X4.44: FindRomCode never hands back a ROM code segment whose load failed (marked-delta copy)
  [ "$tu" = memmodel/epoc/pcodeseg.cpp ] && src="$PORT/x64mm/pcodeseg.cpp"
  if $CXX $DEFS $XD $INCS $PRE -c "$src" -o "$o" 2>"$log"; then
    pass=$((pass+1)); if [ "$ABI" = "-m64" ]; then grep -h "warning:" "$log" >> "$OUT/warnings.txt"; fi; rm -f "$log"
  else
    fail=$((fail+1)); echo "$tu" >> "$OUT/failed.txt"
    n=$(grep -c "error:" "$log")
    printf "FAIL %-46s %3d errors | %s\n" "$tu" "$n" "$(grep -m1 "error:" "$log" | sed 's/.*error: //' | cut -c1-90)"
    grep "error:" "$log" | sed 's/.*error: //' >> "$OUT/errors.txt"
  fi
done
echo "=== ekern census ABI=$ABI (MM=$MM SMP=$SMP): $pass OK, $fail FAIL ==="
echo "--- total errors: $(wc -l < "$OUT/errors.txt") ---"
echo "--- top error classes ---"
sed -E "s/'[^']*'/'X'/g" "$OUT/errors.txt" | sort | uniq -c | sort -rn | head -25
ls $EKA/kernel/x86/*.cia $EKA/klib/x86/*.cia $EKA/common/x86/*.cia $EKA/memmodel/epoc/$MM/x86/*.cia 2>/dev/null \
  | wc -l | sed 's/^/MSVC-_asm .cia files NOT attempted (need x64 siblings): /'

if [ "$LINK" = 1 ]; then
  NK=/out/nkernsmp
  echo "--- amd64 ekern backend (port/x64ekern) ---"
  for b in $PORT/x64ekern/*.cpp; do
    n=$(basename "$b" .cpp)
    if $CXX $DEFS $INCS $PRE -c "$b" -o "$OBJ/x64ekern_$n.o" 2>"$OUT/x64ekern_$n.log"; then echo "  OK   $n"
    else echo "  FAIL $n"; grep -m8 "error:" "$OUT/x64ekern_$n.log" | sed 's/^/       /' | cut -c1-200; fi
  done
  ld -m elf32_x86_64 -r -o "$OUT/ekern_all.o" "$OBJ"/*.o      $NK/nklib.o $NK/nkern.o $NK/nkerns.o $NK/sched.o $NK/dfcs.o $NK/nk_timer.o $NK/nk_irq.o      $NK/nk_bal.o $NK/x64s_atomics.o $NK/x64s_ncutils.o $NK/x64s_ncsched.o $NK/x64_ctxsw.o 2>"$OUT/ld-r.log"     || { echo "ld -r FAILED:"; head -20 "$OUT/ld-r.log"; }
  nm -u "$OUT/ekern_all.o" | awk '{print $2}' | c++filt | sort -u > "$OUT/undefined.txt"
  echo "=== ekern+nkernsmp relocatable link: $(wc -l < "$OUT/undefined.txt") UNDEFINED symbols -> $OUT/undefined.txt ==="
  grep -c "multiple definition" "$OUT/ld-r.log" | sed 's/^/multiple-definition lines: /'
  size "$OUT/ekern_all.o" | tail -1
fi

if [ "$ABI" = "-m64" ]; then
  sort -u "$OUT/warnings.txt" > "$OUT/warnings.uniq.txt"     # a header line warns once per includer
  echo "=== LP64 canary: $(wc -l < "$OUT/warnings.uniq.txt") distinct warning sites ==="
  echo "--- by class ---"
  sed -E "s/^[^ ]+ warning: //; s/'[^']*'/'X'/g; s/ \[-W[^]]*\]//" "$OUT/warnings.uniq.txt" | sort | uniq -c | sort -rn | head -12
  echo "--- pointer<->integer width sites, by file (top 25) ---"
  grep -E "loses precision|different size" "$OUT/warnings.uniq.txt" | cut -d: -f1 | sed "s|$EKA/||" | sort | uniq -c | sort -rn | head -25
  echo "pointer<->integer width sites TOTAL: $(grep -cE "loses precision|different size" "$OUT/warnings.uniq.txt")  in $(grep -E "loses precision|different size" "$OUT/warnings.uniq.txt" | cut -d: -f1 | sort -u | wc -l) files"
fi
