// Portions Copyright (c) Nokia Corporation and/or its subsidiary(-ies)
// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// xu_dll.cpp -- Phorepoc amd64/x32: the DLL entry point.
// Sibling of euser/epoc/x86/uc_dll.cia + uc_dll.cpp (and of toolchain/dll_rt/dll_entry.S on ARM).
// Used by emitter/build_euser_x32.py; linked FIRST (x32_dll.ld puts .text.entry at offset 0) so
// _E32Dll owns code offset 0 = the image entry point.
//
// __unimpl_export / __dso_handle used to live here; they moved to xu_image.cpp at X4.12 because
// they are not DLL-entry business -- EFILE.EXE (TARGETTYPE exexp) needs them too.
//
// THE 16-BYTE ENTRY HEADER is a real platform contract, not decoration (uc_dll.cia):
//   +0  a jump over the header into the body
//   +8  zero word  (the loader writes the code-segment unique ID here for RAM-loaded code)
//   +12 the DLL's UID3 -- Dll::Tls() reads *(TInt*)(MODULE_HANDLE+12), i.e. *(_E32Dll+12)
//       (euser/epoc/up_std.h MODULE_HANDLE, up_dll_tls.cpp). Get this wrong and per-DLL TLS
//       silently keys on garbage.
// Offset 0 must also stay NON-exported: DEpocCodeSeg::Lookup treats "export == entry point" as
// an absent ordinal (the EKDATA trap, Track-B M7.5).
//
// ENTRY REASON arrives in EDI (SysV x32), not on the stack as it did on IA-32.
// 9b.

#include "u32std.h"
#include <e32std.h>

#ifndef DLL_UID3
#define DLL_UID3	0x100039e5		// euser.mmh: uid 0x1000008d 0x100039e5
#endif
#define XU_STR2(x)	#x
#define XU_STR(x)	XU_STR2(x)

extern "C" {

// x32_dll.ld brackets the module's static-constructor table.
extern void (*const __init_array_start[])();
extern void (*const __init_array_end[])();

TInt _E32Dll_Body(TInt aReason)
	{
	if (aReason == KModuleEntryReasonProcessAttach)
		for (void (*const* f)() = __init_array_start; f < __init_array_end; ++f)
			(**f)();
	// X64_TODO: KModuleEntryReasonProcessDetach -> .fini_array walk (destroyStatics).
	return KErrNone;
	}

__attribute__((naked, section(".text.entry"))) TInt _E32Dll(TInt /*aReason*/)
	{
	// Hand-encoded exactly like uc_dll.cia: the header words must land at fixed
	// offsets, so the jump is a literal `eb 0e` (jmp +14 from the next byte = +16)
	// rather than something the assembler may relax to a different width.
	// (UID3 is stringified into the .long, not passed as an "i" operand: a protected-range UID3 such
	// as 0xA00003D6 is no signed 32-bit immediate and %c refused it -- X4.52, the SCP timestamp plug-in)
	__asm__(".byte 0xeb, 0x0e     \n"	// +0  jmp -> +16
			".byte 0,0,0,0,0,0    \n"	// +2  pad
			".long 0              \n"	// +8  loader: RAM code-segment unique ID
			".long " XU_STR(DLL_UID3) "\n"	// +12 UID3 -- Dll::Tls() reads this word
			"jmp _E32Dll_Body     \n");	// +16
	}

} // extern "C"
