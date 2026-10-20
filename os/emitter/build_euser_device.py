"""build_euser_device -- the DEVICE euser DLL (Track B / M7).

The sibling of emitter/build_euser_dll.py (the EKA2L1-BENCH euser). Same
ELF->E32Image pipeline and the same real-from-source TUs, but built to run on
OUR from-source kernel (QEMU/Syborg) instead of the EKA2L1 HLE bench. The one
architectural difference is the executive seam (see docs/track-b/efile-link.md
"device vs bench euser"):

    bench euser  = real-source TUs + toolchain/euser_dll/{sched,sync,timer,...}_env
                   seams that hand-bind the bench's HLE svc numbers
    DEVICE euser = the SAME real-source TUs
                   + toolchain/euser_dll/dev_user_execs.cpp  (the generated
                     user-side executive, compiled -D__GEN_USER_EXEC_CODE__, so
                     Exec::* / E32Loader::* / UserSvr::* emit the DEVICE svc
                     numbers our kernel implements)
                   - every bench HLE exec seam and every *_selftest driver.

Why not just add dev_user_execs.cpp to the bench build: the bench seams bind
DIFFERENT (v10 HLE) svc numbers for the same Exec:: leaves, so the two exec
layers conflict. This target takes the real-source TUs and swaps the seam.

Env adapters that are pure value/algorithm (not svc-number-bound) are kept:
  - hash_env.cpp    DefaultHash::Integer + Mem::Swap (live only in uc_utl.cia)
  - desc_compat.cpp .cia mangled-name forwarders for the descriptor asm
Dropped for the device build (bench-HLE-svc-bound or test drivers):
  - sched_env / sync_env / timer_env  -> replaced by dev_user_execs.cpp
  - euser_heap / trap_env / locale_env / unicode_env  -> the real-source TUs
    (us_exec/us_ksvr/us_property/ub_cln/...) already define these; their Exec::
    leaves now resolve to the device exec layer, not a bench seam
  - every *_selftest.cpp / cobject_mc.cpp  -> bench harness drivers, not library

This is step 1 of the M7 device-euser stand-up. Symbols the included real
source does not yet cover (device run-env leaves, e.g. the process-bootstrap
heap install) fall to the same unimpl stub the bench build uses, and are
reported. Nothing on the link surface is silently dropped.

Run from repo root:  py emitter/build_euser_device.py [out.dll]
"""
from __future__ import annotations

import os
import sys

import build_euser_dll as BE
from e32image import build_dll
from elf import extract
from euser_def import parse_def
from refcheck import check_image, format_result

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
KINC = os.path.join(REPO, "toolchain", "kinc")

# The device exec layer: the generated user-side executive (all 281 Exec::/
# E32Loader::/UserSvr:: stub bodies, device svc numbers). Proven to compile
# 2026-09-18; see toolchain/euser_dll/dev_user_execs.cpp.
DEV_EXECS = os.path.join(BE.ED, "dev_user_execs.cpp")

# Env adapters kept for the device build (pure value/algorithm; not svc-bound).
KEEP_ED = {"hash_env.cpp", "desc_compat.cpp", "unimpl_export.cpp"}

# Device-only REAL-source TUs (baseline) that the bench build gets from HLE seams
# it drops here, closing the residual euser leaves EFILE imports (see the M7 gate
# in docs/track-b/efile-link.md). Each is genuine 2010 source; the Exec:: leaves
# they reach resolve to the DEVICE exec layer (dev_user_execs.cpp), not a bench svc.
#   uc_trp.cia   + us_trp.cpp  — the REAL trap harness (User::Leave family, TTrap).
#                  Non-throw path: __LEAVE_EQUALS_THROW__ is undefined in our build
#                  (we compile -fno-exceptions), so uc_trp.cia's setjmp-style
#                  context-save Leave/Trap is selected -- exactly the device asm,
#                  replacing the bench trap_env.cia seam.
#   uc_huffman.cia — TBitInput::ReadL/HuffmanL (machine-coded; the C++ bodies in
#                  us_decode.cpp are #ifdef'd out under __HUFFMAN_MACHINE_CODED__).
#   us_encode.cpp  — Huffman::IsValid.
#   us_hand.cpp    — TFindHandleBase (+ handle-name find wrappers).
#   us_loc.cpp     — TLocaleMessageText (locale message accessor).
#   common/alloc.cpp — RAllocator::MaxLength (+ RAllocator base).
#   uc_exec.cia    — RThread::RequestSignal/RequestComplete (+ machine-coded exec
#                  wrappers; the big hand-ARM grab-bag -> single .text section).
#   uc_utl.cia     — RFastLock::Wait()/Signal() (+ util grab-bag; single .text).
_EU = os.path.join(BE.B, "euser")
DEVICE_EXTRA_SOURCES = [
    os.path.join(_EU, "epoc", "arm", "uc_trp.cia"),
    os.path.join(_EU, "us_trp.cpp"),
    os.path.join(_EU, "epoc", "arm", "uc_huffman.cia"),
    os.path.join(_EU, "us_encode.cpp"),
    os.path.join(_EU, "us_hand.cpp"),
    os.path.join(_EU, "us_loc.cpp"),
    os.path.join(BE.B, "common", "alloc.cpp"),
    # us_shbuf.cpp — RShPool / RShBuf / TShPoolCreateInfo (2026-10-07): the comms framework's buffer pond
    # (mbufmgr, CommsFW::RCFThread) reaches them from every comms thread's entry; stubbed, c32exe died
    # EUSRNIMP in RCFThreadFunction on the PinePhone and SSM's fail policy shut the phone down.
    os.path.join(_EU, "us_shbuf.cpp"),
    # us_utl.cpp — the real Panic(TCdtPanic) ("USER" category; the descriptor/
    # container check panics route here) + PanicTFixedArray. Was a link stub, so
    # a legit USER panic died as EUSRNIMP with the code lost (seen at EFILE
    # bring-up: TDesC16::Mid's range check). uc_trp.cpp — PanicNoTrapFrame +
    # trap-harness C++ leaves.
    os.path.join(_EU, "us_utl.cpp"),
    os.path.join(_EU, "epoc", "arm", "uc_trp.cpp"),
    os.path.join(_EU, "epoc", "arm", "uc_exec.cia"),
    os.path.join(_EU, "epoc", "arm", "uc_utl.cia"),
    # us_graph.cpp — TRect/TPoint/TSize geometry. EDISP's CScreenDriverTemplate
    # ::Init calls TRect::TRect(const TSize&); it was an unimpl stub, so EWSRV
    # died EUSRNIMP (reason = caller LR 0x...6c0 inside edisp Init, boot41).
    os.path.join(_EU, "us_graph.cpp"),
    # ub_cons.cpp — CConsoleBase + Console::NewL (RLibrary-loads econs.dll,
    # NewConsole @1). ESHELL's CShell::NewL died EUSRNIMP on it (boot44).
    os.path.join(_EU, "cbase", "ub_cons.cpp"),
    # us_evnt.cpp — TRawEvent accessors (Pos/ScanCode/Modifiers...). EWSRV's
    # keyboard path (CWsEventHandler -> CKeyTranslator) reads raw events through
    # these; tools/romgate.py flagged them as EWSRV's last stubbed imports.
    os.path.join(_EU, "us_evnt.cpp"),
    # ub_dtim.cpp — CDeltaTimer (2026-10-06, the Phorepoc ROM on the PinePhone: c32exe died EUSRNIMP in
    # CMBufManager::ConstructL -> CDeltaTimer::NewL); us_ref.cpp — RRefBase (Alloc/Copy/Free).
    # EUSER_LIST_UNIMPL=1 lists the exports still stubbed.
    os.path.join(_EU, "cbase", "ub_dtim.cpp"),
    os.path.join(_EU, "cbase", "ub_tque.cpp"),   # TTickCountQue, CDeltaTimer's queue
    os.path.join(_EU, "us_ref.cpp"),
    os.path.join(_EU, "locmapping.cpp"),   # LocaleMapping: the distinct-locale-model language -> 3-DLL table
    # cxx_rt_dev.cpp — sized operator delete + __cxa_pure_virtual (ours; modern
    # g++ runtime leaves). Were link stubs: every deleting dtor died EUSRNIMP.
    os.path.join(BE.ED, "cxx_rt_dev.cpp"),
    # The graphical stack's euser gaps (tools/romgate.py --gui): CPolicyServer
    # (fbserv + wserv are policy servers), RMsgQueueBase (ws32/wserv event
    # queues), Power (wserv's power-event handling).
    os.path.join(_EU, "cbase", "ub_polsvr.cpp"),
    os.path.join(_EU, "us_mqueue.cpp"),
    os.path.join(_EU, "us_power.cpp"),
]
# All device extras are FILL-INS: any global they define that an earlier (proven)
# object already owns is localized, so only their fresh leaves go global and the
# link stays collision-free (same discipline as the bench M7 fill-ins).
DEVICE_FILLIN = BE.FILLIN_BASENAMES + [os.path.basename(s) for s in DEVICE_EXTRA_SOURCES]
# Exports appended PAST the real euseru.def table (the euser_selftest pattern):
# our -fno-exceptions build makes the TRAP macro reference the setjmp-style
# TTrap::Trap/UnTrap entry points, which euseru.def does not list (an RVCT
# __LEAVE_EQUALS_THROW__ build has no such imports). EFILE.EXE imports them by
# ordinal, so they get spare ordinals here: selftest = max_ordinal+1, then these
# in list order (max+2, max+3). build_efile_exe.py resolves against this list.
EXTRA_EXPORTS = ["_ZN5TTrap4TrapERi", "_ZN5TTrap6UnTrapEv"]


def extra_export_ordinal(name: str, max_ordinal: int) -> int:
    """Ordinal of an EXTRA_EXPORTS name (selftest occupies max_ordinal+1)."""
    return max_ordinal + 2 + EXTRA_EXPORTS.index(name)


# The two big hand-ARM grab-bags branch between their own functions with PC-relative
# b/bl, which -ffunction-sections would split across sections (GAS: "symbol is in a
# different section"); keep each in one .text (reached as a unit, nothing lost to GC).
_SINGLE_SECTION = {"uc_exec.cia", "uc_utl.cia"}


def _device_sources() -> list[str]:
    """DEVICE_SOURCES = bench build's real-source (baseline) TUs + the kept env
    adapters + the device-only real TUs + dev_user_execs.cpp. The baseline set is
    derived from BE.SOURCES so any real-source TU added to the bench build is
    automatically picked up here too."""
    real_src = [s for s in BE.SOURCES if s.startswith(BE.B + os.sep)]
    kept_ed = [os.path.join(BE.ED, n) for n in
               ("hash_env.cpp", "desc_compat.cpp", "unimpl_export.cpp", "dev_markcleanup.cpp")]
    # dev_user_execs.cpp goes LAST so its device Exec::* win nothing by accident;
    # it defines symbols no real-source TU defines (the generated executive).
    return real_src + DEVICE_EXTRA_SOURCES + kept_ed + [DEV_EXECS]


def _extra_flags(basename: str) -> list[str]:
    # SYMBIAN_DISTINCT_LOCALE_MODEL (2026-10-06, as Path C X4.72): variant-wide on S^3 -- without it LoadLocale
    # is the old single-DLL model (a KErrNotSupported stub against the 3-DLL elocl_* the SSM ROM carries)
    return _extra_flags_tu(basename) + ["-DSYMBIAN_DISTINCT_LOCALE_MODEL"]


def _extra_flags_tu(basename: str) -> list[str]:
    """Per-TU extra compiler args for the device build.
    dev_user_execs.cpp needs the generated-exec macro + the kinc header shim
    (u32exec.h -> exec_user.h bodies); the big grab-bag .cia files need a single
    .text section. Everything else compiles as the bench does."""
    if basename == "dev_user_execs.cpp":
        return ["-D__GEN_USER_EXEC_CODE__", f"-I{KINC}"]
    if basename == "heap_hybrid.cpp":	# the overlay copy keeps finding its "..." headers
        return ["-iquote", os.path.join(BE.B, "common")]
    flags = ["-fno-function-sections", "-fno-data-sections"] if basename in _SINGLE_SECTION else []
    # 2026-10-07: EVERY .cia, not just atomics.cia -- the ARMv5 bodies of the fast locks (RFastLock /
    # RCriticalSection Wait/Signal) use SWP with a transient "1 = mid-update" marker and a 1 ms sleep-retry;
    # SWP is UNDEFINED on the A53 (kernel-emulated, ~15k traps/s) and a stranded marker spins every waiter
    # forever (the PinePhone froze after the task switcher closed). ARMV7 = the LDREX/STREX bodies.
    if basename.lower().endswith(".cia"):
        # DEVICE atomics must match the DEVICE kernel's CPU config (Cortex-A8,
        # link_ekern.sh EXTRA): with the symcc default (__MARM_ARMV5__),
        # cpudefs.h selects the kernel-ASSISTED atomics (atomic_*_exec.h ->
        # FAST_EXEC1(EFastExecFastAtomic*)) -- and our v7 kernel's fast exec
        # table has NO such entries (all InvalidFastExec) => the first
        # __e32_atomic_* call dies KERN-EXEC 27 (EInvalidSystemCall; seen at
        # EFILE bring-up, swi#0x21 = fast 33 FastAtomicTas32 with bit23<<9
        # wrapped out). ARMV7 selects the real LDREX/STREX bodies, as a real
        # Cortex euser build does.
        return flags + ["-march=armv7-a", "-mno-unaligned-access",
                "-U__MARM_ARMV5__", "-D__MARM_ARMV7__", "-D__CPU_CORTEX_A8N__"]
    return flags


DEVICE_SOURCES = _device_sources()

# HEAP FAIL LOG (2026-10-07, PinePhone): S60 showed "out of memory" with 790 MB RAM free and the kernel heap fine --
# a USER heap at its max fails inside the process with no syscall, invisible to the kernel's [oom] probes. The
# REAL heap_hybrid.cpp is copied to .mmp-build/_euser_overlay/ with one line after the user-side Alloc's Unlock():
# a failed allocation RDebug-prints the process, the request and the heap's size/max (board log with A64_RDEBUG_LOG).
# HEAP_FAIL_LOG=0 = the unmodified source.
def _heap_overlay() -> None:
    if os.environ.get("HEAP_FAIL_LOG", "1") == "0":
        return
    src = os.path.join(BE.B, "common", "heap_hybrid.cpp")
    i = DEVICE_SOURCES.index(src)
    text = open(src, encoding="latin-1").read()
    anchor = "\t\t__INCREMENT_COUNTERS(addr);\n\t\t}\n\tUnlock();\n\t\n#ifdef ENABLE_BTRACE"
    assert text.count(anchor) == 1, "heap_hybrid.cpp: user-side Alloc anchor not found"
    hook = ('\t\t__INCREMENT_COUNTERS(addr);\n\t\t}\n\tUnlock();\n'
            '\tif (!addr) { TName n = RProcess().Name(); RDebug::Printf("[heapfail] %S size=%d heap=%d max=%d", '
            '&n, aSize, iChunkSize, iMaxLength); }\n\t\n#ifdef ENABLE_BTRACE')
    text = text.replace(anchor, hook).replace('#include "heap_hybrid.h"', '#include "heap_hybrid.h"\n#include <e32debug.h>', 1)
    d = os.path.join(REPO, ".mmp-build", "_euser_overlay")
    os.makedirs(d, exist_ok=True)
    out = os.path.join(d, "heap_hybrid.cpp")
    with open(out, "w", encoding="latin-1", newline="\n") as f:
        f.write(text)
    DEVICE_SOURCES[i] = out
_heap_overlay()

# LEAVE_TRACE=1: opt-in bring-up diagnostic -- log every User::Leave system-wide
# (toolchain/euser_dll/leave_trace_dev.cpp). Off by default: a normal build is
# byte-for-byte the plain from-source euser.
LEAVE_SYM = "_ZN4User5LeaveEi"
LEAVE_TRACE = os.environ.get("LEAVE_TRACE") == "1"
# User::Leave is ALWAYS wrapped (2026-10-16): toolchain/euser_dll/dev_markcleanup.cpp's __wrap_ routes a leave whose innermost
# trap frame is an SDK-style TRAP marker (try/catch model) to a C++ throw, any other to the real setjmp Leave. LEAVE_TRACE adds
# LeaveIfError's tracing wrapper (leave_trace_dev.cpp no longer wraps Leave itself).
INVARIANT_SYM = "_ZN4User5PanicERK7TDesC16i"
INVARIANT2_SYM = "_ZN4User9InvariantEv"      # bring-up: dev_markcleanup.cpp's __wrap_ logs the caller + a stack scan
LEAVE_WRAPPED = ((LEAVE_SYM, "_ZN4User12LeaveIfErrorEi") if LEAVE_TRACE else (LEAVE_SYM,)) + (INVARIANT_SYM, INVARIANT2_SYM)
def _set_ld_wraps() -> None:
    """The --wrap flags belong to euser's OWN link only. They used to be set at import time, and devlink imports this
    module, so every other device image linked through build_euser_dll._link (elocal.fsy, estart, ekdata, eshell)
    got `__wrap_User::Leave` references nothing defines: a link stub -> EUSRNIMP in CFatMountCB::FindVolumeLabelFileL
    on the first Leave (clean minigui build, 2026-10-18)."""
    os.environ["EUSER_LD_EXTRA"] = " ".join(f"-Wl,--wrap={s_} -Wl,-u,__wrap_{s_}" for s_ in (LEAVE_SYM, INVARIANT_SYM, INVARIANT2_SYM))
    if LEAVE_TRACE:
        os.environ["EUSER_LD_EXTRA"] = " ".join(
            f"-Wl,--wrap={s_} -Wl,-u,__wrap_{s_}" for s_ in LEAVE_WRAPPED)


if LEAVE_TRACE:
    _lt = os.path.join(BE.ED, "leave_trace_dev.cpp")
    DEVICE_SOURCES = DEVICE_SOURCES[:-1] + [_lt] + DEVICE_SOURCES[-1:]
    DEVICE_FILLIN = DEVICE_FILLIN + [os.path.basename(_lt)]


def main() -> int:
    _set_ld_wraps()
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "euser_dev.dll")
    workdir = os.environ.get("TEMP", HERE)

    defmap = parse_def(BE.DEF_PATH)
    elf_blob, exportable, stubbed = BE.compile_and_link(
        workdir, sources=DEVICE_SOURCES, fillin=DEVICE_FILLIN,
        extra_flags=_extra_flags)
    if os.environ.get("EUSER_LIST_UNIMPL") == "1":
        for n in sorted(stubbed):
            print(f"LINKSTUB {n}")
    ext = extract(elf_blob)

    if ext.code_base != BE.CODE_BASE:
        sys.exit(f"code_base mismatch: 0x{ext.code_base:X} != 0x{BE.CODE_BASE:X}")
    for name in (BE.SELFTEST_NAME, BE.STUB_NAME):
        if name not in ext.symbols:
            sys.exit(f"required symbol {name!r} not in ELF")

    exportable_set = set(exportable)
    stub_off = ext.symbols[BE.STUB_NAME] - ext.code_base
    export_offsets = []
    real = 0
    for ordn in range(1, defmap.max_ordinal + 1):
        name = defmap.name(ordn)
        if name in exportable_set and name in ext.symbols:
            if name in LEAVE_WRAPPED and ("__wrap_" + name) in ext.symbols:
                export_offsets.append(ext.symbols["__wrap_" + name] - ext.code_base)
                print(f"LEAVE_TRACE: {name} export -> tracing wrapper")
                real += 1
                continue
            export_offsets.append(ext.symbols[name] - ext.code_base)
            real += 1
        else:
            export_offsets.append(stub_off)
            if os.environ.get("EUSER_LIST_UNIMPL") == "1":   # which exports are still unimplemented stubs
                print(f"UNIMPL {ordn} {name}")
    selftest_ord = defmap.max_ordinal + 1
    export_offsets.append(ext.symbols[BE.SELFTEST_NAME] - ext.code_base)
    # TTrap::Trap/UnTrap at spare ordinals past the selftest (see EXTRA_EXPORTS).
    for name in EXTRA_EXPORTS:
        if name not in ext.symbols:
            sys.exit(f"EXTRA_EXPORTS symbol {name!r} not in ELF")
        export_offsets.append(ext.symbols[name] - ext.code_base)

    print("DEVICE euser (dev_user_execs.cpp exec layer; bench HLE seams dropped)")
    print(f"code: {len(ext.code)} B @0x{ext.code_base:X}   "
          f"data: {len(ext.data)} B   bss: {ext.bss_size} B")
    print(f"exports: {len(export_offsets)} ordinals  "
          f"({real}/{defmap.max_ordinal} real from source, "
          f"{defmap.max_ordinal - real} -> unimpl stub); "
          f"{len(stubbed)} link stubs; {BE.SELFTEST_NAME} @ {selftest_ord}; "
          + ", ".join(f"{n} @ {extra_export_ordinal(n, defmap.max_ordinal)}"
                      for n in EXTRA_EXPORTS))
    print(f"code relocs: {len(ext.code_relocs)}   data relocs: {len(ext.data_relocs)}")

    # euser.mmh: `uid 0x1000008d 0x100039e5`. SDK-built apps import euser{000a0000}[100039e5].dll and the loader matches that
    # UID3: with the old placeholder (0x20005001) every 3rd-party app was killed at load (X-plore, 2026-10-12)
    img = build_dll(ext.code, export_offsets, uid2=0x1000008D, uid3=0x100039E5,
                    code_relocs=ext.code_relocs, code_base=ext.code_base,
                    data=ext.data, data_base=ext.data_base, bss_size=ext.bss_size,
                    data_relocs=ext.data_relocs,
                    code_reloc_types=ext.code_reloc_types,
                    data_reloc_types=ext.data_reloc_types)
    with open(out_path, "wb") as fh:
        fh.write(img)
    print(f"\nWrote {out_path} ({len(img)} bytes)")
    result = check_image(img)
    print(format_result(result))
    return 0 if result.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
