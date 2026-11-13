// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// xk_sgresource.cpp -- OURS: the graphics kernel side linked into this kernel.
//  - Graphics Resource (RSgImage): the sg extension (sgextension.dll, a KEXT in a real ROM) and the sgdevice LDD are the REAL
//    2010 sources (graphicsresourceimplementation/src: sgextension, sgresource, sgdevice, sgchannel), entry points renamed by
//    make_ekern.sh. The extension starts with the other built-in extensions (X64ExtensionEntry, before any user process); the
//    LDD is the built-in device Sys\Bin\sgdevice.ldd (xboot.cpp fills export 1 here) that sgresource.dll loads by name
//    (RSgDriver::Open -> User::LoadLogicalDevice).
//  - The surface manager (NGA: RSurfaceManager, the buffers WSERV's composition and its clients share): the REAL 2010
//    reference driver (graphicscompositionref/surfacemgrcommon: surfacemanagerdriver.cpp = DECLARE_EXTENSION_LDD +
//    DECLARE_STANDARD_EXTENSION, extension.cpp), as the ARM boards link it. Its extension starts here with the sg one; its LDD
//    is the built-in device Sys\Bin\surfacemanagerdriver.ldd (RSurfaceManager::Open loads it by name).
#include <kernel/kern_priv.h>

TInt InitExtension_sgext();					// sgextension.cpp (DECLARE_STANDARD_EXTENSION, renamed)
DLogicalDevice* CreateLogicalDevice_sgdev();	// sgdevice.cpp (DECLARE_STANDARD_LDD, renamed)
TInt InitExtension_surfmgr();					// surfacemanagerdriver.cpp (DECLARE_STANDARD_EXTENSION, renamed)
DLogicalDevice* CreateLogicalDevice_surfmgr();	// surfacemanagerdriver.cpp (DECLARE_EXTENSION_LDD, renamed)

void X64StartGraphicsResource()
	{
	TInt r = InitExtension_sgext();
	Kern::Printf("[sg] graphics resource extension -> %d", r);
	r = InitExtension_surfmgr();
	Kern::Printf("[sm] surface manager extension -> %d", r);
	}

extern "C" DLogicalDevice* X64CreateLdd_sgdevice()
	{
	return CreateLogicalDevice_sgdev();
	}

extern "C" DLogicalDevice* X64CreateLdd_surfmgr()
	{
	return CreateLogicalDevice_surfmgr();
	}
