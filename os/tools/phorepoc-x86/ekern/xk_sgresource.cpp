// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// xk_sgresource.cpp -- OURS: the Graphics Resource kernel side (RSgImage) linked into this kernel. The sg extension
// (sgextension.dll, a KEXT in a real ROM) and the sgdevice LDD are the REAL 2010 sources (graphicsresourceimplementation/src:
// sgextension, sgresource, sgdevice, sgchannel), entry points renamed by make_ekern.sh. The extension starts with the other
// built-in extensions (X64ExtensionEntry, before any user process); the LDD is the built-in device Sys\Bin\sgdevice.ldd
// (xboot.cpp fills export 1 here) that sgresource.dll loads by name (RSgDriver::Open -> User::LoadLogicalDevice).
#include <kernel/kern_priv.h>

TInt InitExtension_sgext();					// sgextension.cpp (DECLARE_STANDARD_EXTENSION, renamed)
DLogicalDevice* CreateLogicalDevice_sgdev();	// sgdevice.cpp (DECLARE_STANDARD_LDD, renamed)

void X64StartGraphicsResource()
	{
	TInt r = InitExtension_sgext();
	Kern::Printf("[sg] graphics resource extension -> %d", r);
	}

extern "C" DLogicalDevice* X64CreateLdd_sgdevice()
	{
	return CreateLogicalDevice_sgdev();
	}
