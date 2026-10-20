/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

// rvmwsvgachannel.h -- OURS: the user-side API of the VMware SVGA II / SVGA3D kernel driver
// (tools/phorepoc-x86/ekern/xk_vmwsvga.cpp, built into ekern, device "VmwSvga"). The Symbian counterpart of the DRM_VMW_* ioctls the Mesa
// `svga` winsys uses, in the shape of RMaliChannel:
//   GetInfo   -> the device (SVGA_ID, CAPABILITIES, CAP2, the 3D devcaps that matter: 3D / DXCONTEXT / SM41 / SM5 / GL43 / max texture)
//   OpenArena -> a shared chunk over ONE physically contiguous 2 MB RAM block (the device reads/writes it by physical address: MOB contents,
//                object tables, COTables, command streams). TVmwSvgaInfo::iArenaPhys is its physical address (MOB bases / OTable bases are PPNs of it).
//   Submit    -> one command buffer = [iOffset, iOffset + iLength) of the arena, on device-queue context iContext (0 = the 3D context, 0x3f = the
//                device context), flags NO_IRQ(1) / DX_CONTEXT(2) + iDxContext; synchronous (<= 2 s); the header status comes back in iStatus
//                (1 = COMPLETED). The kernel owns the command-buffer headers and bounds-checks the range; the command stream is not validated.
// Open never needs User::LoadLogicalDevice (the device is installed by the kernel extension).
#ifndef RVMWSVGACHANNEL_H
#define RVMWSVGACHANNEL_H

#include <e32cmn.h>
#ifndef __KERNEL_MODE__
#include <e32std.h>
#endif

_LIT(KVmwSvgaLddName, "VmwSvga");
const TInt KVmwSvgaMajorVersion = 1;

struct TVmwSvgaInfo
	{
	TInt iVersion;				// the negotiated SVGA_ID (3 = SVGA_ID_3)
	TUint32 iCaps, iCap2;
	TUint32 iArenaPhys;
	TInt iArenaSize;
	TUint32 iDevCaps[8];		// 3D, DXCONTEXT, SM41, SM5, GL43, MAX_TEXTURE_WIDTH
	};

struct TVmwSubmit
	{
	TInt iOffset;				// into the arena, 4-byte aligned
	TInt iLength;
	TUint32 iContext;			// SVGA_CB_CONTEXT_0 = 0, _DEVICE = 0x3f
	TUint32 iFlags;				// 1 = NO_IRQ, 2 = DX_CONTEXT
	TUint32 iDxContext;
	TUint32 iStatus;			// out: SVGACBStatus (1 = completed)
	TUint32 iErrorOffset;		// out
	};

enum TVmwSvgaControl { EVmwCtrlGetInfo, EVmwCtrlOpenArena, EVmwCtrlSubmit };

#ifndef __KERNEL_MODE__
class RVmwSvgaChannel : public RBusLogicalChannel
	{
public:
	inline static TVersion VersionRequired() { return TVersion(KVmwSvgaMajorVersion, 0, 0); }
	inline TInt Open() { return DoCreate(KVmwSvgaLddName, VersionRequired(), KNullUnit, NULL, NULL, EOwnerThread); }
	inline TInt GetInfo(TVmwSvgaInfo& aInfo) { TPckg<TVmwSvgaInfo> p(aInfo); return DoControl(EVmwCtrlGetInfo, &p); }
	inline TInt OpenArena(RChunk& aChunk) { TInt h = DoControl(EVmwCtrlOpenArena); return h < 0 ? h : aChunk.SetReturnedHandle(h); }
	inline TInt Submit(TVmwSubmit& aSub) { TPckg<TVmwSubmit> p(aSub); return DoControl(EVmwCtrlSubmit, &p); }
	};
#endif

#endif
