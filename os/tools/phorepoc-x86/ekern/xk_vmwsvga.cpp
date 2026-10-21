// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_vmwsvga.cpp -- Path C (2026-10-16): rungs S0 + S1 of. The VMware SVGA II / SVGA3D PCI device
// (15AD:0405) -- what VMware Workstation exposes and what VirtualBox's `--graphicscontroller vmsvga` emulates (with
// `--accelerate3d on` the 3D command set behind it).
//
// S0: negotiate the register interface (SVGA_ID_3 at port base+1) and log the capability registers and the 3D device caps the Mesa
//     `svga` winsys asks the kernel for (DRM_VMW_GET_PARAM): guest-backed objects (MOBs), DX contexts (VGPU10 = GLES 3.0), SM5 (GL 4.1),
//     GL43 (compute = GLES 3.1).
// S1: "the device answers" -- the kernel half of the command path, as vmwgfx does it (vmwgfx_cmdbuf.c / vmwgfx_fifo.c), with our own code:
//       * FIFO memory (BAR2) mapped and initialised (MIN/MAX/NEXT_CMD/STOP), SVGA_REG_ENABLE = ENABLE|HIDE (the device is on but the
//         display stays on the VBE linear framebuffer the loader set up), TRACES off, CONFIG_DONE;
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
	KRegId = 0, KRegEnable = 1, KRegVramSize = 15, KRegCapabilities = 17, KRegMemStart = 18, KRegMemSize = 19, KRegConfigDone = 20,
	KRegMemRegs = 30, KRegIrqMask = 33, KRegTraces = 45, KRegCommandLow = 48, KRegCommandHigh = 49, KRegMaxPrimaryMem = 50,
	KRegSuggestedGbMemKb = 51, KRegDevCap = 52, KRegScreenTargetMaxW = 55, KRegScreenTargetMaxH = 56, KRegMobMaxSize = 57,
	KRegCap2 = 59, KRegFifoCaps = 68, KRegFence = 69, KRegGbMemSizeKb = 76,
	KCap3d = 0x4000, KCapExtFifo = 0x8000, KCapGmr2 = 0x400000, KCapScreenObj2 = 0x800000, KCapCmdBuffers = 0x1000000,
	KCapCmdBuffers2 = 0x4000000, KCapGbObjects = 0x8000000, KCapDx = 0x10000000, KCapCap2Reg = 0x80000000u,
	KEnableEnable = 1, KEnableHide = 2,
	KIrqAnyFence = 1 << 0, KIrqCommandBuffer = 1 << 3, KIrqError = 1 << 4,
	KFifoMin = 0, KFifoMax = 1, KFifoNextCmd = 2, KFifoStop = 3, KFifoCaps = 4, KFifoFence = 6, KFifoNumRegs = 291,
	KFifoCapFence = 1,
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
	TUint32 DevCap(TUint32 aIdx)			{ Write(KRegDevCap, aIdx); return Read(KRegDevCap); }
	TInt SubmitFromArena(TVmwSubmit& aSub);
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
	TVmwSvga() : iTimer(StepFn, this) {}
	};
static TVmwSvga* TheSvga;

// ---------------------------------------------------------------------------------------------------------------------------------------
// S2: the user API -- the logical device "VmwSvga" (rvmwsvgachannel.h), the Symbian counterpart of the DRM_VMW_* ioctls the Mesa `svga` winsys
// uses. Same shape as RMaliChannel: the channel hands the client the ARENA (the 2 MB physically contiguous DMA block as a shared chunk, so the
// client writes MOB contents, object tables and command streams straight into it), and Submit runs one command buffer out of it. The kernel owns the
// command-buffer HEADERS (a client cannot forge one) and bounds-checks the command range. No validation of the command stream itself (a single
// trusted channel, as the Mali driver). The device is installed from the extension entry: Open needs no User::LoadLogicalDevice.

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
	DThread* iClient;
	DChunk* iArena;
	TUint32 iArenaPhys;
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
	if (!TheSvga || !TheSvga->iDma || !Kern::QueryVersionSupported(TVersion(KVmwSvgaMajorVersion, 0, 0), aVer))
		return KErrNotSupported;
	iClient = &Kern::CurrentThread();
	iClient->Open();
	TChunkCreateInfo info;
	info.iType = TChunkCreateInfo::ESharedKernelMultiple;
	info.iMaxSize = KDmaBytes;
	info.iMapAttr = EMapAttrCachedMax;				// x86 DMA is cache-coherent
	info.iOwnsMemory = EFalse;						// the RAM is the driver's contiguous block, not the chunk's
	info.iDestroyedDfc = NULL;
	TLinAddr lin;
	TUint32 attr;
	TInt r = Kern::ChunkCreate(info, iArena, lin, attr);
	if (r != KErrNone)
		return r;
	r = Kern::ChunkCommitPhysical(iArena, 0, KDmaBytes, TheSvga->iDmaPhys);
	iArenaPhys = TheSvga->iDmaPhys;
	return r;
	}

DVmwSvgaChannel::~DVmwSvgaChannel()
	{
	if (iArena)
		Kern::ChunkClose(iArena);
	if (iClient)
		Kern::SafeClose((DObject*&)iClient, NULL);
	}

TInt DVmwSvgaChannel::Request(TInt aReqNo, TAny* a1, TAny*)
	{
	if (aReqNo < 0)
		return KErrNone;
	TVmwSvga& s = *TheSvga;
	switch (aReqNo)
		{
	case EVmwCtrlGetInfo:
		{
		TVmwSvgaInfo info;
		memclr(&info, sizeof(info));
		info.iVersion = s.iVer; info.iCaps = s.iCaps; info.iCap2 = s.iCap2;
		info.iArenaPhys = iArenaPhys; info.iArenaSize = KDmaBytes;
		info.iDevCaps[0] = s.DevCap(0); info.iDevCaps[1] = s.DevCap(95); info.iDevCaps[2] = s.DevCap(244);
		info.iDevCaps[3] = s.DevCap(258); info.iDevCaps[4] = s.DevCap(261); info.iDevCaps[5] = s.DevCap(19);
		TPckgC<TVmwSvgaInfo> p(info);
		return Kern::ThreadDesWrite(iClient, a1, p, 0, KChunkShiftBy0, NULL);
		}
	case EVmwCtrlOpenArena:
		{
		NKern::ThreadEnterCS();
		TInt h = Kern::MakeHandleAndOpen(NULL, iArena);
		NKern::ThreadLeaveCS();
		return h;
		}
	case EVmwCtrlSubmit:
		{
		TVmwSubmit sub;
		TPckg<TVmwSubmit> p(sub);
		TInt r = Kern::ThreadDesRead(iClient, a1, p, 0, KChunkShiftBy0);
		if (r != KErrNone)
			return r;
		if (sub.iLength <= 0 || sub.iOffset < 0 || sub.iOffset + sub.iLength > KDmaBytes || (sub.iOffset & 3))
			return KErrArgument;
		r = s.SubmitFromArena(sub);
		if (r != KErrNone)
			return r;
		return Kern::ThreadDesWrite(iClient, a1, p, 0, KChunkShiftBy0, NULL);
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

// one command buffer out of the client's arena: the header is ours (slot 8), the command PA = arena phys + offset; synchronous
TInt TVmwSvga::SubmitFromArena(TVmwSubmit& aSub)
	{
	const TInt slot = 8;
	NKern::ThreadEnterCS();
	Kern::MutexWait(*iLock);
	SCbHeader* h = Hdr(slot);
	memclr(h, sizeof(*h));
	h->iFlags = aSub.iFlags & 3;					// NO_IRQ / DX_CONTEXT only
	h->iDxContext = aSub.iDxContext;
	h->iLength = aSub.iLength;
	h->iPaLo = iDmaPhys + aSub.iOffset;
	h->iPaHi = 0;
	h->iStatus = KCbStatusNone;
	Write(KRegCommandHigh, 0);
	Write(KRegCommandLow, HdrPhys(slot) | (aSub.iContext & 0x3f));
	TInt waited = 0;
	while (h->iStatus == KCbStatusNone && waited < 2000)
		{
		NKern::Sleep(NKern::TimerTicks(1));
		++waited;
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

TInt TVmwSvga::Init()
	{
	iLock = NULL; iBase = 0; iIrqs = 0; iIrqBits = 0; iStep = 0; iWait = 0; iFence = 0; iVer = -1;
	iDev = -1;
	for (TInt d = 0; d < 32 && iDev < 0; ++d)
		if (PciRead(d, 0) == 0x040515AD)
			iDev = d;
	if (iDev < 0)
		{
		Kern::Printf("[svga] no VMware SVGA II (15AD:0405) device -- no SVGA3D on this machine");
		return KErrNotFound;
		}
	const TUint32 bar0 = PciRead(iDev, 0x10), bar1 = PciRead(iDev, 0x14), bar2 = PciRead(iDev, 0x18);
	PciWrite(iDev, 0x04, PciRead(iDev, 0x04) | 0x7);					// I/O + memory decode + bus master
	iBase = bar0 & ~3u;
	Kern::Printf("[svga] 15AD:0405 at 00:%02x.0 rev %02x subsys %08x, BAR0(io) %08x BAR1(fb) %08x BAR2(fifo) %08x",
		iDev, PciRead(iDev, 8) & 0xFF, PciRead(iDev, 0x2C), bar0, bar1, bar2);
	if (!(bar0 & 1))
		return KErrNotSupported;
	iVer = Negotiate();			// the value port is base+1 (a dword at an unaligned port); registers read 0 until the ID is accepted
	if (iVer < 0)
		{
		Kern::Printf("[svga] ID negotiation failed");
		return KErrNotSupported;
		}
	Report();
	if (!(iCaps & KCapCmdBuffers) || !(iCaps & KCapGbObjects))
		{
		Kern::Printf("[svga] no command buffers / guest-backed objects: S1 not applicable");
		return KErrNotSupported;
		}
	TInt r = InitFifo();
	if (r != KErrNone)
		return r;
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

void X64StartVmwSvga()
	{
	TheSvga = new TVmwSvga;
	if (TheSvga)
		TheSvga->Init();				// logs why it stops; the object stays (its timer is the self-test)
	}
