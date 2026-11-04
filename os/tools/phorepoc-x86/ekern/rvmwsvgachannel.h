/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// rvmwsvgachannel.h -- OURS: the user-side API of the VMware SVGA II / SVGA3D kernel driver
// (tools/phorepoc-x86/ekern/xk_vmwsvga.cpp, built into ekern, device "VmwSvga"). The Symbian counterpart of the DRM_VMW_* ioctls the Mesa
// `svga` winsys uses, in the shape of RMaliChannel:
//   GetInfo     -> the device (SVGA_ID, CAPABILITIES, CAP2, the 3D devcaps that matter: 3D / DXCONTEXT / SM41 / SM5 / GL43 / max texture) and
//                  this channel's id windows (below)
//   GetDevCaps  -> all SVGA3D_DEVCAP_MAX device caps (what DRM_VMW_GET_3D_CAP returns)
//   OpenArena   -> a shared chunk over ONE physically contiguous RAM block (the channel's command staging area: the device reads command
//                  buffers from it by physical address). TVmwSvgaInfo::iArenaPhys is its physical address (the legacy S2 test builds MOBs in it).
//   Submit      -> one command buffer = [iOffset, iOffset + iLength) of the arena, on device-queue context iContext (0 = the 3D context, 0x3f = the
//                  device context), flags NO_IRQ(1) / DX_CONTEXT(2) + iDxContext; synchronous (<= 2 s); the header status comes back in iStatus
//                  (1 = COMPLETED). The kernel owns the command-buffer headers and bounds-checks the range; the command stream is not validated.
//   OpenPool    -> S3: a shared chunk of guest memory (TVmwSvgaInfo::iPoolBytes of address space), committed page by page from anywhere in RAM
//                  (zeroed). A MOB (guest-backed object memory) is a range of it: DefineMob builds the page table the device walks (one
//                  contiguous range, or PT64 depth 1 / 2) and issues DEFINE_GB_MOB64; DestroyMob undoes it. The device-wide object tables
//                  (OTables) are the kernel's, set up at the first OpenPool.
//   PoolCommit  -> commit [iOffset, iOffset + iSize) of the pool (pages already committed are kept).
// Ids: the device's object ids (MOB, surface, DX context) are device-wide; each channel gets a window of each (TVmwSvgaInfo::iMobBase ...) so
// concurrent processes never collide. A channel's objects are destroyed when it closes (the kernel keeps the MOBs it defined and the highest
// surface / context id the client reported with Submit).
// Open never needs User::LoadLogicalDevice (the device is installed by the kernel extension).
#ifndef RVMWSVGACHANNEL_H
#define RVMWSVGACHANNEL_H

#include <e32cmn.h>
#ifndef __KERNEL_MODE__
#include <e32std.h>
#endif

_LIT(KVmwSvgaLddName, "VmwSvga");
const TInt KVmwSvgaMajorVersion = 1;
const TInt KVmwNumDevCaps = 262;			// SVGA3D_DEVCAP_MAX

struct TVmwSvgaInfo
	{
	TInt iVersion;				// the negotiated SVGA_ID (3 = SVGA_ID_3)
	TUint32 iCaps, iCap2;
	TUint32 iArenaPhys;
	TInt iArenaSize;
	TUint32 iDevCaps[8];		// 3D, DXCONTEXT, SM41, SM5, GL43, MAX_TEXTURE_WIDTH
	// S3
	TInt iPoolBytes;			// the pool's size (address space; pages are committed on demand)
	TUint32 iMobBase, iMobCount;	// this channel's window of MOB ids
	TUint32 iSurfBase, iSurfCount;	// ... of surface ids
	TUint32 iCtxBase, iCtxCount;	// ... of DX context ids
	TUint32 iMobMaxKb;			// SVGA_REG_MOB_MAX_SIZE in KB
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
	TUint32 iSurfHigh;			// in: the highest surface / DX context id of this channel's windows defined so far (cleanup at close); 0 = none
	TUint32 iCtxHigh;
	};

struct TVmwDevCaps
	{
	TUint32 iCaps[KVmwNumDevCaps];
	};

struct TVmwPoolRange
	{
	TInt iOffset, iSize;
	};

struct TVmwMob
	{
	TUint32 iMobId;
	TInt iOffset, iSize;		// DefineMob: a committed range of the pool
	};

enum TVmwSvgaControl { EVmwCtrlGetInfo, EVmwCtrlOpenArena, EVmwCtrlSubmit, EVmwCtrlGetDevCaps, EVmwCtrlOpenPool, EVmwCtrlPoolCommit,
	EVmwCtrlDefineMob, EVmwCtrlDestroyMob };

#ifndef __KERNEL_MODE__
class RVmwSvgaChannel : public RBusLogicalChannel
	{
public:
	inline static TVersion VersionRequired() { return TVersion(KVmwSvgaMajorVersion, 0, 0); }
	inline TInt Open() { return DoCreate(KVmwSvgaLddName, VersionRequired(), KNullUnit, NULL, NULL, EOwnerProcess); }
	inline TInt GetInfo(TVmwSvgaInfo& aInfo) { TPckg<TVmwSvgaInfo> p(aInfo); return DoControl(EVmwCtrlGetInfo, &p); }
	inline TInt GetDevCaps(TVmwDevCaps& aCaps) { TPckg<TVmwDevCaps> p(aCaps); return DoControl(EVmwCtrlGetDevCaps, &p); }
	inline TInt OpenArena(RChunk& aChunk) { TInt h = DoControl(EVmwCtrlOpenArena); return h < 0 ? h : aChunk.SetReturnedHandle(h); }
	inline TInt OpenPool(RChunk& aChunk) { TInt h = DoControl(EVmwCtrlOpenPool); return h < 0 ? h : aChunk.SetReturnedHandle(h); }
	inline TInt PoolCommit(TInt aOffset, TInt aSize) { TVmwPoolRange r = { aOffset, aSize }; TPckg<TVmwPoolRange> p(r); return DoControl(EVmwCtrlPoolCommit, &p); }
	inline TInt DefineMob(TUint32 aMobId, TInt aOffset, TInt aSize) { TVmwMob m = { aMobId, aOffset, aSize }; TPckg<TVmwMob> p(m); return DoControl(EVmwCtrlDefineMob, &p); }
	inline TInt DestroyMob(TUint32 aMobId) { TVmwMob m = { aMobId, 0, 0 }; TPckg<TVmwMob> p(m); return DoControl(EVmwCtrlDestroyMob, &p); }
	inline TInt Submit(TVmwSubmit& aSub) { TPckg<TVmwSubmit> p(aSub); return DoControl(EVmwCtrlSubmit, &p); }
	};
#endif

#endif
