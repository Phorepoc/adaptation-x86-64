#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-1.0

# make_ekern.sh -- IN-CONTAINER build of the Phorepoc amd64/x32 image that boots the REAL EKA2
# kernel (ekern + flexible memory model + nkernSMP + our amd64 backends), not the nanokernel
# demo (make_smp.sh). One multiboot2 ELF: low boot stub (boot.S + xboot.cpp) + the kernel image
# at EKA2's native linear addresses (ekern.lds). Repo at /repo, output in /out/ekern.
#   host:  docker.exe run --rm -v <repo>:/repo -v <repo>/.phorepoc-x86-build:/out phorepoc-x86 \
#              bash /repo/tools/phorepoc-x86/ekern/make_ekern.sh [run]
# docs/phorepoc/x86-64-port.md X4.4.
set -eu
cd /repo
SRC=tools/phorepoc-x86
PORT=$SRC/port
EK=$SRC/ekern
EKA=baseline/kernelhwsrv/kernel/eka
OUT=/out/ekern
mkdir -p "$OUT"

# X4.53: steps 1-2 (nkern + the ekern census, the slow part) are skipped when nothing they compile has
# changed since the last good build with the same knobs (the stamp is taken BEFORE building, so an edit
# made during a build still invalidates it). The ROM is linked in at step 4, so a new ROM never needs
# them. KCLEAN=1 forces a rebuild.
KSTAMP=$OUT/kernel-objs.stamp
# X4.68: the baseline tree is pinned (baseline.lock) and never edited in place -- its git commit stands for it in
# the key; walking all of kernelhwsrv through the Windows bind mount for a -newer test cost ~7 s per run
git_head() { local g="$1/.git" h; h=$(cat "$g/HEAD" 2>/dev/null); case "$h" in "ref: "*) cat "$g/${h#ref: }" 2>/dev/null \
  || grep -m1 " ${h#ref: }\$" "$g/packed-refs" 2>/dev/null | cut -d' ' -f1;; *) echo "$h";; esac; }
KKEY="UB=${UARTBATCH:-0} DBG=${DBG:-1} FAIL_DLLDATA=${FAIL_DLLDATA:-} KHS=$(git_head baseline/kernelhwsrv)"
if [ "${KCLEAN:-0}" != 1 ] && [ -f "$KSTAMP" ] && [ "$(cat "$KSTAMP")" = "$KKEY" ] && [ -d /out/ekern-census-flexible/obj ] \
   && [ -z "$(find "$PORT" "$EK" toolchain/kinc -newer "$KSTAMP" -type f 2>/dev/null | head -1)" ]; then
  echo "[1-2/5] nkernSMP + ekern objects: unchanged since the last build ($KKEY) -- reused"
else
rm -f "$KSTAMP"; echo "$KKEY" > "$KSTAMP.new"
echo "[1/5] nkernSMP neutral core + amd64 SMP backend (x32)"
ABI=-mx32 NOUT=/out/nkernsmp bash "$PORT/build_nkernsmp_link.sh" >"$OUT/nkernsmp.log" 2>&1 \
  || { tail -20 "$OUT/nkernsmp.log"; exit 1; }

echo "[2/5] ekern + flexible memory model + x64ekern backend (objects via the census, LINK=1)"
DBG="${DBG:-1}" LINK=1 MM=flexible bash "$PORT/ekern_census_x32.sh" >"$OUT/census.log" 2>&1 || true
grep -E "^=== ekern" "$OUT/census.log"
grep -q "0 FAIL" "$OUT/census.log" || { grep -E "FAIL" "$OUT/census.log" | head; exit 1; }
# the amd64 backend (port/x64ekern) is reported per file below the headline and NOT counted in it: a
# failing backend TU used to link its STALE object silently (X4.64: two kernel fixes never ran)
grep -q "^  FAIL " "$OUT/census.log" && { grep -A3 "^  FAIL " "$OUT/census.log" | head -12; exit 1; }
mv "$KSTAMP.new" "$KSTAMP"
fi
# the only symbols the kernel objects may still want are the ones xk_entry.cpp (and xk_threaddump.cpp: X64_CrashWho) supply
if grep -v -x -e "DebugOutputChar(unsigned int)" -e "KPrintf(char const\*, ...)" -e __code_base -e __utext_base -e X64_CrashWho /out/ekern-census-flexible/undefined.txt | grep -q .; then
  echo "unexpected undefined symbols:"; head -20 /out/ekern-census-flexible/undefined.txt; exit 1
fi
KOBJ=/out/ekern-census-flexible/obj

echo "[3/5] boot stub + bootstrap + kernel entry / vectors / static variant"
CXX="gcc -mx32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -nostdinc -std=gnu++03 \
     -fno-exceptions -fno-rtti -fno-lifetime-dse -fstack-reuse=named_vars -fno-strict-aliasing -fno-delete-null-pointer-checks -w -mno-red-zone -mgeneral-regs-only -masm=intel -O2 \
     -fno-builtin -fno-tree-loop-distribute-patterns"
# DBG=1 (default): debug kernel. TRACE=<hex>: TRomHeader::iTraceMask[0], needs DBG=1
# (KBOOT=bit1 KMMU=3 KSCHED=5 KPROC=6 KTHREAD=9 KPANIC=31 ...; default KPANIC|KBOOT).
DBGDEF=""; [ "${DBG:-1}" = 1 ] && DBGDEF="-D_DEBUG"
# SCREEN=WxH (X4.26): the display HAL reports a centred WxH screen inside the framebuffer (S60: 360x640)
SCREENDEF=""; [ -n "${SCREEN:-}" ] && SCREENDEF="-DXK_SCREEN_W=${SCREEN%x*} -DXK_SCREEN_H=${SCREEN#*x}"
# SCALE=n (X4.35): the display shows each screen pixel as an n x n block (pixel doubling: a phone screen
# is tiny on a desktop monitor) and GRUB is asked for a framebuffer of SCREEN x n. Default 2 with SCREEN=;
# SCALE=1 = the old 1024x768 framebuffer with the screen centred 1:1. If GRUB cannot set that mode the
# display takes the largest scale that fits whatever it got. VirtualBox: vbox_test.sh adds the mode.
FBDEF=""
if [ -n "${SCREEN:-}" ] && [ "${SCALE:-2}" -gt 1 ]; then
  FBDEF="-DFB_W=$(( ${SCREEN%x*} * ${SCALE:-2} )) -DFB_H=$(( ${SCREEN#*x} * ${SCALE:-2} ))"
  SCREENDEF="$SCREENDEF -DXK_SCALE=${SCALE:-2}"
fi
DEFS="-D__CPU_X86 -D__GCC32__ -D__EPOC32__ -D__SYMBIAN32__ -D__KERNEL_MODE__ -D__IN_KERNEL__ \
      -D_UNICODE $DBGDEF -DXBOOT_TRACE_MASK=${TRACE:-0x80000002} -DXK_UART_BATCH=${UARTBATCH:-0} -DXK_DEBUG_NMI=${NMI:-0} -DXK_FB_SHADOW=${FBSHADOW:--1} -DXK_DUMP_AT=${DUMP:-0} -DXK_SLIDE_CLOSED=${SLIDE:-0} -DXK_DISK_AS_D=${DISK_AS_D:-0} $SCREENDEF -D__SMP__ -D__MEMMODEL_FLEXIBLE__ -DMM_FLEXIBLE"
INCS="-I $PORT/x64smp -I $EKA/include/nkernsmp -I $EKA/include/nkernsmp/x86 -I $EKA/include \
      -I $EKA/include/kernel -I $EKA/include/kernel/x86 -I $EKA/include/memmodel/epoc \
      -I $PORT/x64mm -I $EKA/include/memmodel/epoc/flexible -I $EKA/include/memmodel/epoc/flexible/x86 \
      -I $EKA/memmodel/epoc/flexible -I $EKA/memmodel/epoc/flexible/mmu -I $EKA/memmodel/epoc/flexible/mmu/x86 \
      -I $EKA/include/memmodel/epoc/mmubase -I $EKA/memmodel/epoc/mmubase -I $EKA/memmodel/epoc \
      -I $EKA/klib -I $EKA/common -I toolchain/kinc -I toolchain/kinc/kernel -I $PORT"
PRE="-include $PORT/port_prelude.h -include e32def.h"
# X4.68: step 3's objects depend only on the knobs (DEFS, FBDEF), the pinned kernel (KKEY) and our own port sources;
# a run that changes none of them (every app-debug iteration: only the ROM changes) reuses them. The ROM image
# object (xk_uimage.o) is rebuilt when romsect.bin is newer; link + ISO are skipped when nothing changed at all.
OSTAMP=$OUT/image-objs.stamp
OKEY="$DEFS|$FBDEF|$KKEY|snd${SOUND_TEST:-}"
OBJS_OK=0
if [ "${KCLEAN:-0}" != 1 ] && [ -f "$OSTAMP" ] && [ "$(cat "$OSTAMP")" = "$OKEY" ]    && [ -z "$(find "$PORT" "$EK" "$SRC/boot.S" "$SRC/grub.cfg" toolchain/kinc -newer "$OSTAMP" -type f 2>/dev/null | head -1)" ]; then
  echo "[3/5] boot stub + port objects: unchanged -- reused"; OBJS_OK=1
fi
if [ "$OBJS_OK" = 0 ]; then
rm -f "$OSTAMP"; echo "$OKEY" > "$OSTAMP.new"
gcc -mx32 -fno-pic $FBDEF -c "$SRC/boot.S" -o "$OUT/boot.o"
gcc -mx32 -fno-pic -c "$EK/xk_vectors.S" -o "$OUT/xk_vectors.o"
gcc -mx32 -fno-pic -c "$EK/xk_aptramp.S" -o "$OUT/xk_aptramp.o"
fi
# SECONDARY=uexe (default): the ROM secondary is a real euser-based EXE (uexe/make_uexe.sh, APP=...);
# SECONDARY=rom:     a PRELINKED ROM SECTION -- separate euser.dll + uhello.exe E32Images with real
#                    TRomImageHeader records and imports resolved by ordinal. Build it on the HOST
#                    first (the emitter is host Python), then pass SECONDARY=rom:
#                      py emitter/build_euser_x32.py
#                      IMPORTS=1 py emitter/build_uhello_x32.py
#                      py tools/rombuild.py --section 0x80400000 \
#                         --xip 'Sys\Bin\euser.dll=emitter/euser_x32.dll' \
#                         --xip 'Sys\Bin\uhello.exe=.phorepoc-x86-build/uexe/uhello.imp.exe' \
#                         .phorepoc-x86-build/rom/romsect.bin
# SECONDARY=minisec: the hand-rolled ring-3 asm program (xk_minisec.cpp) -- no euser involved.
case "${SECONDARY:-uexe}" in
uexe)
  bash "$SRC/uexe/make_uexe.sh" > "$OUT/uexe.log" 2>&1 || { tail -15 "$OUT/uexe.log"; exit 1; }
  sed "s/^/      /" "$OUT/uexe.log"
  gcc -mx32 -fno-pic -DUIMAGE_BIN='"/out/uexe/uexe.bin"' -c "$EK/xk_uimage.S" -o "$OUT/xk_uimage.o"
  ;;
rom)
  [ -f /out/rom/romsect.bin ] || { echo "SECONDARY=rom: /out/rom/romsect.bin missing -- build it on the host (see above)"; exit 1; }
  ls -l /out/rom/romsect.bin
  [ "$OBJS_OK" = 1 ] && [ -f "$OUT/xk_uimage.o" ] && [ ! /out/rom/romsect.bin -nt "$OUT/xk_uimage.o" ]     || gcc -mx32 -fno-pic -DUIMAGE_BIN='"/out/rom/romsect.bin"' -c "$EK/xk_uimage.S" -o "$OUT/xk_uimage.o"
  ;;
*)
  gcc -mx32 -fno-pic -c "$EK/xk_uimage.S" -o "$OUT/xk_uimage.o"
  ;;
esac
if [ "$OBJS_OK" = 0 ]; then
$CXX $DEFS $INCS $PRE -c "$EK/xboot.cpp"    -o "$OUT/xboot.o"
$CXX $DEFS $INCS $PRE -c "$EK/xk_entry.cpp" -o "$OUT/xk_entry.o"
$CXX $DEFS $INCS $PRE -c "$EK/xk_irq.cpp"   -o "$OUT/xk_irq.o"
$CXX $DEFS $INCS $PRE -c "$EK/xk_smp.cpp"   -o "$OUT/xk_smp.o"
# X4.21: device interrupts. The REAL IA-32 IOAPIC/local-APIC half of NIrq (nkernsmp/x86/ncirq.cpp,
# unmodified; overrides x64s_ncsched.cpp's weak stand-ins) + the board's Interrupt API over it.
# (its debug-build DbgMemDump collides with locmedia.cpp's: renamed.)
$CXX $DEFS $INCS $PRE -DDbgMemDump=DbgMemDump_ncirq -c "$EKA/nkernsmp/x86/ncirq.cpp" -o "$OUT/ncirq.o"
$CXX $DEFS $INCS $PRE -c "$EK/xk_interrupt.cpp" -o "$OUT/xk_interrupt.o"
# X4.17: the REAL local-media stack as built-in extensions, unmodified -- ELOCD (locmedia +
# dmasupport) and the internal RAM drive (medint/iram.cpp). Each is its own DLL in a real ROM, so
# their entry symbols collide; renamed at compile time (the exstart pattern), called by xk_media.cpp.
MED="-I $EKA/drivers/locmedia -I $EKA/drivers/locmedia/traces -I $EKA/include/drivers -I $EK"
$CXX $DEFS $INCS $MED $PRE -DInitExtension=InitExtension_locmedia -DKernelModuleEntry=KernelModuleEntry_locmedia \
     -DCreateLogicalDevice=CreateLogicalDevice_locmedia -c "$EKA/drivers/locmedia/locmedia.cpp" -o "$OUT/locmedia.o"
$CXX $DEFS $INCS $MED $PRE -c "$EKA/drivers/locmedia/dmasupport.cpp" -o "$OUT/dmasupport.o"
$CXX $DEFS $INCS $MED $PRE -DInitExtension=InitExtension_medint -DKernelModuleEntry=KernelModuleEntry_medint \
     -DCreatePhysicalDevice=CreatePhysicalDevice_medint -c "$EKA/drivers/medint/iram.cpp" -o "$OUT/iram.o"
$CXX $DEFS $INCS $MED $PRE -c "$EK/xk_media.cpp" -o "$OUT/xk_media.o"
# X4.22: the board's ATA (IDE PIO) hard disk, a second fixed medium behind the same ELOCD
$CXX $DEFS $INCS $MED $PRE -c "$EK/xk_ata.cpp" -o "$OUT/xk_ata.o"
# X4.18: the board's display HAL (VGA text) and PC keyboard (i8042). The keyboard reuses Syborg's
# PC-set-1 -> EStdKey table, cut verbatim out of syborg_keyboard.h (its initializer braces only).
KBDH=baseline/symbian-2010/oss.FCL.sf.adapt.Qemu/baseport/syborg/keyboard/syborg_keyboard.h
awk '/KConvertCode\[\] *=/{on=1;next} on&&/^ *};/{print "}";exit} on{print}' "$KBDH" > "$OUT/xk_kbdtable.inc"
[ "$(grep -c "EStdKey" "$OUT/xk_kbdtable.inc")" -gt 50 ] || { echo "keyboard table extraction failed"; exit 1; }
$CXX $DEFS $INCS $PRE -c "$EK/xk_display.cpp" -o "$OUT/xk_display.o"
$CXX $DEFS $INCS $PRE -c "$EK/xk_power.cpp" -o "$OUT/xk_power.o"
$CXX $DEFS $INCS $PRE -I "$OUT" -c "$EK/xk_keyboard.cpp" -o "$OUT/xk_keyboard.o"
$CXX $DEFS $INCS $PRE -c "$EK/xk_absmouse.cpp" -o "$OUT/xk_absmouse.o"
$CXX $DEFS $INCS $PRE -c "$EK/xk_timesync.cpp" -o "$OUT/xk_timesync.o"
# X4.47: the e1000 NIC (PDD) + the REAL Ethernet LDD (unmodified; only its entry point is renamed)
$CXX $DEFS $INCS $MED $PRE -DCreateLogicalDevice=CreateLogicalDevice_enet -c "$EKA/drivers/ethernet/d_ethernet.cpp" -o "$OUT/d_ethernet.o"
$CXX $DEFS $INCS $MED $PRE -c "$EK/xk_e1000.cpp" -o "$OUT/xk_e1000.o"
# X4.88: AC'97 audio (PDD) + the REAL shared-chunk sound LDD (unmodified; entry renamed). SOUND_TEST=1: a boot sine
$CXX $DEFS $INCS $MED $PRE -DCreateLogicalDevice=CreateLogicalDevice_soundsc -DKernelModuleEntry=KernelModuleEntry_soundsc -c "$EKA/drivers/soundsc/soundldd.cpp" -o "$OUT/soundldd.o"
$CXX $DEFS $INCS $MED $PRE ${SOUND_TEST:+-DSOUND_TEST} -c "$EK/xk_ac97.cpp" -o "$OUT/xk_ac97.o"
# camera (camera-plan.md C1): the 2010 shared-chunk camera LDD (DCameraScLdd, restored from kernelhwsrv BRANCH_RCL_3 into third_party/symbian2010-camerasc, unmodified, entry renamed)
# + the 2010 template PDD (a timer-driven fake sensor that alternates a YUV logo; unmodified) + our two built-in-device wrappers (xk_camerasc.cpp)
CAMD=third_party/symbian2010-camerasc
CAMP=$CAMD/bsptemplate/asspandvariant/template_variant/camerasc
CAMI="-I $CAMD/kernel/eka/include"
$CXX $DEFS $CAMI $INCS $MED $PRE -DCreateLogicalDevice=CreateLogicalDevice_camerasc -DKernelModuleEntry=KernelModuleEntry_camerasc -DDBufferManager=DCamBufferManager -c "$CAMD/kernel/eka/drivers/camerasc/cameraldd.cpp" -o "$OUT/cameraldd.o"
$CXX $DEFS $CAMI $INCS $MED -I $CAMP $PRE -DCreatePhysicalDevice=CreatePhysicalDevice_camerasc -DKernelModuleEntry=KernelModuleEntry_camerasc_pdd -c "$CAMP/camerasc.cpp" -o "$OUT/camerasc_pdd.o"
$CXX $DEFS $CAMI $INCS $MED -I $CAMP $PRE -c "$CAMP/camerasc_sensor.cpp" -o "$OUT/camerasc_sensor.o"
$CXX $DEFS $CAMI $INCS $MED $PRE -c "$EK/xk_camerasc.cpp" -o "$OUT/xk_camerasc.o"
# S0 (vm-gpu-options.md): the read-only SVGA II / SVGA3D capability probe
$CXX $DEFS $INCS $PRE -c "$EK/xk_vmwsvga.cpp" -o "$OUT/xk_vmwsvga.o"
# X4.27 debug aid: DUMP=<s> dumps every thread (state + symbolizable user stack) once, s seconds in
$CXX $DEFS $INCS $PRE -fno-access-control -c "$EK/xk_threaddump.cpp" -o "$OUT/xk_threaddump.o"
# the first user program: no kernel headers, no kernel symbols
$CXX -c "$EK/xk_minisec.cpp" -o "$OUT/xk_minisec.o"
# the REAL exstart extension (unmodified baseline TU), statically linked: only its entry point is renamed
$CXX $DEFS $INCS $PRE -DKernelModuleEntry=X64ExstartEntry -DInitExtension=X64ExstartInit \
     -c "$EKA/kernel/exstart.cpp" -o "$OUT/exstart.o"
mv "$OSTAMP.new" "$OSTAMP"
fi

LSTAMP=$OUT/image.stamp
LKEY="$OKEY|${SECONDARY:-uexe}"
if [ "${KCLEAN:-0}" != 1 ] && [ -f "$LSTAMP" ] && [ "$(cat "$LSTAMP")" = "$LKEY" ] && [ -f "$OUT/phorepoc-ekern.iso" ]    && [ ! "$KSTAMP" -nt "$LSTAMP" ] && [ ! "$OSTAMP" -nt "$LSTAMP" ] && [ ! "$OUT/xk_uimage.o" -nt "$LSTAMP" ]    && [ "${SECONDARY:-uexe}" != uexe ]; then
  echo "[4-5/5] kernel image + ISO: unchanged -- reused"
else
rm -f "$LSTAMP"
echo "[4/5] link at EKA2's native addresses"
NK=/out/nkernsmp
ld -m elf32_x86_64 -n -z max-page-size=0x1000 -T "$EK/ekern.lds" -Map "$OUT/ekern.map" -o "$OUT/ekern.elf32" \
   "$OUT/boot.o" "$OUT/xboot.o" "$OUT/xk_entry.o" "$OUT/xk_irq.o" "$OUT/xk_smp.o" "$OUT/ncirq.o" "$OUT/xk_interrupt.o" "$OUT/xk_aptramp.o" "$OUT/xk_minisec.o" "$OUT/xk_uimage.o" "$OUT/exstart.o" "$OUT/xk_vectors.o" \
   "$OUT/locmedia.o" "$OUT/dmasupport.o" "$OUT/iram.o" "$OUT/xk_media.o" "$OUT/xk_ata.o" "$OUT/xk_display.o" "$OUT/xk_keyboard.o" "$OUT/xk_absmouse.o" "$OUT/xk_timesync.o" "$OUT/d_ethernet.o" "$OUT/xk_e1000.o" "$OUT/soundldd.o" "$OUT/xk_ac97.o" "$OUT/xk_vmwsvga.o" "$OUT/cameraldd.o" "$OUT/camerasc_pdd.o" "$OUT/camerasc_sensor.o" "$OUT/xk_camerasc.o" "$OUT/xk_threaddump.o" "$OUT/xk_power.o" "$KOBJ"/*.o \
   $NK/nklib.o $NK/nkern.o $NK/nkerns.o $NK/sched.o $NK/dfcs.o $NK/nk_timer.o $NK/nk_irq.o $NK/nk_bal.o \
   $NK/x64s_atomics.o $NK/x64s_ncutils.o $NK/x64s_ncsched.o $NK/x64_ctxsw.o 2>"$OUT/ld.log" \
  || { grep -v "RWX\|GNU-stack\|deprecated" "$OUT/ld.log" | c++filt | head -30; exit 1; }
# GRUB's multiboot2 loader rejects ELF32/EM_X86_64: re-wrap the container (code unchanged).
objcopy -O elf64-x86-64 "$OUT/ekern.elf32" "$OUT/ekern.elf"
grub-file --is-x86-multiboot2 "$OUT/ekern.elf" || { echo "not a multiboot2 image" >&2; exit 1; }
size "$OUT/ekern.elf32" | tail -1

echo "[5/5] ISO"
rm -rf "$OUT/iso"; mkdir -p "$OUT/iso/boot/grub"
cp "$OUT/ekern.elf" "$OUT/iso/boot/phorepoc.elf"
cp "$SRC/grub.cfg" "$OUT/iso/boot/grub/grub.cfg"
grub-mkrescue -o "$OUT/phorepoc-ekern.iso" "$OUT/iso" 2>/dev/null
ls -l "$OUT/phorepoc-ekern.iso"
echo "$LKEY" > "$LSTAMP"
fi

if [ "${1:-}" = "run" ]; then
  echo "== qemu -smp ${CPUS:-2}, ${SECS:-15} s, serial capped =="
  # SHOT=1: also screendump the guest display to $OUT/screen.png after SHOT_WAIT s, typing KEYS
  # first (tools/phorepoc-x86/qmon.py -- the VGA text shell has no serial echo of its own)
  MON=""; [ "${SHOT:-0}" = 1 ] && { rm -f /tmp/qmon /tmp/qmon.qmp; MON="-monitor unix:/tmp/qmon,server,nowait -qmp unix:/tmp/qmon.qmp,server,nowait"; }
  # WATCH=1 (X4.53, app_regress.sh): instead of one timed shot, follow the serial log -- a screendump per
  # "[shot] <app>" into $OUT/shots/, and QEMU quits at "[cycle] end" (bootui's *secs|app|... line)
  if [ "${WATCH:-0}" = 1 ]; then
    rm -rf "$OUT/shots"; mkdir -p "$OUT/shots"; rm -f /tmp/qmon /tmp/qmon.qmp
    MON="-monitor unix:/tmp/qmon,server,nowait -qmp unix:/tmp/qmon.qmp,server,nowait"
    : > "$OUT/run.log"
    python3 "$SRC/qmon.py" /tmp/qmon "$OUT/shots/x.png" --watch "$OUT/run.log" &
    QPID=$!
  elif [ "${SHOT:-0}" = 1 ]; then
    # X4.68, event-driven runs: SHOT_ON="<serial text>" starts the KEYS/MONCMDS script when the log says so
    # (SHOT_WAIT is then only its timeout), MONCMDS may hold wait:<text>[@<s>] steps, and SHOT_QUIT=1 quits
    # QEMU after the final screendump -- a scripted run lasts as long as its script, not SECS
    [ -n "${SHOT_ON:-}" ] && : > "$OUT/run.log"
    python3 "$SRC/qmon.py" /tmp/qmon "$OUT/screen.png" --wait "${SHOT_WAIT:-20}" \
      --keys "${KEYS:-}" --mon "${MONCMDS:-}" ${BURST:+--burst} --after "${SHOT_AFTER:-3}" --fb "${QFB:-720x1280}" \
      --log "$OUT/run.log" ${SHOT_ON:+--start-on "$SHOT_ON"} $([ "${SHOT_QUIT:-0}" = 1 ] && echo --quit) &
    QPID=$!
  fi
  # RTC=<date> (e.g. 2026-12-01T12:00:00): start the guest CMOS clock there (DST checks, X4.41)
  # DISK=1: the test hard disk (tools/phorepoc-x86/make_disk.sh) as the primary IDE master -> D: (X4.22)
  DRIVE=""; [ "${DISK:-0}" = 1 ] && DRIVE="-drive file=${DISKIMG:-/out/disk/phorepoc-disk.img},format=raw,if=ide,index=0"
  # CARD=1 (X4.111): the memory card = a second disk on the primary IDE SLAVE (index 1; the ISO is index 2, secondary master).
  # Symbian sees a removable drive, F: (make_disk.sh: IMG=/out/disk/phorepoc-card.img LABEL="MEMORY CARD" bash make_disk.sh 64)
  [ "${CARD:-0}" = 1 ] && DRIVE="$DRIVE -drive file=${CARDIMG:-/out/disk/phorepoc-card.img},format=raw,if=ide,index=1"
  # NETOPTS=<slirp options> (X4.49): the NIC spelled out (QEMU's default is this same e1000 on user
  # networking) with extra options, e.g. NETOPTS=dhcpstart=10.0.2.77 to see a DHCP lease differ from
  # the static IAP's 10.0.2.15
  NIC=""; [ -n "${NETOPTS:-}" ] && NIC="-nic user,model=e1000,$NETOPTS"
  # PCAP=1 (X4.68): the same NIC as an explicit netdev + a filter-dump of every frame -> $OUT/net.pcap
  # (tcpdump -nr it in the container: DNS, TCP, what an app actually sent)
  [ "${PCAP:-0}" = 1 ] && NIC="-netdev user,id=n0${NETOPTS:+,$NETOPTS} -device e1000,netdev=n0 -object filter-dump,id=f0,netdev=n0,file=$OUT/net.pcap"
  # X4.88: AC'97 audio (xk_ac97.cpp). SOUND_WAV=1 captures the output to $OUT/sound.wav (headless: the host checks
  # it); otherwise a silent backend -- the device is still there, drivers and DevSound run
  AUDIO="-audiodev none,id=snd0 -device AC97,audiodev=snd0"
  [ "${SOUND_WAV:-0}" = 1 ] && AUDIO="-audiodev wav,id=snd0,path=$OUT/sound.wav -device AC97,audiodev=snd0"
  # QUIT_ON="<serial text>" (2026-10-13): QEMU stops a few seconds after the log shows it (e.g. '[js] exit(' when the test
  # exe ends) -- the run lasts as long as the test, not SECS (phwebtest: ~5 min of a 7m42s boot was idle). Only the log's
  # tail is read, once a second.
  if [ -n "${QUIT_ON:-}" ]; then
    : > "$OUT/run.log"
    ( while sleep 1; do
        if tail -c 65536 "$OUT/run.log" 2>/dev/null | grep -a -q -F -- "$QUIT_ON"; then
          sleep "${QUIT_GRACE:-3}"; pkill -f "qemu-system-x86_64 -smp" ; break
        fi
        pgrep -f "qemu-system-x86_64 -smp" >/dev/null || { [ -s "$OUT/run.log" ] && break; }
      done ) &
    QWATCH=$!
  fi
  timeout ${SECS:-15} qemu-system-x86_64 -smp ${CPUS:-2} -cdrom "$OUT/phorepoc-ekern.iso" -serial stdio -display none \
      $DRIVE $NIC $AUDIO ${RTC:+-rtc base=$RTC} -boot d $MON -no-reboot -m ${MEM:-128} 2>&1 | stdbuf -oL head -c ${LOGMAX:-400000} > "$OUT/run.log" || true
  # QEMU gone (timeout, crash, or the watcher's own quit): the watcher must not outlive it
  [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null || true
  [ -n "${QWATCH:-}" ] && kill "$QWATCH" 2>/dev/null || true
  wait
  echo "($(wc -l < "$OUT/run.log") lines in $OUT/run.log)"; tail -40 "$OUT/run.log" | cut -c1-200
fi
