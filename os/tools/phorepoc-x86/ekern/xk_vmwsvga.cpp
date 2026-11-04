// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// xk_vmwsvga.cpp -- Path C (2026-10-16): rungs S0 + S1 of. The VMware SVGA II / SVGA3D PCI device
// (15AD:0405) -- what VMware Workstation exposes and what VirtualBox's `--graphicscontroller vmsvga` emulates (with
// `--accelerate3d on` the 3D command set behind it).
//
// S0: negotiate the register interface (SVGA_ID_3 at port base+1) and log the capability registers and the 3D device caps the Mesa
//     `svga` winsys asks the kernel for (DRM_VMW_GET_PARAM): guest-backed objects (MOBs), DX contexts (VGPU10 = GLES 3.0), SM5 (GL 4.1),
//     GL43 (compute = GLES 3.1).
// S1: "the device answers" -- the kernel half of the command path, as vmwgfx does it (vmwgfx_cmdbuf.c / vmwgfx_fifo.c), with our own code:
//       * FIFO memory (BAR2) mapped and initialised (MIN/MAX/NEXT_CMD/STOP), SVGA_REG_ENABLE = ENABLE|HIDE (the device is on; the display driver
//         then takes over the scan-out with its first mode set, see below), TRACES off, CONFIG_DONE;
//       * the PCI INTx line bound, SVGA_REG_IRQMASK = COMMAND_BUFFER | ANY_FENCE (status port base+8, write-1-to-clear);
//       * command buffers: a 64-byte-aligned SVGACBHeader in physically contiguous RAM, submitted by writing its physical address
//         (| context id) to SVGA_REG_COMMAND_HIGH/LOW. First on the device context (0x3f): SVGA_DC_CMD_START_STOP_CONTEXT(enable, ctx 0);
//         then on context 0: an SVGA_CMD_FENCE. The header's status field turns SVGA_CB_STATUS_COMPLETED when the device ran it.
//     The self-test runs from NTimer callbacks (no sleeping in the extension entry) and logs one line per step; the exit line of S1 is
//         [svga] S1: command buffer completed + fence signalled
// S2: the guest-backed object path -- object tables (MOB, surface, context, shader, screen target, DX context: one page each, device-managed,
//     SET_OTABLE_BASE64 with MOBFMT_RANGE), one MOB over a contiguous RAM range, a GB surface bound to it, the surface filled by the CPU,
//     UPDATE_GB_IMAGE, and a SCREEN TARGET (DEFINE/BIND/UPDATE_GB_SCREENTARGET) that makes the host show it:
//         [svga] S2: screen target shown
//     The driver is a stub: single-threaded submission, the user API (RVmwSvgaChannel) is the LDD part of S2 (see below).
// Scan-out: on this adapter the display driver (xk_display.cpp) does not use the Bochs DISPI registers. X64SvgaProbe() brings the device up before the
//     display picks its framebuffer path; every mode (the first one and each host window resize) is programmed through SVGA_REG_WIDTH/HEIGHT/
//     BITS_PER_PIXEL + ENABLE and the pitch is read back from SVGA_REG_BYTES_PER_LINE; and each flush of the display reports the rectangle it wrote
//     with SVGA_CMD_UPDATE through the legacy FIFO (TVmwSvga::FifoWrite: ring between MIN and MAX, room checked against STOP, NEXT_CMD published
//     after the words, host pinged through SVGA_FIFO_BUSY + SVGA_REG_SYNC). The host window repaints only what it is told about.
// No-op (one log line) when the device is absent. Register numbers, capability and command-buffer layouts are Mesa's own MIT headers
// (src/gallium/drivers/svga/include/svga_reg.h, svga3d_devcaps.h).
#include <kernel/kern_priv.h>
#include <assp.h>
#include "xk_interrupt.h"
#include "rvmwsvgachannel.h"

static inline TUint32 Inl(TUint16 aPort)
	{ TUint32 v; __asm__ __volatile__("in eax, dx" : "=a"(v) : "d"(aPort)); return v; }
static inline void Outl(TUint16 aPort, TUint32 aVal)
	{ __asm__ __volatile__("out dx, eax" : : "d"(aPort), "a"(aVal) : "memory"); }
static TUint32 PciRead(TInt aDev, TInt aReg)
	{ Outl(0xCF8, 0x80000000u | aDev << 11 | (aReg & 0xFC)); return Inl(0xCFC); }
static void PciWrite(TInt aDev, TInt aReg, TUint32 aVal)
	{ Outl(0xCF8, 0x80000000u | aDev << 11 | (aReg & 0xFC)); Outl(0xCFC, aVal); }

enum
	{
	KRegId = 0, KRegEnable = 1, KRegWidth = 2, KRegHeight = 3, KRegBitsPerPixel = 7, KRegBytesPerLine = 12, KRegFbStart = 13,
	KRegFbOffset = 14, KRegSync = 21, KRegVramSize = 15, KRegCapabilities = 17, KRegMemStart = 18, KRegMemSize = 19, KRegConfigDone = 20,
	KRegMemRegs = 30, KRegIrqMask = 33, KRegTraces = 45, KRegCommandLow = 48, KRegCommandHigh = 49, KRegMaxPrimaryMem = 50,
	KRegSuggestedGbMemKb = 51, KRegDevCap = 52, KRegScreenTargetMaxW = 55, KRegScreenTargetMaxH = 56, KRegMobMaxSize = 57,
	KRegCap2 = 59, KRegFifoCaps = 68, KRegFence = 69, KRegGbMemSizeKb = 76,
	KCap3d = 0x4000, KCapExtFifo = 0x8000, KCapGmr2 = 0x400000, KCapScreenObj2 = 0x800000, KCapCmdBuffers = 0x1000000,
	KCapCmdBuffers2 = 0x4000000, KCapGbObjects = 0x8000000, KCapDx = 0x10000000, KCapCap2Reg = 0x80000000u,
	KEnableEnable = 1, KEnableHide = 2,
	KIrqAnyFence = 1 << 0, KIrqCommandBuffer = 1 << 3, KIrqError = 1 << 4,
	KFifoMin = 0, KFifoMax = 1, KFifoNextCmd = 2, KFifoStop = 3, KFifoCaps = 4, KFifoFence = 6, KFifoNumRegs = 291,
	KFifoCapFence = 1, KFifoBusy = 290, KCmdUpdate = 1, KSyncGeneric = 1,
	KCbContextDevice = 0x3f, KCbStatusNone = 0, KCbStatusCompleted = 1,
	KDcCmdStartStopContext = 1, KCmdFence = 30,
	KIrqStatusPort = 8,
	// SVGA3D command ids (svga3d_cmd.h) and enums
	KCmdDefineGbSurface = 1097, KCmdBindGbSurface = 1099, KCmdUpdateGbImage = 1101, KCmdSetOtableBase64 = 1115,
	KCmdDefineGbScreenTarget = 1124, KCmdDestroyGbScreenTarget = 1125, KCmdBindGbScreenTarget = 1126, KCmdUpdateGbScreenTarget = 1127,
	KCmdDefineGbMob64 = 1135,
	KOtableMob = 0, KOtableSurface = 1, KOtableContext = 2, KOtableShader = 3, KOtableScreenTarget = 4, KOtableDxContext = 5,
	KMobFmtRange = 3, KSurfFmtX8R8G8B8 = 1, KSurfScreenTarget = 1 << 16, KStFlagPrimary = 1,
	// layout of the 1 MB DMA block
	KOffHeaders = 0, KOffCmds = 0x1000, KCmdSlotBytes = 0x1000, KOffOtables = 0x10000, KOffImage = 0x20000, KDmaBytes = 0x200000,
	KImgW = 360, KImgH = 320,
	// S2b: a DX (VGPU10) context and a GPU clear
	KCmdDxDefineContext = 1143, KCmdDxBindContext = 1145, KCmdDxClearRenderTargetView = 1176, KCmdDxDefineRenderTargetView = 1187,
	KCmdDxSetCoTable = 1207, KCbFlagDxContext = 2,
	KSurfBindShaderResource = 1 << 23, KSurfBindRenderTarget = 1 << 24, KSurfFmtB8G8R8A8Unorm = 141, KResTexture2D = 3,
	KOffDxCtx = 0x100000, KDxCtxBytes = 0x10000, KOffCoTables = 0x110000, KNumCoTables = 12
	};

// SVGACBHeader (svga_reg.h, packed): 64 bytes
struct SCbHeader
	{
	volatile TUint32 iStatus, iErrorOffset;
	TUint32 iIdLo, iIdHi;
	TUint32 iFlags, iLength;
	TUint32 iPaLo, iPaHi;				// ptr.pa
	TUint32 iOffset, iDxContext;
	TUint32 iMustBeZero[6];
	};

class TVmwSvga
	{
public:
	TInt Init();
	TInt Probe();							// find + negotiate + FIFO up; idempotent (the display calls it before the 3D setup)
	TBool SetMode(TInt aW, TInt aH, TInt& aPitch, TUint32& aFbPhys, TUint32 aExpectPhys);
	TBool Update(TInt aX, TInt aY, TInt aW, TInt aH);
	void Ping();
	TBool FifoWrite(const TUint32* aWords, TInt aCount);
	TUint32 DevCap(TUint32 aIdx)			{ Write(KRegDevCap, aIdx); return Read(KRegDevCap); }
	TUint32 RegRead(TUint32 aIdx)			{ return Read(aIdx); }
	TInt SubmitChannel(class DVmwSvgaChannel& aCh, TVmwSubmit& aSub);
	TInt KernelCmd(const TUint32* aWords, TInt aCount, TUint32 aContext, TUint32 aFlags = 0, TUint32 aDxContext = 0);
	void ScanoutDrop();
	TInt EnsureObjectTables();
	TInt BuildTable(class TVmwPool& aPool, TInt aOff, TInt aSize, TUint32& aDepth, TUint64& aBase, TInt*& aPtPages, TInt& aPtCount);
	TInt PtAlloc();
	void PtFree(TInt aPage);
	static void Isr(TAny* aPtr);
	static void StepFn(TAny* aPtr);
private:
	TUint32 Read(TUint32 aIdx)				{ Outl(iBase, aIdx); return Inl(iBase + 1); }
	void Write(TUint32 aIdx, TUint32 aVal)	{ Outl(iBase, aIdx); Outl(iBase + 1, aVal); }
	TInt Negotiate();
	void Report();
	TInt InitFifo();
	void Submit(TInt aSlot, TUint32 aContext, TInt aLength, TUint32 aFlags = 0, TUint32 aDxContext = 0);
	TBool Done(TInt aSlot) const			{ return Hdr(aSlot)->iStatus != KCbStatusNone; }
	SCbHeader* Hdr(TInt aSlot) const		{ return (SCbHeader*)(iDma + KOffHeaders + aSlot * 64); }
	TUint32* Cmd(TInt aSlot) const			{ return (TUint32*)(iDma + KOffCmds + aSlot * KCmdSlotBytes); }
	TUint32 HdrPhys(TInt aSlot) const		{ return iDmaPhys + KOffHeaders + aSlot * 64; }
	TUint32 CmdPhys(TInt aSlot) const		{ return iDmaPhys + KOffCmds + aSlot * KCmdSlotBytes; }
	void Begin(TInt aSlot)					{ iCbSlot = aSlot; iCbLen = 0; }
	void Add(TUint32 aId, const void* aBody, TInt aSize);
	void Send(TUint32 aContext)				{ Submit(iCbSlot, aContext, iCbLen); }
	void SendDx(TUint32 aCid)				{ Submit(iCbSlot, 0, iCbLen, KCbFlagDxContext, aCid); }
	void BuildDxSetup();
	void BuildDxClear();
	void BuildScreenTarget();
public:
	TUint32 iBase;
	TInt iDev;
	TUint32 iCaps, iCap2;
	TInt iVer;
	volatile TUint32* iFifo;				// BAR2, as dwords
	TInt iFifoBytes;
	TUint8* iDma;
	TPhysAddr iDmaPhys;
	volatile TInt iIrqs;
	volatile TUint32 iIrqBits;
	TInt iStep;
	TInt iWait;
	TInt iCbSlot, iCbLen;
	DMutex* iLock;
	NTimer iTimer;
	TUint32 iFence;
	TInt iProbe;							// 0 = not tried, 1 = device up (FIFO configured), < 0 = no usable device
	class TVmwPool* iKPool;					// S3: the device-wide object tables (OTables) and the page-table pages of the MOBs (pool: kernel-owned)
	TInt iOtablesUp;						// 0 = not set up, 1 = up, < 0 = failed
	TUint32 iPtUsed[1024 / 32];				// page-table pages of iKPool (KPtPageBase ..): in use
	TUint32 iSlots;							// channel id windows in use (bit per slot)
	TUint32 iCbSpins, iCbTicks;
	TUint32 iCbs, iCbSleeps;				// client command buffers run / timer ticks slept waiting for them (statistics)
	TBool iModeSet;							// the display owns the scan-out through the SVGA registers (ENABLE without HIDE)
	TUint32 iUpdates, iUpdatesDropped, iFifoFullWaits;
	TInt iModeW, iModeH;					// the display mode the registers hold
	volatile TInt iScanout;					// 1 = a GPU screen target (a client's render target) is what the host shows (see ScanoutDrop)
	TUint32 iScanoutDrops;
	TVmwSvga() : iTimer(StepFn, this), iProbe(0), iKPool(NULL), iOtablesUp(0), iSlots(0), iCbSpins(0), iCbTicks(0), iCbs(0), iCbSleeps(0), iModeSet(EFalse), iModeW(0), iModeH(0), iScanout(0), iScanoutDrops(0), iUpdates(0), iUpdatesDropped(0), iFifoFullWaits(0)
		{ memclr(iPtUsed, sizeof(iPtUsed)); }
	};
static TVmwSvga* TheSvga;

// ---------------------------------------------------------------------------------------------------------------------------------------
// S2/S3: the user API -- the logical device "VmwSvga" (rvmwsvgachannel.h), the Symbian counterpart of the DRM_VMW_* ioctls the Mesa `svga` winsys
// uses. Same shape as RMaliChannel: the channel hands the client its ARENA (a physically contiguous block as a shared chunk: the command staging
// area), Submit runs one command buffer out of it, and (S3) a POOL: a large shared chunk committed page by page from any RAM, whose ranges are
// the device's guest-backed memory (MOBs). The kernel owns the command-buffer HEADERS (a client cannot forge one), the device-wide object tables
// (OTables), the MOB page tables and the object-id windows of each channel; it bounds-checks the command range but does not validate the stream
// (a single trusted channel, as the Mali driver). The device is installed from the extension entry: Open needs no User::LoadLogicalDevice.

const TInt KPoolBytes = 256 << 20;				// a channel's pool: address space (physical pages are committed on demand)
const TInt KKPoolBytes = 8 << 20;				// the kernel's own pool: the OTables, then the page-table pages
const TInt KPtPageBase = 1024, KPtPages = 1024;	// page-table pages of the MOBs: pages 1024 .. 2047 of the kernel pool (4 MB .. 8 MB)
const TInt KArenaBytes = 2 << 20, KArenaMin = 512 << 10;
const TInt KIdSlots = 16, KIdStride = 2048;		// ids of one channel: slot * 2048 + 1 ..
const TInt KMobsPerSlot = KIdStride - 1, KSurfsPerSlot = KIdStride - 1, KCtxPerSlot = 15;
const TInt KCtxStride = 16;
const TInt KHdrSlotChannel = 16, KHdrSlotKernel = 9;
// OTables in the kernel pool (entry sizes: MOB 16, surface 64; the others have room to spare): MOB 32768 entries, surface 32768, the rest 1024
enum
	{
	KOtMobOff = 0, KOtMobBytes = 0x100000, KOtSurfOff = 0x100000, KOtSurfBytes = 0x200000, KOtCtxOff = 0x300000, KOtShaderOff = 0x310000,
	KOtStOff = 0x320000, KOtDxOff = 0x330000, KOtSmallBytes = 0x10000, KOtEnd = 0x340000,
	KCmdDestroyGbMob = 1094, KCmdDestroyGbSurface = 1098, KCmdDxDestroyContext = 1144,
	KMobFmtPt64d1 = 5, KMobFmtPt64d2 = 6
	};

// guest memory for the device: a shared chunk the client maps, whose pages are committed here (any RAM, zeroed), or one contiguous block
class TVmwPool
	{
public:
	static TVmwPool* New(TInt aBytes, TBool aContiguous);
	TInt Commit(TInt aOff, TInt aSize);
	TBool Committed(TInt aOff, TInt aSize) const;
	TUint8* Addr(TInt aOff, TInt aSize);
	TPhysAddr Phys(TInt aPage) const				{ return iPhys[aPage]; }
	void Release();									// the owner lets go; the pages are freed when the last user mapping is gone
	static void DestroyedFn(TAny* aPtr);
	TVmwPool() : iDfc(DestroyedFn, this, Kern::DfcQue0(), 1) {}
	DChunk* iChunk;
	TInt iBytes, iPages;
	TPhysAddr* iPhys;
	TPhysAddr iBlock;								// contiguous: the block (iPhys then holds each page of it)
	TDfc iDfc;
	};

TVmwPool* TVmwPool::New(TInt aBytes, TBool aContiguous)
	{
	TVmwPool* p = new TVmwPool;
	if (!p)
		return NULL;
	p->iBytes = aBytes;
	p->iPages = aBytes >> 12;
	p->iBlock = 0;
	p->iChunk = NULL;
	p->iPhys = (TPhysAddr*)Kern::AllocZ(p->iPages * sizeof(TPhysAddr));
	if (!p->iPhys)
		{
		delete p;
		return NULL;
		}
	TChunkCreateInfo info;
	info.iType = TChunkCreateInfo::ESharedKernelMultiple;
	info.iMaxSize = aBytes;
	info.iMapAttr = EMapAttrCachedMax;				// x86 DMA is cache-coherent
	info.iOwnsMemory = EFalse;						// the RAM is ours, not the chunk's
	info.iDestroyedDfc = &p->iDfc;
	TLinAddr lin;
	TUint32 attr;
	TInt r = Kern::ChunkCreate(info, p->iChunk, lin, attr);
	if (r != KErrNone)
		{
		Kern::Free(p->iPhys);
		delete p;
		return NULL;
		}
	if (aContiguous)
		{
		r = Epoc::AllocPhysicalRam(aBytes, p->iBlock);
		if (r == KErrNone)
			r = Kern::ChunkCommitPhysical(p->iChunk, 0, aBytes, p->iBlock);
		if (r == KErrNone)
			{
			TLinAddr ka;
			if (Kern::ChunkAddress(p->iChunk, 0, aBytes, ka) == KErrNone)
				memclr((TAny*)ka, aBytes);
			for (TInt i = 0; i < p->iPages; ++i)
				p->iPhys[i] = p->iBlock + (i << 12);
			}
		else
			{
			if (p->iBlock)
				Epoc::FreePhysicalRam(p->iBlock, aBytes);
			p->iBlock = 0;
			p->Release();							// (frees the object through the DFC when the chunk goes)
			return NULL;
			}
		}
	return p;
	}

TBool TVmwPool::Committed(TInt aOff, TInt aSize) const
	{
	if (aOff < 0 || aSize <= 0 || (aOff & 0xFFF) || aOff + aSize > iBytes)
		return EFalse;
	for (TInt p = aOff >> 12, e = (aOff + aSize + 0xFFF) >> 12; p < e; ++p)
		if (!iPhys[p])
			return EFalse;
	return ETrue;
	}

TInt TVmwPool::Commit(TInt aOff, TInt aSize)
	{
	if (aOff < 0 || aSize <= 0 || (aOff & 0xFFF) || aOff + aSize > iBytes || iBlock)
		return KErrArgument;
	const TInt first = aOff >> 12, end = (aOff + aSize + 0xFFF) >> 12;
	for (TInt p = first; p < end;)
		{
		if (iPhys[p])
			{
			++p;
			continue;
			}
		TInt q = p;
		while (q < end && !iPhys[q])
			++q;
		const TInt n = q - p;
		TPhysAddr* list = (TPhysAddr*)Kern::Alloc(n * sizeof(TPhysAddr));
		if (!list)
			return KErrNoMemory;
		TInt r = Epoc::AllocPhysicalRam(n, list);
		if (r == KErrNone)
			{
			r = Kern::ChunkCommitPhysical(iChunk, p << 12, n << 12, list);
			if (r != KErrNone)
				Epoc::FreePhysicalRam(n, list);
			}
		if (r != KErrNone)
			{
			Kern::Free(list);
			return r;
			}
		for (TInt i = 0; i < n; ++i)
			iPhys[p + i] = list[i];
		Kern::Free(list);
		TLinAddr ka;
		r = Kern::ChunkAddress(iChunk, p << 12, n << 12, ka);
		if (r != KErrNone)
			return r;
		memclr((TAny*)ka, n << 12);
		p = q;
		}
	return KErrNone;
	}

TUint8* TVmwPool::Addr(TInt aOff, TInt aSize)
	{
	TLinAddr ka;
	return Kern::ChunkAddress(iChunk, aOff, aSize, ka) == KErrNone ? (TUint8*)ka : NULL;
	}

void TVmwPool::Release()
	{
	if (iChunk)
		{
		DChunk* c = iChunk;
		iChunk = NULL;
		Kern::ChunkClose(c);					// the destroyed-DFC frees the pages and this object when the chunk is gone
		}
	else
		DestroyedFn(this);
	}

void TVmwPool::DestroyedFn(TAny* aPtr)
	{
	TVmwPool* p = (TVmwPool*)aPtr;
	NKern::ThreadEnterCS();
	if (p->iBlock)
		Epoc::FreePhysicalRam(p->iBlock, p->iBytes);
	else if (p->iPhys)
		{
		// free in runs of at most 256 pages
		TPhysAddr list[256];
		TInt n = 0;
		for (TInt i = 0; i < p->iPages; ++i)
			if (p->iPhys[i])
				{
				list[n++] = p->iPhys[i];
				if (n == 256)
					{
					Epoc::FreePhysicalRam(n, list);
					n = 0;
					}
				}
		if (n)
			Epoc::FreePhysicalRam(n, list);
		}
	Kern::Free(p->iPhys);
	delete p;
	NKern::ThreadLeaveCS();
	}

// a MOB or object table as the device walks it: one contiguous range, else PT64 depth 1 (<= 512 pages) or depth 2 (page-table pages from the kernel pool)
TInt TVmwSvga::PtAlloc()
	{
	for (TInt i = 0; i < KPtPages; ++i)
		if (!(iPtUsed[i >> 5] & (1u << (i & 31))))
			{
			const TInt page = KPtPageBase + i;
			if (iKPool->Commit(page << 12, 0x1000) != KErrNone)
				return -1;
			iPtUsed[i >> 5] |= 1u << (i & 31);
			return page;
			}
	return -1;
	}

void TVmwSvga::PtFree(TInt aPage)
	{
	const TInt i = aPage - KPtPageBase;
	iPtUsed[i >> 5] &= ~(1u << (i & 31));
	}

TInt TVmwSvga::BuildTable(TVmwPool& aPool, TInt aOff, TInt aSize, TUint32& aDepth, TUint64& aBase, TInt*& aPtPages, TInt& aPtCount)
	{
	aPtPages = NULL;
	aPtCount = 0;
	const TInt n = (aSize + 0xFFF) >> 12, p0 = aOff >> 12;
	TBool contiguous = ETrue;
	for (TInt i = 1; i < n && contiguous; ++i)
		contiguous = aPool.Phys(p0 + i) == aPool.Phys(p0) + (i << 12);
	if (contiguous)
		{
		aDepth = KMobFmtRange;
		aBase = (TUint64)aPool.Phys(p0) >> 12;
		return KErrNone;
		}
	const TInt l1 = (n + 511) / 512;
	const TInt count = n <= 512 ? 1 : 1 + l1;
	if (l1 > 512)
		return KErrTooBig;
	TInt* pages = (TInt*)Kern::Alloc(count * sizeof(TInt));
	if (!pages)
		return KErrNoMemory;
	for (TInt i = 0; i < count; ++i)
		{
		pages[i] = PtAlloc();
		if (pages[i] < 0)
			{
			while (--i >= 0)
				PtFree(pages[i]);
			Kern::Free(pages);
			return KErrNoMemory;
			}
		}
	if (n <= 512)
		{
		TUint64* e = (TUint64*)iKPool->Addr(pages[0] << 12, 0x1000);
		memclr(e, 0x1000);
		for (TInt i = 0; i < n; ++i)
			e[i] = (TUint64)aPool.Phys(p0 + i) >> 12;
		aDepth = KMobFmtPt64d1;
		}
	else
		{
		TUint64* root = (TUint64*)iKPool->Addr(pages[0] << 12, 0x1000);
		memclr(root, 0x1000);
		for (TInt j = 0; j < l1; ++j)
			{
			root[j] = (TUint64)iKPool->Phys(pages[1 + j]) >> 12;
			TUint64* e = (TUint64*)iKPool->Addr(pages[1 + j] << 12, 0x1000);
			memclr(e, 0x1000);
			for (TInt i = 0; i < 512 && j * 512 + i < n; ++i)
				e[i] = (TUint64)aPool.Phys(p0 + j * 512 + i) >> 12;
			}
		aDepth = KMobFmtPt64d2;
		}
	aBase = (TUint64)iKPool->Phys(pages[0]) >> 12;
	aPtPages = pages;
	aPtCount = count;
	return KErrNone;
	}

// a command the kernel itself issues (MOB and OTable management), through its own command-buffer slot; the caller holds iLock
TInt TVmwSvga::KernelCmd(const TUint32* aWords, TInt aCount, TUint32 aContext, TUint32 aFlags, TUint32 aDxContext)
	{
	if (aCount * 4 > KCmdSlotBytes)
		return KErrTooBig;
	memcpy(Cmd(KHdrSlotKernel), aWords, aCount * 4);
	Submit(KHdrSlotKernel, aContext, aCount * 4, aFlags, aDxContext);
	TInt waited = 0;
	for (TInt spin = 0; spin < 4000 && !Done(KHdrSlotKernel); ++spin)
		Kern::NanoWait(1000);
	while (!Done(KHdrSlotKernel) && waited < 2000)
		{
		NKern::Sleep(NKern::TimerTicks(1));
		++waited;
		}
	const TUint32 st = Hdr(KHdrSlotKernel)->iStatus;
	if (st != KCbStatusCompleted)
		{
		Kern::Printf("[svga] kernel command %u: status %u error offset %u", aWords[0], st, Hdr(KHdrSlotKernel)->iErrorOffset);
		return st == KCbStatusNone ? KErrTimedOut : KErrGeneral;
		}
	return KErrNone;
	}

// the device-wide object tables, once (vmw_otables_setup, with our own sizes); a PT64-mapped part of the kernel pool each
TInt TVmwSvga::EnsureObjectTables()
	{
	if (iOtablesUp)
		return iOtablesUp > 0 ? KErrNone : iOtablesUp;
	TInt r = KErrNoMemory;
	iKPool = TVmwPool::New(KKPoolBytes, EFalse);
	if (iKPool)
		r = iKPool->Commit(0, KOtEnd);
	if (r != KErrNone)
		return iOtablesUp = r;
	static const struct { TInt iType, iOff, iBytes; } KTables[] = {
		{ KOtableMob, KOtMobOff, KOtMobBytes }, { KOtableSurface, KOtSurfOff, KOtSurfBytes }, { KOtableContext, KOtCtxOff, KOtSmallBytes },
		{ KOtableShader, KOtShaderOff, KOtSmallBytes }, { KOtableScreenTarget, KOtStOff, KOtSmallBytes }, { KOtableDxContext, KOtDxOff, KOtSmallBytes } };
	for (TUint i = 0; i < sizeof(KTables) / sizeof(KTables[0]) && r == KErrNone; ++i)
		{
		TUint32 depth;
		TUint64 base;
		TInt* pt;
		TInt ptc;
		r = BuildTable(*iKPool, KTables[i].iOff, KTables[i].iBytes, depth, base, pt, ptc);   // (its page-table pages stay for good)
		if (r != KErrNone)
			break;
		const TUint32 cmd[8] = { KCmdSetOtableBase64, 24, (TUint32)KTables[i].iType, (TUint32)base, (TUint32)(base >> 32), (TUint32)KTables[i].iBytes, 0, depth };
		r = KernelCmd(cmd, 8, 0);
		Kern::Free(pt);
		}
	Kern::Printf("[svga] S3: object tables (MOB %d KB, surface %d KB, ...) r=%d", KOtMobBytes >> 10, KOtSurfBytes >> 10, r);
	iOtablesUp = (r == KErrNone ? 1 : r);
	return r;
	}

class DVmwSvgaDevice : public DLogicalDevice
	{
public:
	DVmwSvgaDevice() { iVersion = TVersion(KVmwSvgaMajorVersion, 0, 0); }
	virtual TInt Install()						{ return SetName(&KVmwSvgaLddName); }
	virtual void GetCaps(TDes8&) const			{}
	virtual TInt Create(DLogicalChannelBase*& aChannel);
	};

class DVmwSvgaChannel : public DLogicalChannelBase
	{
public:
	virtual ~DVmwSvgaChannel();
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt Request(TInt aReqNo, TAny* a1, TAny* a2);
	TUint32 MobBase() const						{ return iSlot * KIdStride + 1; }
	TUint32 SurfBase() const					{ return iSlot * KIdStride + 1; }
	TUint32 CtxBase() const						{ return iSlot * KCtxStride + 1; }
	DThread* iClient;
	TVmwPool* iArena;
	TVmwPool* iPool;
	TInt iSlot;
	TUint32 iSurfHigh, iCtxHigh;
	struct TMob { TInt iPtCount; TInt* iPtPages; };
	TMob* iMobs[KMobsPerSlot];
	};

TInt DVmwSvgaDevice::Create(DLogicalChannelBase*& aChannel)
	{
	aChannel = new DVmwSvgaChannel;
	return aChannel ? KErrNone : KErrNoMemory;
	}

TInt DVmwSvgaChannel::DoCreate(TInt, const TDesC8*, const TVersion& aVer)
	{
	iClient = NULL;
	iArena = NULL;
	iPool = NULL;
	iSlot = -1;
	if (!TheSvga || !TheSvga->iDma || !Kern::QueryVersionSupported(TVersion(KVmwSvgaMajorVersion, 0, 0), aVer))
		return KErrNotSupported;
	iClient = &Kern::CurrentThread();
	iClient->Open();
	NKern::ThreadEnterCS();
	Kern::MutexWait(*TheSvga->iLock);
	for (TInt s = 0; s < KIdSlots && iSlot < 0; ++s)
		if (!(TheSvga->iSlots & (1u << s)))
			{
			TheSvga->iSlots |= 1u << s;
			iSlot = s;
			}
	Kern::MutexSignal(*TheSvga->iLock);
	TInt r = iSlot < 0 ? KErrInUse : KErrNone;
	if (r == KErrNone)
		{
		iArena = TVmwPool::New(KArenaBytes, ETrue);
		if (!iArena)
			iArena = TVmwPool::New(KArenaMin, ETrue);
		if (!iArena)
			r = KErrNoMemory;
		}
	NKern::ThreadLeaveCS();
	return r;
	}

DVmwSvgaChannel::~DVmwSvgaChannel()
	{
	TVmwSvga* s = TheSvga;
	NKern::ThreadEnterCS();
	if (s && iSlot >= 0)
		{
		Kern::MutexWait(*s->iLock);
		TInt nMob = 0;
		// what this channel left on the device: contexts, then surfaces (ids the client reported), then the MOBs the kernel knows
		for (TUint32 c = 0; c < iCtxHigh && c < (TUint32)KCtxPerSlot; ++c)
			{
			const TUint32 cmd[3] = { KCmdDxDestroyContext, 4, CtxBase() + c };
			s->KernelCmd(cmd, 3, 0);
			}
		for (TUint32 i = 0; i < iSurfHigh && i < (TUint32)KSurfsPerSlot; ++i)
			{
			const TUint32 cmd[3] = { KCmdDestroyGbSurface, 4, SurfBase() + i };
			s->KernelCmd(cmd, 3, 0);
			}
		for (TInt i = 0; i < KMobsPerSlot; ++i)
			if (iMobs[i])
				{
				const TUint32 cmd[3] = { KCmdDestroyGbMob, 4, MobBase() + i };
				s->KernelCmd(cmd, 3, 0);
				for (TInt k = 0; k < iMobs[i]->iPtCount; ++k)
					s->PtFree(iMobs[i]->iPtPages[k]);
				Kern::Free(iMobs[i]->iPtPages);
				delete iMobs[i];
				++nMob;
				}
		s->iSlots &= ~(1u << iSlot);
		Kern::MutexSignal(*s->iLock);
		Kern::Printf("[svga] channel slot %d closed: %d contexts, %d surfaces, %d MOBs destroyed (device: %u command buffers, %u ticks slept)", iSlot, iCtxHigh, iSurfHigh, nMob, s->iCbs, s->iCbSleeps);
		}
	if (iPool)
		iPool->Release();
	if (iArena)
		iArena->Release();
	NKern::ThreadLeaveCS();
	if (iClient)
		Kern::SafeClose((DObject*&)iClient, NULL);
	}

TInt DVmwSvgaChannel::Request(TInt aReqNo, TAny* a1, TAny*)
	{
	if (aReqNo < 0)
		return KErrNone;
	TVmwSvga& s = *TheSvga;
	DThread* const t = &Kern::CurrentThread();
	switch (aReqNo)
		{
	case EVmwCtrlGetInfo:
		{
		TVmwSvgaInfo info;
		memclr(&info, sizeof(info));
		NKern::ThreadEnterCS();
		Kern::MutexWait(*s.iLock);
		info.iVersion = s.iVer; info.iCaps = s.iCaps; info.iCap2 = s.iCap2;
		info.iArenaPhys = iArena->iBlock; info.iArenaSize = iArena->iBytes;
		info.iDevCaps[0] = s.DevCap(0); info.iDevCaps[1] = s.DevCap(95); info.iDevCaps[2] = s.DevCap(244);
		info.iDevCaps[3] = s.DevCap(258); info.iDevCaps[4] = s.DevCap(261); info.iDevCaps[5] = s.DevCap(19);
		info.iMobMaxKb = s.RegRead(KRegMobMaxSize) >> 10;
		info.iDisplayW = s.iModeW;
		info.iDisplayH = s.iModeH;
		Kern::MutexSignal(*s.iLock);
		NKern::ThreadLeaveCS();
		info.iPoolBytes = KPoolBytes;
		info.iMobBase = MobBase(); info.iMobCount = KMobsPerSlot;
		info.iSurfBase = SurfBase(); info.iSurfCount = KSurfsPerSlot;
		info.iCtxBase = CtxBase(); info.iCtxCount = KCtxPerSlot;
		TPckgC<TVmwSvgaInfo> p(info);
		return Kern::ThreadDesWrite(t, a1, p, 0, KChunkShiftBy0, NULL);
		}
	case EVmwCtrlScanout:
		{
		// a1 = 0: the client took its screen target down (the framebuffer shows again: repaint it), 1: it defined one, 2: query
		const TInt op = (TInt)a1;
		if (op == 1)
			s.iScanout = 1;
		else if (op == 0 && s.iScanout)
			{
			s.iScanout = 0;
			const TUint32 full[5] = { KCmdUpdate, 0, 0, (TUint32)s.iModeW, (TUint32)s.iModeH };
			s.FifoWrite(full, 5);
			}
		return s.iScanout;
		}
	case EVmwCtrlGetDevCaps:
		{
		NKern::ThreadEnterCS();
		TVmwDevCaps* caps = (TVmwDevCaps*)Kern::Alloc(sizeof(TVmwDevCaps));
		if (!caps)
			{
			NKern::ThreadLeaveCS();
			return KErrNoMemory;
			}
		Kern::MutexWait(*s.iLock);
		for (TInt i = 0; i < KVmwNumDevCaps; ++i)
			caps->iCaps[i] = s.DevCap(i);
		Kern::MutexSignal(*s.iLock);
		NKern::ThreadLeaveCS();
		TPtrC8 p((const TUint8*)caps, sizeof(TVmwDevCaps));
		TInt r = Kern::ThreadDesWrite(t, a1, p, 0, KChunkShiftBy0, NULL);
		NKern::ThreadEnterCS();
		Kern::Free(caps);
		NKern::ThreadLeaveCS();
		return r;
		}
	case EVmwCtrlOpenArena:
		{
		NKern::ThreadEnterCS();
		TInt h = Kern::MakeHandleAndOpen(NULL, iArena->iChunk);
		NKern::ThreadLeaveCS();
		return h;
		}
	case EVmwCtrlOpenPool:
		{
		NKern::ThreadEnterCS();
		Kern::MutexWait(*s.iLock);
		TInt r = s.EnsureObjectTables();
		if (r == KErrNone && !iPool)
			{
			iPool = TVmwPool::New(KPoolBytes, EFalse);
			if (!iPool)
				r = KErrNoMemory;
			}
		Kern::MutexSignal(*s.iLock);
		TInt h = r;
		if (r == KErrNone)
			h = Kern::MakeHandleAndOpen(NULL, iPool->iChunk);
		NKern::ThreadLeaveCS();
		return h;
		}
	case EVmwCtrlPoolCommit:
		{
		TVmwPoolRange rg;
		TPckg<TVmwPoolRange> p(rg);
		TInt r = Kern::ThreadDesRead(t, a1, p, 0, KChunkShiftBy0);
		if (r != KErrNone)
			return r;
		if (!iPool)
			return KErrNotReady;
		NKern::ThreadEnterCS();
		Kern::MutexWait(*s.iLock);
		r = iPool->Commit(rg.iOffset, rg.iSize);
		Kern::MutexSignal(*s.iLock);
		NKern::ThreadLeaveCS();
		return r;
		}
	case EVmwCtrlDefineMob:
	case EVmwCtrlDestroyMob:
		{
		TVmwMob m;
		TPckg<TVmwMob> p(m);
		TInt r = Kern::ThreadDesRead(t, a1, p, 0, KChunkShiftBy0);
		if (r != KErrNone)
			return r;
		if (!iPool || m.iMobId < MobBase() || m.iMobId >= MobBase() + KMobsPerSlot)
			return KErrArgument;
		const TInt idx = m.iMobId - MobBase();
		NKern::ThreadEnterCS();
		Kern::MutexWait(*s.iLock);
		if (aReqNo == EVmwCtrlDefineMob)
			{
			if (iMobs[idx])
				r = KErrAlreadyExists;
			else if (m.iSize <= 0 || !iPool->Committed(m.iOffset, m.iSize))
				r = KErrArgument;
			else
				{
				TUint32 depth;
				TUint64 base;
				TInt* pt;
				TInt ptc;
				r = s.BuildTable(*iPool, m.iOffset, m.iSize, depth, base, pt, ptc);
				if (r == KErrNone)
					{
					const TUint32 cmd[7] = { KCmdDefineGbMob64, 20, m.iMobId, depth, (TUint32)base, (TUint32)(base >> 32), (TUint32)m.iSize };
					r = s.KernelCmd(cmd, 7, 0);
					TMob* rec = r == KErrNone ? new TMob : NULL;
					if (rec)
						{
						rec->iPtCount = ptc;
						rec->iPtPages = pt;
						iMobs[idx] = rec;
						}
					else
						{
						for (TInt k = 0; k < ptc; ++k)
							s.PtFree(pt[k]);
						Kern::Free(pt);
						if (r == KErrNone)
							r = KErrNoMemory;
						}
					}
				}
			}
		else if (!iMobs[idx])
			r = KErrNotFound;
		else
			{
			const TUint32 cmd[3] = { KCmdDestroyGbMob, 4, m.iMobId };
			r = s.KernelCmd(cmd, 3, 0);
			TMob* rec = iMobs[idx];
			iMobs[idx] = NULL;
			for (TInt k = 0; k < rec->iPtCount; ++k)
				s.PtFree(rec->iPtPages[k]);
			Kern::Free(rec->iPtPages);
			delete rec;
			}
		Kern::MutexSignal(*s.iLock);
		NKern::ThreadLeaveCS();
		return r;
		}
	case EVmwCtrlSubmit:
		{
		TVmwSubmit sub;
		TPckg<TVmwSubmit> p(sub);
		TInt r = Kern::ThreadDesRead(t, a1, p, 0, KChunkShiftBy0);
		if (r != KErrNone)
			return r;
		if (sub.iLength <= 0 || sub.iOffset < 0 || sub.iOffset + sub.iLength > iArena->iBytes || (sub.iOffset & 3))
			return KErrArgument;
		r = s.SubmitChannel(*this, sub);
		if (r != KErrNone)
			return r;
		return Kern::ThreadDesWrite(t, a1, p, 0, KChunkShiftBy0, NULL);
		}
	default:
		return KErrNotSupported;
		}
	}

TInt TVmwSvga::Negotiate()
	{
	for (TInt v = 3; v >= 0; --v)
		{
		Write(KRegId, 0x900000u << 8 | v);
		if (Read(KRegId) == (0x900000u << 8 | v))
			return v;
		}
	return -1;
	}

void TVmwSvga::Report()
	{
	iCaps = Read(KRegCapabilities);
	iCap2 = (iCaps & KCapCap2Reg) ? Read(KRegCap2) : 0;
	Kern::Printf("[svga] SVGA_ID_%d, CAPABILITIES %08x CAP2 %08x", iVer, iCaps, iCap2);
	Kern::Printf("[svga] caps: 3D %d  ExtFifo %d  GMR2 %d  ScreenObj2 %d  CmdBuffers %d/%d  GBOBJECTS(MOB) %d  DX(VGPU10) %d",
		!!(iCaps & KCap3d), !!(iCaps & KCapExtFifo), !!(iCaps & KCapGmr2), !!(iCaps & KCapScreenObj2),
		!!(iCaps & KCapCmdBuffers), !!(iCaps & KCapCmdBuffers2), !!(iCaps & KCapGbObjects), !!(iCaps & KCapDx));
	Kern::Printf("[svga] VRAM %u KB, FIFO mem %u KB at %08x, max primary %u KB, suggested GB mem %u KB, mob max %u KB, screen target max %ux%u",
		Read(KRegVramSize) >> 10, Read(KRegMemSize) >> 10, Read(KRegMemStart), Read(KRegMaxPrimaryMem) >> 10,
		Read(KRegSuggestedGbMemKb), Read(KRegMobMaxSize) >> 10, Read(KRegScreenTargetMaxW), Read(KRegScreenTargetMaxH));
	if (iCaps & KCapGbObjects)
		{
		static const struct { TUint16 iIdx; const char* iName; } KCaps[] = {
			{ 0, "3D" }, { 19, "MAX_TEXTURE_WIDTH" }, { 95, "DXCONTEXT" }, { 244, "SM41" }, { 258, "SM5" }, { 260, "MAX_FORCED_SAMPLE_COUNT" }, { 261, "GL43" } };
		for (TUint i = 0; i < sizeof(KCaps) / sizeof(KCaps[0]); ++i)
			{
			Write(KRegDevCap, KCaps[i].iIdx);
			Kern::Printf("[svga] devcap %s (%d) = %u", KCaps[i].iName, KCaps[i].iIdx, Read(KRegDevCap));
			}
		}
	}

// vmw_fifo_init, trimmed: the FIFO is configured although commands go through command buffers (the device wants it configured)
TInt TVmwSvga::InitFifo()
	{
	const TUint32 fifoPhys = Read(KRegMemStart), fifoSize = Read(KRegMemSize);
	DPlatChunkHw* chunk = NULL;
	TInt r = DPlatChunkHw::New(chunk, fifoPhys, fifoSize, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone)
		return r;
	iFifo = (volatile TUint32*)chunk->LinearAddress();
	iFifoBytes = fifoSize;
	Write(KRegEnable, KEnableEnable | KEnableHide);
	Write(KRegTraces, 0);
	TUint32 minReg = (iCaps & KCapExtFifo) ? Read(KRegMemRegs) : 4;
	const TUint32 min = minReg << 2;
	iFifo[KFifoMin] = min;
	iFifo[KFifoMax] = fifoSize;
	iFifo[KFifoNextCmd] = min;
	iFifo[KFifoStop] = min;
	Write(KRegConfigDone, 1);
	Kern::Printf("[svga] FIFO %u bytes at %08x configured (min %u, caps %08x, reg fence %u), device ENABLE|HIDE",
		fifoSize, fifoPhys, min, iFifo[KFifoCaps], Read(KRegFence));
	return KErrNone;
	}

void TVmwSvga::Submit(TInt aSlot, TUint32 aContext, TInt aLength, TUint32 aFlags, TUint32 aDxContext)
	{
	SCbHeader* h = Hdr(aSlot);
	memclr(h, sizeof(*h));
	h->iFlags = aFlags;					// 0 = interrupt on completion; KCbFlagDxContext + iDxContext for DX commands
	h->iDxContext = aDxContext;
	h->iLength = aLength;
	h->iPaLo = CmdPhys(aSlot);
	h->iPaHi = 0;
	h->iStatus = KCbStatusNone;
	Write(KRegCommandHigh, 0);
	Write(KRegCommandLow, HdrPhys(aSlot) | aContext);
	}

void TVmwSvga::Add(TUint32 aId, const void* aBody, TInt aSize)
	{
	TUint32* h = (TUint32*)((TUint8*)Cmd(iCbSlot) + iCbLen);
	h[0] = aId;
	h[1] = aSize;
	memcpy(h + 2, aBody, aSize);
	iCbLen += 8 + aSize;
	}

// S2: one command buffer that sets up every object table, a MOB, a GB surface, a screen target and shows the CPU-filled image
void TVmwSvga::BuildScreenTarget()
	{
	Begin(2);
	static const TUint32 KOtables[] = { KOtableMob, KOtableSurface, KOtableContext, KOtableShader, KOtableScreenTarget, KOtableDxContext };
	for (TUint i = 0; i < sizeof(KOtables) / sizeof(KOtables[0]); ++i)
		{
		TUint32 b[6];						// SVGA3dCmdSetOTableBase64: type, PPN64, size, validSize, ptDepth
		b[0] = KOtables[i];
		const TUint64 ppn = (TUint64)(iDmaPhys + KOffOtables + i * 0x1000) >> 12;
		b[1] = (TUint32)ppn; b[2] = (TUint32)(ppn >> 32);
		b[3] = 0x1000; b[4] = 0; b[5] = KMobFmtRange;
		Add(KCmdSetOtableBase64, b, 24);
		}
	{
	TUint32 b[5];							// SVGA3dCmdDefineGBMob64: mobid, ptDepth, PPN64, sizeInBytes
	const TUint64 ppn = (TUint64)(iDmaPhys + KOffImage) >> 12;
	b[0] = 1; b[1] = KMobFmtRange; b[2] = (TUint32)ppn; b[3] = (TUint32)(ppn >> 32); b[4] = KImgW * KImgH * 4;
	Add(KCmdDefineGbMob64, b, 20);
	}
	{
	TUint32 b[9];							// SVGA3dCmdDefineGBSurface: sid, flags, format, mips, samples, filter, size w h d
	b[0] = 1; b[1] = KSurfScreenTarget | KSurfBindRenderTarget | KSurfBindShaderResource; b[2] = KSurfFmtB8G8R8A8Unorm; b[3] = 1; b[4] = 0; b[5] = 0; b[6] = KImgW; b[7] = KImgH; b[8] = 1;
	Add(KCmdDefineGbSurface, b, 36);
	}
	{
	TUint32 b[2] = { 1, 1 };				// SVGA3dCmdBindGBSurface: sid, mobid
	Add(KCmdBindGbSurface, b, 8);
	}
	// the picture: a red/blue/green ramp over a white frame, X8R8G8B8
	TUint32* px = (TUint32*)(iDma + KOffImage);
	for (TInt y = 0; y < KImgH; ++y)
		for (TInt x = 0; x < KImgW; ++x)
			{
			TUint32 c = (x < KImgW / 3) ? 0xFF0000u : (x < 2 * KImgW / 3) ? 0x00C000u : 0x0000FFu;
			if (x < 6 || y < 6 || x >= KImgW - 6 || y >= KImgH - 6)
				c = 0xFFFFFFu;
			px[y * KImgW + x] = c | 0xFF000000u;
			}
	{
	TUint32 b[9] = { 1, 0, 0, 0, 0, 0, KImgW, KImgH, 1 };	// SVGA3dCmdUpdateGBImage: image{sid, face, mipmap}, box{x y z w h d}
	Add(KCmdUpdateGbImage, b, 36);
	}
	{
	TUint32 b[7] = { 0, KImgW, KImgH, 0, 0, KStFlagPrimary, 0 };	// DefineGBScreenTarget: stid, w, h, xRoot, yRoot, flags, dpi
	Add(KCmdDefineGbScreenTarget, b, 28);
	}
	{
	TUint32 b[4] = { 0, 1, 0, 0 };			// BindGBScreenTarget: stid, image{sid, face, mipmap}
	Add(KCmdBindGbScreenTarget, b, 16);
	}
	{
	TUint32 b[5] = { 0, 0, 0, KImgW, KImgH };	// UpdateGBScreenTarget: stid, rect{x y w h}
	Add(KCmdUpdateGbScreenTarget, b, 20);
	}
	}

// S2b part 1: MOBs for the context and the 12 COTables, DX_DEFINE_CONTEXT, DX_BIND_CONTEXT, DX_SET_COTABLE x12 (vmwgfx's dx context creation)
void TVmwSvga::BuildDxSetup()
	{
	Begin(4);
	{
	TUint32 b[5];
	const TUint64 ppn = (TUint64)(iDmaPhys + KOffDxCtx) >> 12;
	b[0] = 2; b[1] = KMobFmtRange; b[2] = (TUint32)ppn; b[3] = (TUint32)(ppn >> 32); b[4] = KDxCtxBytes;
	Add(KCmdDefineGbMob64, b, 20);
	}
	for (TInt i = 0; i < KNumCoTables; ++i)
		{
		TUint32 b[5];
		const TUint64 ppn = (TUint64)(iDmaPhys + KOffCoTables + i * 0x1000) >> 12;
		b[0] = 3 + i; b[1] = KMobFmtRange; b[2] = (TUint32)ppn; b[3] = (TUint32)(ppn >> 32); b[4] = 0x1000;
		Add(KCmdDefineGbMob64, b, 20);
		}
	{ TUint32 b[1] = { 0 }; Add(KCmdDxDefineContext, b, 4); }
	{ TUint32 b[3] = { 0, 2, 0 }; Add(KCmdDxBindContext, b, 12); }			// cid, mobid, validContents
	for (TInt i = 0; i < KNumCoTables; ++i)
		{
		TUint32 b[4] = { 0, (TUint32)(3 + i), (TUint32)i, 0 };				// cid, mobid, type, validSizeInBytes
		Add(KCmdDxSetCoTable, b, 16);
		}
	}

// S2b part 2 (a DX-context command buffer): a render target view of surface 1 and the GPU clear of it
void TVmwSvga::BuildDxClear()
	{
	Begin(5);
	{
	TUint32 b[7] = { 0, 1, KSurfFmtB8G8R8A8Unorm, KResTexture2D, 0, 0, 1 };		// rtvId, sid, format, dimension, desc{mipSlice, firstArraySlice, arraySize}
	Add(KCmdDxDefineRenderTargetView, b, 28);
	}
	{
	TUint32 b[5] = { 0, 0x3F800000u, 0x3F000000u, 0x00000000u, 0x3F800000u };	// rtvId, rgba = (1.0, 0.5, 0.0, 1.0): orange
	Add(KCmdDxClearRenderTargetView, b, 20);
	}
	}

// one command buffer out of the channel's arena: the header is ours (a slot per channel), the command PA = arena phys + offset; synchronous
TInt TVmwSvga::SubmitChannel(DVmwSvgaChannel& aCh, TVmwSubmit& aSub)
	{
	const TInt slot = KHdrSlotChannel + aCh.iSlot;
	NKern::ThreadEnterCS();
	Kern::MutexWait(*iLock);
	if (aSub.iSurfHigh > aCh.iSurfHigh)
		aCh.iSurfHigh = aSub.iSurfHigh;
	if (aSub.iCtxHigh > aCh.iCtxHigh)
		aCh.iCtxHigh = aSub.iCtxHigh;
	SCbHeader* h = Hdr(slot);
	memclr(h, sizeof(*h));
	h->iFlags = aSub.iFlags & 3;					// NO_IRQ / DX_CONTEXT only
	h->iDxContext = aSub.iDxContext;
	h->iLength = aSub.iLength;
	h->iPaLo = aCh.iArena->iBlock + aSub.iOffset;
	h->iPaHi = 0;
	h->iStatus = KCbStatusNone;
	Write(KRegCommandHigh, 0);
	Write(KRegCommandLow, HdrPhys(slot) | (aSub.iContext & 0x3f));
	TInt waited = 0, spins = 0;
	const TUint32 tick0 = NKern::TickCount();
	// the host usually answers within microseconds: poll first (a sleep costs a whole timer tick), then give the CPU away
	for (; spins < 4000 && h->iStatus == KCbStatusNone; ++spins)
		Kern::NanoWait(1000);
	while (h->iStatus == KCbStatusNone && waited < 2000)
		{
		NKern::Sleep(NKern::TimerTicks(1));
		++waited;
		}
	++iCbs;
	iCbSleeps += waited;
	iCbSpins += spins;
	iCbTicks += NKern::TickCount() - tick0;
	if (iCbs % 240 == 0)
		{
		Kern::Printf("[svga] %u command buffers: polled %u us (avg %u), slept %u ticks, %u ms waiting in all, %u irqs", iCbs, iCbSpins, iCbSpins / 240, iCbSleeps, iCbTicks, iIrqs);
		iCbSpins = 0; iCbTicks = 0;
		}
	aSub.iStatus = h->iStatus;
	aSub.iErrorOffset = h->iErrorOffset;
	Kern::MutexSignal(*iLock);
	NKern::ThreadLeaveCS();
	return aSub.iStatus == KCbStatusNone ? KErrTimedOut : KErrNone;
	}

void TVmwSvga::Isr(TAny* aPtr)
	{
	TVmwSvga& s = *(TVmwSvga*)aPtr;
	const TUint32 st = Inl(s.iBase + KIrqStatusPort);
	if (!st)
		return;							// a shared line, someone else's
	Outl(s.iBase + KIrqStatusPort, st);	// write-1-to-clear
	++s.iIrqs;
	s.iIrqBits |= st;
	}

// the self-test, one step per timer tick (25 ms apart): submit, wait for the header status, next
void TVmwSvga::StepFn(TAny* aPtr)
	{
	TVmwSvga& s = *(TVmwSvga*)aPtr;
	switch (s.iStep)
		{
	case 0:		// device context: start command-buffer context 0
		{
		TUint32* c = s.Cmd(0);
		c[0] = KDcCmdStartStopContext; c[1] = 1; c[2] = 0;
		s.Submit(0, KCbContextDevice, 12);
		Kern::Printf("[svga] S1: device-context cb submitted (START_STOP_CONTEXT enable ctx 0), header %08x", s.HdrPhys(0));
		s.iStep = 1; s.iWait = 0;
		break;
		}
	case 1:
		if (s.Done(0) || ++s.iWait > 40)
			{
			Kern::Printf("[svga] S1: device-context cb status %d (1 = completed), error offset %u, irqs %d (bits %02x), %d ticks",
				s.Hdr(0)->iStatus, s.Hdr(0)->iErrorOffset, s.iIrqs, s.iIrqBits, s.iWait);
			s.iStep = s.Done(0) ? 2 : 9;
			}
		break;
	case 2:		// context 0: a fence
		{
		s.iFence = 0x1234;
		TUint32* c = s.Cmd(1);
		c[0] = KCmdFence; c[1] = s.iFence;
		s.Submit(1, 0, 8);
		Kern::Printf("[svga] S1: context-0 cb submitted (SVGA_CMD_FENCE %u)", s.iFence);
		s.iStep = 3; s.iWait = 0;
		break;
		}
	case 3:
		if (s.Done(1) || ++s.iWait > 40)
			{
			const TUint32 regFence = s.Read(KRegFence), fifoFence = s.iFifo[KFifoFence];
			const TBool cbOk = s.Hdr(1)->iStatus == KCbStatusCompleted;
			const TBool fenceOk = regFence == s.iFence || fifoFence == s.iFence;
			Kern::Printf("[svga] S1: context-0 cb status %d, error offset %u, fence: reg %u fifo %u (want %u), irqs %d (bits %02x), %d ticks",
				s.Hdr(1)->iStatus, s.Hdr(1)->iErrorOffset, regFence, fifoFence, s.iFence, s.iIrqs, s.iIrqBits, s.iWait);
			if (cbOk && fenceOk)
				Kern::Printf("[svga] S1: command buffer completed + fence signalled");
			else
				Kern::Printf("[svga] S1: FAILED (cb %s, fence %s)", cbOk ? "ok" : "not completed", fenceOk ? "ok" : "not seen");
#ifdef VMWSVGA_KERNEL_S2
			s.iStep = cbOk ? 4 : 9;
#else
			s.iStep = 9;
#endif
			}
		break;
#ifdef VMWSVGA_KERNEL_S2
	case 4:		// S2: object tables + MOB + GB surface + screen target, one command buffer
		s.BuildScreenTarget();
		Kern::Printf("[svga] S2: cb submitted (6 object tables, MOB 1, GB surface 1 %dx%d, screen target 0), %d bytes", KImgW, KImgH, s.iCbLen);
		s.Send(0);
		s.iStep = 5; s.iWait = 0;
		break;
	case 5:
		if (s.Done(2) || ++s.iWait > 80)
			{
			Kern::Printf("[svga] S2: cb status %d (1 = completed), error offset %u, irqs %d (bits %02x)",
				s.Hdr(2)->iStatus, s.Hdr(2)->iErrorOffset, s.iIrqs, s.iIrqBits);
			if (s.Hdr(2)->iStatus == KCbStatusCompleted)
				{
				Kern::Printf("[svga] S2: screen target shown");
				s.iStep = 6; s.iWait = 0;
				}
			else
				s.iStep = 9;
			}
		break;
	case 6:		// the CPU-filled ramp is up: after ~8 s the GPU path (S2b): a DX context ...
		if (++s.iWait > 320)
			{
			s.BuildDxSetup();
			Kern::Printf("[svga] S2b: DX context setup cb (13 MOBs, DX_DEFINE_CONTEXT, DX_BIND_CONTEXT, 12 x DX_SET_COTABLE), %d bytes", s.iCbLen);
			s.Send(0);
			s.iStep = 7; s.iWait = 0;
			}
		break;
	case 7:
		if (s.Done(4) || ++s.iWait > 80)
			{
			Kern::Printf("[svga] S2b: DX setup cb status %d, error offset %u, irqs %d", s.Hdr(4)->iStatus, s.Hdr(4)->iErrorOffset, s.iIrqs);
			if (s.Hdr(4)->iStatus == KCbStatusCompleted)
				{
				s.BuildDxClear();
				s.SendDx(0);
				Kern::Printf("[svga] S2b: DX-context cb submitted (DEFINE_RENDERTARGET_VIEW, CLEAR_RENDERTARGET_VIEW orange), %d bytes", s.iCbLen);
				s.iStep = 8; s.iWait = 0;
				}
			else
				s.iStep = 9;
			}
		break;
	case 8:
		if (s.Done(5) || ++s.iWait > 80)
			{
			Kern::Printf("[svga] S2b: DX clear cb status %d, error offset %u, irqs %d", s.Hdr(5)->iStatus, s.Hdr(5)->iErrorOffset, s.iIrqs);
			if (s.Hdr(5)->iStatus == KCbStatusCompleted)
				{
				s.Begin(6);
				TUint32 b[5] = { 0, 0, 0, KImgW, KImgH };			// UPDATE_GB_SCREENTARGET: present the GPU-rendered surface
				s.Add(KCmdUpdateGbScreenTarget, b, 20);
				s.Send(0);
				Kern::Printf("[svga] S2b: GPU clear done + screen target updated");
				s.iStep = 10; s.iWait = 0;
				}
			else
				s.iStep = 9;
			}
		break;
	case 10:	// hold ~30 s more, then take the screen target down
		if (++s.iWait > 1200)
			{
			s.Begin(3);
			TUint32 b[1] = { 0 };
			s.Add(KCmdDestroyGbScreenTarget, b, 4);
			s.Send(0);
			Kern::Printf("[svga] S2: screen target destroyed");
			s.iStep = 9;
			}
		break;
#endif
		}
	if (s.iStep != 9)
		s.iTimer.OneShot(NKern::TimerTicks(25));
	}

// ---------------------------------------------------------------------------------------------------------------------------------------
// Scan-out (the display's side, xk_display.cpp). Once SVGA_REG_ENABLE is set the device -- not the Bochs VBE "DISPI" registers -- owns the mode:
// the host takes the size from SVGA_REG_WIDTH/HEIGHT/BITS_PER_PIXEL and the pitch from SVGA_REG_BYTES_PER_LINE, and DISPI writes are ignored (a
// DISPI mode change after ENABLE leaves the host showing the old size at the wrong pitch). And the host window only repaints rectangles the guest
// reports with SVGA_CMD_UPDATE through the FIFO (the legacy "update this rectangle of the framebuffer" command of the SVGA II programming model):
// writing pixels into the framebuffer is not announced by itself. So on this device the display sets every mode through the registers and
// reports each flush's dirty rectangle here.

// The legacy FIFO (svga_reg.h SVGA_FIFO_*): a ring of dwords between MIN and MAX bytes, the guest writes at NEXT_CMD, the device consumes up to STOP.
// A command may be written once there is room for all of it and one dword to spare (NEXT_CMD must never catch up with STOP: equal means empty).
TBool TVmwSvga::FifoWrite(const TUint32* aWords, TInt aCount)
	{
	const TUint32 bytes = aCount * 4;
	const TUint32 min = iFifo[KFifoMin], max = iFifo[KFifoMax];
	TUint32 next = iFifo[KFifoNextCmd];
	for (TInt tries = 0; ; ++tries)
		{
		const TUint32 stop = iFifo[KFifoStop];
		const TUint32 room = (stop > next) ? stop - next - 4 : (max - next) + (stop - min) - 4;
		if (room >= bytes)
			break;
		if (tries >= 20)
			return EFalse;						// the device is not consuming: the caller keeps its dirty rectangle and tries again
		++iFifoFullWaits;
		Ping();									// wake the device's FIFO processing, then give it a tick
		NKern::Sleep(NKern::TimerTicks(1));
		}
	for (TInt i = 0; i < aCount; ++i)
		{
		iFifo[next >> 2] = aWords[i];
		next += 4;
		if (next >= max)
			next = min;
		}
	iFifo[KFifoNextCmd] = next;					// publish: the FIFO memory is mapped uncached, so the words above are already visible
	Ping();
	return ETrue;
	}

// vmw_fifo_ping_host: tell the device there is something in the FIFO (once per busy period -- the device clears FIFO_BUSY when it has drained it)
void TVmwSvga::Ping()
	{
	if (!iFifo[KFifoBusy])
		{
		iFifo[KFifoBusy] = 1;
		Write(KRegSync, KSyncGeneric);
		}
	}

// A client (Mesa's svga driver, a full-screen GL window) made its render target the display: a screen target replaces the whole VM display
// while it exists (S2), so what WSERV composes into the framebuffer is invisible. The display driver is the arbiter of what is on screen:
// the moment it reports a rectangle (WSERV drew something: a note, a status change, a window that is not full-screen any more, the
// pointer), the screen target goes and the host shows the framebuffer again -- all of it, repainted here. The client sees iScanout drop
// to 0 (EVmwCtrlScanout, query) and presents through the window server until it takes the display over again.
void TVmwSvga::ScanoutDrop()
	{
	NKern::ThreadEnterCS();
	Kern::MutexWait(*iLock);
	if (iScanout)
		{
		const TUint32 cmd[3] = { KCmdDestroyGbScreenTarget, 4, 0 };
		KernelCmd(cmd, 3, 0);
		iScanout = 0;
		++iScanoutDrops;
		if (iScanoutDrops <= 8)
			Kern::Printf("[svga] scan-out dropped (#%u): the window server drew something; the host shows the framebuffer again", iScanoutDrops);
		}
	Kern::MutexSignal(*iLock);
	NKern::ThreadLeaveCS();
	const TUint32 full[5] = { KCmdUpdate, 0, 0, (TUint32)iModeW, (TUint32)iModeH };
	FifoWrite(full, 5);
	}

TBool TVmwSvga::Update(TInt aX, TInt aY, TInt aW, TInt aH)
	{
	if (iProbe <= 0 || !iModeSet || aW <= 0 || aH <= 0)
		return ETrue;
	if (iScanout)
		ScanoutDrop();							// something the CPU composed changed on screen: the GPU frame is no longer the whole picture
	const TUint32 cmd[5] = { KCmdUpdate, (TUint32)aX, (TUint32)aY, (TUint32)aW, (TUint32)aH };
	if (!FifoWrite(cmd, 5))
		{
		++iUpdatesDropped;
		return EFalse;
		}
	++iUpdates;
	return ETrue;
	}

// Program a mode through the registers (vmw_kms_write_svga): size and depth, then ENABLE without HIDE; read back what the device made of it.
// aExpectPhys != 0: the framebuffer address the caller already mapped -- a different address (FB_START + FB_OFFSET) refuses the mode.
TBool TVmwSvga::SetMode(TInt aW, TInt aH, TInt& aPitch, TUint32& aFbPhys, TUint32 aExpectPhys)
	{
	if (iProbe <= 0)
		return EFalse;
	const TUint32 oldW = Read(KRegWidth), oldH = Read(KRegHeight), oldBpp = Read(KRegBitsPerPixel);
	const TBool wasSet = iModeSet;
	Write(KRegEnable, 0);						// the device derives its pitch and display size when it is enabled, not when the registers are written
	Write(KRegWidth, aW);
	Write(KRegHeight, aH);
	Write(KRegBitsPerPixel, 32);
	Write(KRegEnable, KEnableEnable);			// SVGA mode, display shown
	const TUint32 w = Read(KRegWidth), h = Read(KRegHeight), bpp = Read(KRegBitsPerPixel), pitch = Read(KRegBytesPerLine);
	const TUint32 phys = Read(KRegFbStart) + Read(KRegFbOffset);
	Kern::Printf("[svga] mode %ux%u -> %ux%ux%u pitch %u, framebuffer %08x (VRAM %u KB)", aW, aH, w, h, bpp, pitch, phys, Read(KRegVramSize) >> 10);
	if ((TInt)w != aW || (TInt)h != aH || bpp != 32 || pitch < w * 4u || (aExpectPhys && phys != aExpectPhys))
		{
		Kern::Printf("[svga] mode refused (size, depth, pitch or framebuffer address differ): back to %ux%ux%u", oldW, oldH, oldBpp);
		Write(KRegWidth, oldW);
		Write(KRegHeight, oldH);
		Write(KRegBitsPerPixel, oldBpp);
		Write(KRegEnable, wasSet ? KEnableEnable : KEnableEnable | KEnableHide);
		return EFalse;
		}
	iModeW = (TInt)w;
	iModeH = (TInt)h;
	iModeSet = ETrue;
	aPitch = pitch;
	aFbPhys = phys;
	return ETrue;
	}

// The device lookup, ID negotiation, capability report and FIFO set-up -- everything that needs no memory beyond the FIFO. Idempotent: the display
// driver runs it first (it must know before it picks the framebuffer path), X64StartVmwSvga then finishes the 3D side.
TInt TVmwSvga::Probe()
	{
	if (iProbe)
		return iProbe > 0 ? KErrNone : iProbe;
	iLock = NULL; iBase = 0; iIrqs = 0; iIrqBits = 0; iStep = 0; iWait = 0; iFence = 0; iVer = -1;
	iDev = -1;
	for (TInt d = 0; d < 32 && iDev < 0; ++d)
		if (PciRead(d, 0) == 0x040515AD)
			iDev = d;
	if (iDev < 0)
		{
		Kern::Printf("[svga] no VMware SVGA II (15AD:0405) device -- no SVGA3D on this machine");
		return iProbe = KErrNotFound;
		}
	const TUint32 bar0 = PciRead(iDev, 0x10), bar1 = PciRead(iDev, 0x14), bar2 = PciRead(iDev, 0x18);
	PciWrite(iDev, 0x04, PciRead(iDev, 0x04) | 0x7);					// I/O + memory decode + bus master
	iBase = bar0 & ~3u;
	Kern::Printf("[svga] 15AD:0405 at 00:%02x.0 rev %02x subsys %08x, BAR0(io) %08x BAR1(fb) %08x BAR2(fifo) %08x",
		iDev, PciRead(iDev, 8) & 0xFF, PciRead(iDev, 0x2C), bar0, bar1, bar2);
	if (!(bar0 & 1))
		return iProbe = KErrNotSupported;
	iVer = Negotiate();			// the value port is base+1 (a dword at an unaligned port); registers read 0 until the ID is accepted
	if (iVer < 0)
		{
		Kern::Printf("[svga] ID negotiation failed");
		return iProbe = KErrNotSupported;
		}
	Report();
	TInt r = InitFifo();
	if (r != KErrNone)
		return iProbe = r;
	iProbe = 1;
	return KErrNone;
	}

TInt TVmwSvga::Init()
	{
	TInt r = Probe();
	if (r != KErrNone)
		return r;
	if (!(iCaps & KCapCmdBuffers) || !(iCaps & KCapGbObjects))
		{
		Kern::Printf("[svga] no command buffers / guest-backed objects: S1 not applicable");
		return KErrNotSupported;
		}
	r = Epoc::AllocPhysicalRam(KDmaBytes, iDmaPhys);
	DPlatChunkHw* dma = NULL;
	if (r == KErrNone)
		r = DPlatChunkHw::New(dma, iDmaPhys, KDmaBytes, EMapAttrSupRw | EMapAttrCachedMax);
	if (r != KErrNone)
		return r;
	iDma = (TUint8*)dma->LinearAddress();
	memclr(iDma, KDmaBytes);
	const TInt line = PciRead(iDev, 0x3C) & 0xFF;
	const TInt irq = (line > 0 && line < 16) ? (EX64IrqPciRouted | line) : -1;
	const TInt h = irq >= 0 ? Interrupt::Bind(irq, Isr, this) : KErrNotSupported;
	if (h >= 0)
		{
		Write(KRegIrqMask, KIrqCommandBuffer | KIrqAnyFence | KIrqError);
		Interrupt::Enable(h);										// by HANDLE: the pin may be shared
		}
	Kern::Printf("[svga] irq: PIRQ-routed %d (bind %d), mask %02x", line, h, Read(KRegIrqMask));
	_LIT(KMutexName, "VmwSvgaLock");
	r = Kern::MutexCreate(iLock, KMutexName, KMutexOrdGeneral0);
	if (r != KErrNone)
		return r;
	DVmwSvgaDevice* dev = new DVmwSvgaDevice;
	if (dev)
		{
		r = Kern::InstallLogicalDevice(dev);
		Kern::Printf("[svga] logical device VmwSvga installed (r=%d), arena %u KB at phys %08x", r, KDmaBytes >> 10, iDmaPhys);
		}
	iStep = 0;
	iTimer.OneShot(NKern::TimerTicks(50));
	return KErrNone;
	}

static TVmwSvga* EnsureSvga()
	{
	if (!TheSvga)
		TheSvga = new TVmwSvga;
	return TheSvga;
	}

void X64StartVmwSvga()
	{
	if (EnsureSvga())
		TheSvga->Init();				// logs why it stops; the object stays (its timer is the self-test)
	}

// ---- the display driver's interface (xk_display.cpp) ----
// X64SvgaProbe: is there a VMware SVGA II device (VirtualBox VMSVGA, VMware, QEMU -vga vmware) the display must drive through its registers?
TBool X64SvgaProbe()
	{
	return EnsureSvga() && TheSvga->Probe() == KErrNone;
	}

TBool X64SvgaSetMode(TInt aW, TInt aH, TInt& aPitch, TUint32& aFbPhys, TUint32 aExpectPhys)
	{
	return TheSvga && TheSvga->SetMode(aW, aH, aPitch, aFbPhys, aExpectPhys);
	}

// one SVGA_CMD_UPDATE; EFalse = the FIFO had no room (the caller keeps the rectangle and reports it again with the next one)
TBool X64SvgaUpdate(TInt aX, TInt aY, TInt aW, TInt aH)
	{
	return !TheSvga || TheSvga->Update(aX, aY, aW, aH);
	}

// bounded statistics for the display's periodic monitor line: updates sent, updates the FIFO could not take, waits for room, FIFO drained by the device
void X64SvgaStats(TUint32& aSent, TUint32& aDropped, TUint32& aFullWaits, TBool& aDrained)
	{
	aSent = aDropped = aFullWaits = 0;
	aDrained = ETrue;
	if (TheSvga && TheSvga->iProbe > 0)
		{
		aSent = TheSvga->iUpdates;  aDropped = TheSvga->iUpdatesDropped;  aFullWaits = TheSvga->iFifoFullWaits;
		aDrained = TheSvga->iFifo[KFifoStop] == TheSvga->iFifo[KFifoNextCmd];
		}
	}
