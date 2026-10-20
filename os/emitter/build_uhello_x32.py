"""build_uhello_x32 -- the EXE side of Path C's real images (amd64 CPU, x32 ABI).

Two modes, both producing an E32Image EXE from `uexe/uhello.cpp` + the eexe entry stub:

  static (default)  X4.9a. uhello + the WHOLE euser linked into one image. This was the first
                    x32 E32Image ever emitted and stays as the regression for the plain
                    ELF -> E32Image path: no imports, no ordinals, nothing to resolve.
  IMPORTS=1         X4.9b/c. uhello + the entry stub ONLY, with every euser reference going
                    through an import veneer bound BY ORDINAL to euser.dll -- the real platform
                    arrangement, and what rombuild/the loader will prelink. Run
                    `py emitter/build_euser_x32.py` first; this reads its export sidecar.

    py emitter/build_uhello_x32.py              (run make_uexe.sh or make_ekern.sh first)
    IMPORTS=1 py emitter/build_uhello_x32.py

The x32 toolchain lives in the phorepoc-x86 container, so the links and the assembler run there;
the emitter -- elf.extract, e32image.build_dll, the self-checks -- runs here on the host.

Self-checks. Static: every relocated word points inside the image. Imports: additionally every
import word in the code holds `ordinal | adj<<16` as the loader expects to find it, every ordinal
is inside euser's export table, and the ones landing on euser's unimpl stub are NAMED -- those
are the calls that would panic EUSRNIMP at run time, i.e. exactly how much of uhello's euser
surface is not yet from source.
"""
from __future__ import annotations

import json
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import build_euser_x32 as EU                  # noqa: E402
import devlink                                # noqa: E402
import e32image                               # noqa: E402
import elf                                    # noqa: E402
from euser_def import parse_def               # noqa: E402

BUILD = os.path.join(REPO, ".phorepoc-x86-build")
UEXE = os.path.join(BUILD, "uexe")
EUSER_DLL = "euser.dll"


def _docker(script: str) -> subprocess.CompletedProcess:
    repo_w = REPO.replace("\\", "/")
    cmd = ["docker.exe", "run", "--rm",
           "-v", f"{repo_w}:/repo", "-v", f"{repo_w}/.phorepoc-x86-build:/out",
           "phorepoc-x86", "bash", "-c", script]
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    return subprocess.run(cmd, check=True, env=env, capture_output=True, text=True)


def euser_ordinal_map() -> dict[str, int]:
    """symbol -> euser ordinal, in the amd64 spelling. The frozen eabi/euseru.def table (with
    the va_list alias) plus the spare ordinals past it, laid out exactly as the ARM device build
    lays them out -- so devlink's cross-architecture ordinal arithmetic stays one rule."""
    defmap = parse_def(EU.DEF_PATH)
    resolve = {EU.x86_name(name): ordn for ordn, name in defmap.by_ordinal.items()}
    for i, name in enumerate(EU.EXTRA_EXPORTS):
        resolve[name] = defmap.max_ordinal + 2 + i
    return resolve


# --------------------------------------------------------------------------------------------
# static mode (X4.9a)
# --------------------------------------------------------------------------------------------

def static_link() -> str:
    """ld -q inside the container (that is where the x32 binutils live)."""
    _docker("cd /out/uexe && ld -m elf32_x86_64 -n -q -T /repo/emitter/x32_exe.ld "
            "-o uhello.e32.elf uexe.rel.o todo_stubs.o 2>&1 | grep -v 'RWX\\|deprecated' ; true")
    return os.path.join(UEXE, "uhello.e32.elf")


# --------------------------------------------------------------------------------------------
# import mode (X4.9c)
# --------------------------------------------------------------------------------------------

def import_link() -> tuple[str, dict[str, int], list[str]]:
    """Link uhello + the entry stub ALONE, routing every euser reference through a veneer.
    Returns (elf path, name -> ordinal for the veneers we emitted, names with no ordinal)."""
    # gcchelp_local.o = this image's own operator new/delete. euseru.def exports none of them;
    # a real image statically links scppnwdl.lib (targettype lib) for exactly this reason, so
    # the import-linked EXE carries its own copy and reaches the allocator through euser's
    # exported User:: entry points like everything else.
    objs = "xu_exe.o uhello.o gcchelp_local.o"
    _docker(f"cd /out/uexe && ld -m elf32_x86_64 -n -q --gc-sections --no-demangle "
            f"--unresolved-symbols=ignore-all -u _E32Startup -T /repo/emitter/x32_exe.ld "
            f"-o uhello.imp1.elf {objs}")
    r = _docker("nm --undefined-only /out/uexe/uhello.imp1.elf")
    missing = sorted({l.split()[-1] for l in r.stdout.splitlines() if l.split()})

    resolve = euser_ordinal_map()
    imports = {n: resolve[n] for n in missing if n in resolve}
    unresolved = [n for n in missing if n not in resolve]

    src = devlink.make_veneers_x32(UEXE, imports.keys(), "uhello")
    _docker(f"cd /out/uexe && gcc -mx32 -c {os.path.basename(src)} -o uhello_veneers.o")
    _docker(f"cd /out/uexe && ld -m elf32_x86_64 -n -q --gc-sections --no-demangle "
            f"-u _E32Startup -T /repo/emitter/x32_exe.ld "
            f"-o uhello.imp.elf {objs} uhello_veneers.o")
    return os.path.join(UEXE, "uhello.imp.elf"), imports, unresolved


def check_imports(image: bytes, ext, import_blocks, imports: dict[str, int]):
    """Confirm the emitted image says what the loader will read, and report which imports land
    on euser's unimplemented-export stub."""
    problems: list[str] = []
    with open(EU.EXPORT_MAP) as fh:
        emap = json.load(fh)
    by_ordinal = {int(k): v for k, v in emap["ordinals"].items()}

    stubbed: list[str] = []
    for name, ordn in sorted(imports.items()):
        if name not in ext.symbols:
            continue                                    # veneer collected by --gc-sections
        if not 1 <= ordn <= emap["count"]:
            problems.append(f"{name}: ordinal {ordn} outside euser's {emap['count']} exports")
            continue
        if by_ordinal[ordn][1] == emap["stub_offset"]:
            stubbed.append(f"{name} @ {ordn}")
    # Read the words out of the FINISHED IMAGE, not out of ext.code: build_dll stamps the
    # ordinals into its own copy of the code, which is the copy the loader will see.
    code_offset = struct.unpack_from("<I", image, 100)[0]
    for _dll, entries in import_blocks:
        for (off, ordn, adj) in entries:
            word = struct.unpack_from("<I", image, code_offset + off)[0]
            if word != (ordn | (adj << 16)):
                problems.append(f"import word at +0x{off:x} is 0x{word:08x}, "
                                f"expected 0x{ordn | (adj << 16):08x}")
    # Landing on the stub is a finding, not a malformed image: it names the euser exports uhello
    # would call that we do not build from source yet (they panic EUSRNIMP if reached).
    return len(stubbed), problems, stubbed


def main() -> int:
    use_imports = os.environ.get("IMPORTS") == "1"
    if not os.path.exists(os.path.join(UEXE, "uexe.rel.o")):
        print("run tools/phorepoc-x86/uexe/make_uexe.sh first (uexe.rel.o missing)")
        return 1

    imports: dict[str, int] = {}
    unresolved: list[str] = []
    if use_imports:
        if not os.path.exists(EU.EXPORT_MAP):
            print("run py emitter/build_euser_x32.py first (euser export map missing)")
            return 1
        path, imports, unresolved = import_link()
    else:
        path = static_link()

    ext = elf.extract(open(path, "rb").read())
    entry = ext.symbols["_E32Startup"] - ext.code_base
    blocks = (devlink.import_blocks(ext, {n: (EUSER_DLL, o) for n, o in imports.items()},
                                    dll_order=(EUSER_DLL,), arch="x32")
              if use_imports else None)
    image = e32image.build_dll(
        ext.code, [], exe=True, cpu=e32image.CPU_X86,
        uid2=0, uid3=0x20FFFF01, entry_point=entry, imports=blocks,
        code_relocs=ext.code_relocs, code_reloc_types=ext.code_reloc_types,
        data=ext.data, data_base=ext.data_base, bss_size=ext.bss_size,
        data_relocs=ext.data_relocs, data_reloc_types=ext.data_reloc_types,
        code_base=ext.code_base, cap1=0xFFFFFFFF, cap2=0xFFFFFFFF,
        stack_size=0x4000, heap_min=0x1000, heap_max=0x100000)
    out = os.path.join(UEXE, "uhello.imp.exe" if use_imports else "uhello.exe")
    with open(out, "wb") as fh:
        fh.write(image)
    e32image.write_pcrel_sidecar(out, ext)

    # --- self-checks ---------------------------------------------------------------------
    cpu = struct.unpack_from("<H", image, 122)[0]
    bad, folded = EU.check_relocs(ext)
    # An EXE's entry header is uc_exe.cia's: the same `jmp +16` over 16 bytes as a DLL's, but
    # with a ZERO word at +12 (the loader's code-segment-id slot) instead of UID3.
    entry_problems = EU.check_entry_header(ext.code, entry, uid3=0)
    print(f"{os.path.basename(out)}: {len(image)} bytes, cpu 0x{cpu:04x}, "
          f"code {len(ext.code)} @0x{ext.code_base:x}, data {len(ext.data)} "
          f"bss {ext.bss_size}, entry +0x{entry:x} ({'header OK' if not entry_problems else 'HEADER BAD'})")
    print(f"relocs: {len(ext.code_relocs)} code, {len(ext.data_relocs)} data; "
          f"{bad} unclassified ({folded} with a folded addend)")
    for p in entry_problems:
        print("  ! " + p)

    ok = (cpu == e32image.CPU_X86 and bad == 0 and not entry_problems
          and image[:4] == struct.pack("<I", e32image.UID1_EXE))
    if use_imports:
        bound = sum(len(e) for _d, e in blocks)
        n_stub, problems, stubbed = check_imports(image, ext, blocks, imports)
        print(f"imports: {bound} bound to {EUSER_DLL} by ordinal "
              f"({len(imports)} resolved, {len(imports) - bound} veneers gc'd), "
              f"{len(unresolved)} names with NO euser ordinal")
        for n in unresolved[:10]:
            print(f"  unresolved: {n}")
        print(f"  {n_stub} of them bind an ordinal that euser serves with the unimpl stub"
              + (": " + ", ".join(stubbed[:6]) if stubbed else ""))
        for p in problems[:10]:
            print("  ! " + p)
        ok = ok and not problems and not unresolved and bound > 0
        print("X32 EXE IMPORTS OK" if ok else "X32 EXE IMPORTS FAIL")
    else:
        print("X32 E32IMAGE OK" if ok else "X32 E32IMAGE FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
