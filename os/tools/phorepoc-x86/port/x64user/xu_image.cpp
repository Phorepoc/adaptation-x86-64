// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// xu_image.cpp -- Phorepoc amd64/x32: the bits EVERY E32Image carries, whatever its target type.
//
// Split out of xu_dll.cpp at X4.12, when EFILE.EXE became the first EXE with an export table
// (TARGETTYPE exexp) and so the first non-DLL image needing __unimpl_export. None of what is
// here belongs to the DLL entry point; it belongs to "a module compiled by a modern g++ and
// presenting a frozen .def export table", which euser/efsrv/hal/efile all are.
//
// Every image links this. It must therefore define NOTHING a 2010 TU also defines -- in
// particular not operator new/delete or __cxa_pure_virtual, which euser's own census TUs
// (common/gcchelp.cpp) already provide. Those live in xu_cxxrt.cpp, which only images ABOVE
// euser link.
//
// 9b (the stub's design), X4.12 (the split).

#include "u32std.h"
#include <e32std.h>

extern "C" {

// g++ takes the address of this for __cxa_atexit; nothing consumes the registrations
// (process teardown reclaims), so an address is all it has to be.
void* __dso_handle = 0;

// A file-scope object with a destructor makes g++ emit __cxa_atexit(dtor, obj, &__dso_handle)
// from the constructor. EFILE is the first Path C image with one, so it is the first to need
// this symbol. Registering nothing is the honest placeholder, not a shortcut: the platform runs
// static destructors from destroyStatics() at module DETACH, which is the same X64_TODO that
// _E32Dll_Body's missing .fini_array walk names. Until that exists there is nowhere to put a
// registration, and a process only ever exits by being torn down, which reclaims everything.
// Returning KErrNone matches the Itanium ABI contract (0 = registered).
TInt __cxa_atexit(void (*)(void*), void*, void*)
	{
	return KErrNone;
	}

// Every ordinal an image does not yet build from source points here (the amd64 twin of
// toolchain/euser_dll/unimpl_export.cpp). Calling one panics THIS thread with category
// "EUSRNIMP" and reason = the CALLER's return address -- so the panic names the exact call
// site instead of running into unrelated bytes.
//
// Deliberately via User::Panic rather than the Exec::ThreadKill gate underneath it: the exec
// stubs live in EUSER and are not exported, so a module above euser (efsrv, hal, efile...)
// cannot reach them -- but User::Panic is euseru.def ordinal 650, which every image can import.
// Inside euser itself the call simply resolves locally.
void __unimpl_export()
	{
	_LIT(KCat, "EUSRNIMP");
	User::Panic(KCat, (TInt)(TUint32)(TLinAddr)__builtin_return_address(0));
	for (;;) {}
	}

} // extern "C"
