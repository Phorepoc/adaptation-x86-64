// Portions Copyright (c) Nokia Corporation and/or its subsidiary(-ies)
// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xu_cxxrt.cpp -- Phorepoc amd64/x32: the modern-g++ runtime leaves that no 2010 TU provides.
// The amd64 sibling of toolchain/euser_dll/cxx_rt_dev.cpp (ARM). Linked by an image that needs
// them; euser does not (its census TUs already cover everything it references).
//
//   _ZdlPvj            sized operator delete (C++14). Every deleting destructor compiled by a
//                      modern g++ ends in it, so any image with a CBase-derived class hits it.
//   __cxa_pure_virtual pure-call trap. A silent return would carry on with a garbage vtable
//                      slot; User::Invariant is the platform's "this cannot happen" panic.
//
// No writable data on purpose: these images are XIP.
#include <e32std.h>

void operator delete(TAny* aPtr, TUint) __NO_THROW
	{
	operator delete(aPtr);
	}

extern "C" void __cxa_pure_virtual()
	{
	for (;;)
		User::Invariant();
	}
