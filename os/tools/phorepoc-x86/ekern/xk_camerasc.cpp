// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// xk_camerasc.cpp -- Path C (camera-plan.md C1): the two built-in-device factories of the camera driver pair (rombuild --builtin-device; xboot.cpp points
// export 1 of Sys\Bin\ecamerasc.ldd / camerasc.pdd here), exactly as the sound driver's (xk_ac97.cpp). The LDD is the 2010 DCameraScLdd and the PDD the 2010
// template PDD (a fake sensor), both compiled unmodified with only their entry points renamed (make_ekern.sh); the user API is RDevCameraSc ("CameraSc").
#include <kernel/kern_priv.h>

DLogicalDevice* CreateLogicalDevice_camerasc();		// cameraldd.cpp (DECLARE_STANDARD_LDD, renamed)
DPhysicalDevice* CreatePhysicalDevice_camerasc();	// camerasc.cpp, the template PDD (DECLARE_STANDARD_PDD, renamed)

extern "C" DLogicalDevice* X64CreateLdd_camerasc()
	{
	return CreateLogicalDevice_camerasc();
	}

extern "C" DPhysicalDevice* X64CreatePdd_camerasc()
	{
	return CreatePhysicalDevice_camerasc();
	}
