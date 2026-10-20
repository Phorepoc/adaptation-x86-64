// xk_absmouse.cpp -- the Phorepoc amd64 board's ABSOLUTE pointer: "mouse integration". X4.35.
//
// A PS/2 mouse is relative, so a hypervisor has to GRAB the host pointer to feed it (VirtualBox: the
// pointer is captured by the VM window until the host key; the keyboard goes with it) and the guest's
// pointer never lines up with the host's. What the guest additions of every hypervisor do instead is
// tell the host "I can take absolute coordinates": the host then stops grabbing, and the pointer simply
// is wherever the host pointer is over the VM window. Two such devices, one per hypervisor we run on:
//
//   VirtualBox  the VMMDev PCI device (80EE:CAFE), the guest-additions device itself. A request is a
//               struct in guest RAM whose PHYSICAL address is written to the device's I/O port
//               (BAR0 + 0); the host processes it synchronously and writes the status into its header.
//               ReportGuestInfo (the additions announce themselves; the host refuses the rest until
//               then), SetMouseStatus(GUEST_CAN_ABSOLUTE | GUEST_NEEDS_HOST_CURSOR), then
//               GetMouseStatus = the position, 0..0xFFFF across the guest display, while the host has
//               HOST_WANTS_ABSOLUTE set (the user can switch integration off: then PS/2 is relative
//               again). Buttons still arrive as PS/2 packets.
//   QEMU        the VMware "vmmouse" behind the backdoor port 0x5658 (QEMU's pc machine has it, so do
//   (+VMware)   VMware's): ENABLE, REQUEST_ABSOLUTE, then 4-word packets (buttons, x, y, wheel) from its
//               own queue, 0..0xFFFF. QEMU nudges the PS/2 mouse by one count per packet as a wake-up;
//               the PS/2 bytes carry nothing else while absolute.
//
// The i8042 driver calls X64AbsMouseRead on every PS/2 packet (that is when buttons change) and, for
// motion, polls every 20 ms on DfcQue0 -- except on VirtualBox once the VMMDev interrupt is found
// (X4.43, below): then motion is interrupt-driven and the poll is a 500 ms safety net. On a real PC
// neither device answers and the PS/2 mouse stays relative as before.
#include <kernel/kern_priv.h>
#include <assp.h>
#include "xk_interrupt.h"
#include "xk_absmouse.h"

static inline TUint32 Inl(TUint16 aPort)
	{ TUint32 v; __asm__ __volatile__("in eax, dx" : "=a"(v) : "d"(aPort)); return v; }
static inline void Outl(TUint16 aPort, TUint32 aVal)
	{ __asm__ __volatile__("out dx, eax" : : "d"(aPort), "a"(aVal) : "memory"); }
static inline void Outw(TUint16 aPort, TUint16 aVal)
	{ __asm__ __volatile__("out dx, ax" : : "d"(aPort), "a"(aVal)); }

// ---- PCI configuration mechanism #1 -------------------------------------------------------------------
static TUint32 PciRead(TInt aBus, TInt aDev, TInt aFn, TInt aReg)
	{
	Outl(0xCF8, 0x80000000u | aBus << 16 | aDev << 11 | aFn << 8 | (aReg & 0xFC));
	return Inl(0xCFC);
	}
static void PciWrite16(TInt aBus, TInt aDev, TInt aFn, TInt aReg, TUint16 aVal)
	{
	Outl(0xCF8, 0x80000000u | aBus << 16 | aDev << 11 | aFn << 8 | (aReg & 0xFC));
	Outw(0xCFC + (aReg & 2), aVal);
	}

// ---- VirtualBox VMMDev (VBox/VMMDev.h; the values are the host's ABI) --------------------------------
static const TUint32 KVmmDevId = 0xCAFE80EEu;			// device << 16 | vendor
static const TUint32 KReqHeaderVersion = 0x10001;
static const TUint32 KVmmDevVersion = 0x00010004;		// VMMDEV_VERSION 1.4
enum { EReqGetMouseStatus = 1, EReqSetMouseStatus = 2, EReqGetHostTime = 10, EReqAcknowledgeEvents = 41,
		EReqCtlGuestFilterMask = 42, EReqReportGuestInfo = 50, EReqGetDisplayChangeRequest = 51,
		EReqReportGuestCapabilities = 55 };
static const TUint32 KEventMousePositionChanged = 1u << 9;	// VMMDEV_EVENT_MOUSE_POSITION_CHANGED
static const TUint32 KEventDisplayChangeRequest = 1u << 2;	// VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST (X4.45)
static const TUint32 KGuestSupportsGraphics = 1u << 2;		// VMMDEV_GUEST_SUPPORTS_GRAPHICS: "I can resize"
enum { EGuestCanAbsolute = 1<<0, EHostWantsAbsolute = 1<<1, EGuestNeedsHostCursor = 1<<2,
		EGuestNewProtocol = 1<<4 };						// "new protocol": the host raises MOUSE_POSITION_CHANGED (X4.43)

struct SReqHeader { TUint32 iSize, iVersion, iType; TInt32 iRc; TUint32 iReserved1, iReserved2; };
struct SReqMouse { SReqHeader iHdr; TUint32 iFeatures; TInt32 iX, iY; };
struct SReqGuestInfo { SReqHeader iHdr; TUint32 iInterfaceVersion, iOsType; };

static DPlatChunkHw* TheReqChunk;						// one page, below 4 GiB: the request buffer
static TUint32 TheReqPhys;
static TUint16 TheVmmPort;

static TInt VmmRequest(TUint32 aType, const TAny* aBody, TInt aBodySize, TAny* aOut, TInt aBufOff = 0)
	{
	SReqHeader* h = (SReqHeader*)(TheReqChunk->LinearAddress() + aBufOff);	// aBufOff: the ISR's own buffer
	h->iSize = sizeof(SReqHeader) + aBodySize;
	h->iVersion = KReqHeaderVersion;
	h->iType = aType;
	h->iRc = -1;										// VERR_GENERAL_FAILURE until the host says otherwise
	h->iReserved1 = h->iReserved2 = 0;
	memcpy(h + 1, aBody, aBodySize);
	Outl(TheVmmPort, TheReqPhys + aBufOff);				// synchronous: done when the OUT returns
	if (aOut)
		memcpy(aOut, h + 1, aBodySize);
	return h->iRc;
	}

// X4.81 (user: "I can't see any mouse cursor in VirtualBox"): with mouse integration VirtualBox hides its own
// pointer over the VM until the guest hands it a SHAPE -- GUEST_NEEDS_HOST_CURSOR alone left it invisible, and our
// software cursor steps aside in absolute mode. What Guest Additions do: VMMDevReq_SetPointerShape (type 3) --
// flags VISIBLE|SHAPE, hot spot, size, then a 1-bpp AND mask (rows byte-padded, the mask padded to 4 bytes;
// 1 = transparent) followed by 32-bpp BGRA colour. Our arrow (xk_display.cpp KCursor), so both paths look the same.
extern const char* const* X64CursorBitmap(TInt& aW, TInt& aH);	// xk_display.cpp
static const TUint32 KReqSetPointerShape = 3;
static const TUint32 KPointerVisible = 0x1, KPointerShape = 0x4;

static TInt SetPointerShape()
	{
	TInt w, h;
	const char* const* bmp = X64CursorBitmap(w, h);
	const TInt maskRow = (w + 7) / 8;
	const TInt maskSize = (maskRow * h + 3) & ~3;
	const TInt bodySize = 5 * 4 + maskSize + w * h * 4;
	TUint8 body[5 * 4 + 64 + 12 * 19 * 4];
	if (bodySize > (TInt)sizeof(body))
		return KErrOverflow;
	memset(body, 0, sizeof(body));
	TUint32* hdr = (TUint32*)body;
	hdr[0] = KPointerVisible | KPointerShape;
	hdr[1] = 0;											// hot spot: the arrow's tip
	hdr[2] = 0;
	hdr[3] = (TUint32)w;
	hdr[4] = (TUint32)h;
	TUint8* mask = body + 5 * 4;
	TUint32* colour = (TUint32*)(mask + maskSize);
	for (TInt y=0; y<h; ++y)
		for (TInt x=0; x<w; ++x)
			{
			const char c = bmp[y][x];
			if (c == '.')
				mask[y * maskRow + x / 8] |= (TUint8)(0x80 >> (x & 7));	// transparent
			else
				colour[y * w + x] = c == 'o' ? 0x00FFFFFFu : 0x00000000u;
			}
	return VmmRequest(KReqSetPointerShape, body, bodySize, NULL);
	}

static TInt TheVmmDevSlot;
static TBool VmmDevInit()
	{
	TInt bus = -1, dev = 0;
	for (TInt d=0; d<32 && bus<0; ++d)					// bus 0 is where VirtualBox puts it (00:04.0)
		if (PciRead(0, d, 0, 0) == KVmmDevId)
			bus = 0, dev = d;
	if (bus < 0)
		return EFalse;
	const TUint32 bar0 = PciRead(bus, dev, 0, 0x10);
	if (!(bar0 & 1))
		return EFalse;
	TheVmmPort = (TUint16)(bar0 & 0xFFFC);
	TheVmmDevSlot = dev;
	PciWrite16(bus, dev, 0, 0x04, (TUint16)(PciRead(bus, dev, 0, 0x04) | 0x3));	// I/O + memory decode on
	TPhysAddr phys;
	TInt r = Epoc::AllocPhysicalRam(0x1000, phys);
	if (r == KErrNone)
		r = DPlatChunkHw::New(TheReqChunk, phys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone)
		return EFalse;
	TheReqPhys = phys;
	TUint32 info[2] = { KVmmDevVersion, 0 /* VBOXOSTYPE_Unknown */ };
	TInt rcInfo = VmmRequest(EReqReportGuestInfo, info, sizeof(info), NULL);
	TUint32 mouse[3] = { EGuestCanAbsolute | EGuestNeedsHostCursor | EGuestNewProtocol, 0, 0 };
	TInt rcSet = VmmRequest(EReqSetMouseStatus, mouse, sizeof(mouse), NULL);
	TInt rcShape = SetPointerShape();
	Kern::Printf("[absmouse] VirtualBox VMMDev at 00:%02x.0, port %04x: ReportGuestInfo %d, SetMouseStatus %d, "
					"SetPointerShape %d", dev, TheVmmPort, rcInfo, rcSet, rcShape);
	return rcSet == 0;
	}

static TInt VmmDevRead(SAbsPacket* aOut)
	{
	TUint32 mouse[3] = { 0, 0, 0 };
	if (VmmRequest(EReqGetMouseStatus, mouse, sizeof(mouse), mouse) != 0)
		return 0;
	aOut->iAbsolute = (mouse[0] & EHostWantsAbsolute) != 0;
	aOut->iX = mouse[1];
	aOut->iY = mouse[2];
	aOut->iButtons = -1;								// the PS/2 stream carries them
	return 1;
	}

// ---- the VMware backdoor vmmouse (QEMU hw/i386/vmmouse.c) ---------------------------------------------
static const TUint32 KBdMagic = 0x564D5868, KBdPort = 0x5658;
enum { EBdGetVersion = 10, EBdAbsData = 39, EBdAbsStatus = 40, EBdAbsCommand = 41 };
static const TUint32 KVmmEnable = 0x45414552, KVmmRequestAbsolute = 0x53424152, KVmmVersionId = 0x3442554a;

static void Backdoor(TUint32 aCmd, TUint32 aArg, TUint32* aRegs)	// aRegs = eax, ebx, ecx, edx after
	{
	TUint32 a = KBdMagic, b = aArg, c = aCmd, d = KBdPort;
	__asm__ __volatile__("in eax, dx" : "+a"(a), "+b"(b), "+c"(c), "+d"(d) : : "memory");
	aRegs[0] = a;  aRegs[1] = b;  aRegs[2] = c;  aRegs[3] = d;
	}

static TBool VmMouseInit()
	{
	TUint32 r[4];
	Backdoor(EBdGetVersion, ~KBdMagic, r);
	if (r[1] != KBdMagic)								// nothing behind the port
		return EFalse;
	Backdoor(EBdAbsCommand, KVmmEnable, r);
	Backdoor(EBdAbsStatus, 0, r);
	if ((r[0] & 0xFFFF0000u) == 0xFFFF0000u || (r[0] & 0xFFFF) == 0)
		return EFalse;
	Backdoor(EBdAbsData, 1, r);
	if (r[0] != KVmmVersionId)
		return EFalse;
	Backdoor(EBdAbsCommand, KVmmRequestAbsolute, r);
	Kern::Printf("[absmouse] VMware backdoor vmmouse (QEMU/VMware): absolute");
	return ETrue;
	}

static TInt VmMouseRead(SAbsPacket* aOut, TInt aMax)
	{
	TInt n = 0;
	while (n < aMax)
		{
		TUint32 r[4];
		Backdoor(EBdAbsStatus, 0, r);
		if ((r[0] & 0xFFFF0000u) == 0xFFFF0000u)			// the device reset its queue: enable again
			{
			VmMouseInit();
			break;
			}
		if ((r[0] & 0xFFFF) < 4)
			break;
		Backdoor(EBdAbsData, 4, r);
		const TUint32 f = r[0];
		SAbsPacket& p = aOut[n++];
		p.iAbsolute = !(f & 0x0001);					// VMMOUSE_RELATIVE_PACKET
		p.iX = r[1];
		p.iY = r[2];
		p.iButtons = ((f & 0x20) ? 1 : 0) | ((f & 0x10) ? 2 : 0) | ((f & 0x08) ? 4 : 0);	// PS/2 layout
		}
	return n;
	}

// ---- the interface for xk_keyboard.cpp -----------------------------------------------------------------
static TInt TheKind;

// ---- the VMMDev interrupt (X4.43, reworked X4.47) ------------------------------------------------------
// The VMMDev raises its PCI INTA# (level) while an event in the guest's filter mask is pending, until the
// guest acknowledges it (AcknowledgeEvents). Which IOAPIC pin that line reaches is the chipset's routing,
// which ACPI's _PRT describes and we do not parse. So it is found at run time. The candidates: the ISA
// IRQ the firmware's PIRQ router gave it (its PCI interrupt-line register, bound as EX64IrqPciRouted),
// and the PCI pins GSI 16..23, all with the ISR below; the one that sees an event is kept.
// The line is SHARED (VirtualBox puts the e1000 on it too), so -- as the real VBoxGuest driver does --
// the ISR acknowledges in place, through its own request buffer (second half of the request page; the
// DFC's requests use the first): no event = not ours, return; events = the line drops, they are noted
// for the DFC (X64AbsMouseIrqService, on DfcQue0), which reads the mouse / display-change request.
// Nothing is ever masked, and every Enable/Unbind is by HANDLE (a pin number would mean all its handlers).
static const TInt KMaxCand = 9;
static const TInt KIsrReqOff = 0x800;					// the ISR's request buffer, in the same page
static TInt TheCand[KMaxCand];							// candidate Interrupt::Bind ids
static TInt TheHandle[KMaxCand];						// their handles (>0), or 0
static TInt TheNumCand;
static TDfc* TheIrqDfc;
static volatile TInt TheIrq = -1;						// the settled candidate, or -1
static volatile TUint32 ThePendingEvents;				// acknowledged by the ISR, not yet serviced
static volatile TInt TheWinner = -1;								// set by the first ISR that saw an event

static void VmmIsr(TAny* aIndex)
	{
	const TInt i = (TInt)(TLinAddr)aIndex;
	TUint32 ev = 0;
	VmmRequest(EReqAcknowledgeEvents, &ev, sizeof(ev), &ev, KIsrReqOff);
	if (!ev)
		return;											// another device on a shared line
	__e32_atomic_ior_ord32(&ThePendingEvents, ev);
	if (TheIrq < 0 && TheWinner < 0)
		TheWinner = i;									// benign race: any pin that saw an event is it
	TheIrqDfc->Add();
	}

TInt X64AbsMouseBindIrq(TDfc* aDfc)
	{
	if (TheKind != EAbsVmmDev)
		return KErrNotSupported;
	TUint32 mask[2] = { KEventMousePositionChanged | KEventDisplayChangeRequest, 0 };	// or-mask, not-mask
	if (VmmRequest(EReqCtlGuestFilterMask, mask, sizeof(mask), NULL) != 0)
		return KErrGeneral;
	// X4.45: tell the host we can change the display size: it then sends the VM window's size
	// (auto-resize guest display) as display-change requests
	TUint32 caps = KGuestSupportsGraphics;
	TInt rcCaps = VmmRequest(EReqReportGuestCapabilities, &caps, sizeof(caps), NULL);
	TheIrqDfc = aDfc;
	const TInt line = PciRead(0, TheVmmDevSlot, 0, 0x3C) & 0xFF;
	if (line > 0 && line < 16)
		TheCand[TheNumCand++] = EX64IrqPciRouted | line;
	for (TInt g=16; g<16+8; ++g)
		TheCand[TheNumCand++] = g;
	for (TInt i=0; i<TheNumCand; ++i)
		{
		const TInt h = Interrupt::Bind(TheCand[i], VmmIsr, (TAny*)(TLinAddr)i);
		TheHandle[i] = h > 0 ? h : 0;
		if (h > 0)
			Interrupt::Enable(h);
		}
	Kern::Printf("[absmouse] VMMDev: position + display-change events on (graphics capability %d); "
					"its interrupt: PIRQ-routed IRQ %d or GSI 16..23 (probing)", rcCaps, line);
	return KErrNone;
	}

TInt X64AbsMouseIrqService()
	{
	if (TheIrq < 0 && TheWinner >= 0)
		{
		TheIrq = TheWinner;
		for (TInt j=0; j<TheNumCand; ++j)						// the other probes are done with
			if (j != TheIrq && TheHandle[j])
				{
				Interrupt::Unbind(TheHandle[j]);
				TheHandle[j] = 0;
				}
		const TInt id = TheCand[TheIrq];
		Kern::Printf("[absmouse] VMMDev interrupt = %s %d: motion is interrupt-driven",
						id & EX64IrqPciRouted ? "PIRQ-routed ISA IRQ" : "GSI", id & ~EX64IrqPciRouted);
		}
	const TUint32 ev = __e32_atomic_swp_ord32(&ThePendingEvents, 0);
	if (ev && TheIrq >= 0)
		{
		static TInt serviced = 0;
		if (++serviced == 10)									// once: proof the line keeps working
			Kern::Printf("[absmouse] 10 VMMDev interrupts serviced");
		}
	if (ev & KEventDisplayChangeRequest)
		{
		// X4.45: the host window's size; eventAck tells the host this request has been seen
		TUint32 req[4] = { 0, 0, 0, KEventDisplayChangeRequest };	// xres, yres, bpp, eventAck
		if (VmmRequest(EReqGetDisplayChangeRequest, req, sizeof(req), req) == 0 && req[0] && req[1])
			{
			extern void X64DisplayRequestMode(TInt aW, TInt aH);	// xk_display.cpp
			X64DisplayRequestMode((TInt)req[0], (TInt)req[1]);
			}
		}
	return TheIrq;
	}

// The host's clock (X4.42, xk_timesync.cpp): VMMDevReq_GetHostTime = ms since 1970-01-01 UTC. Only once
// the VMMDev is set up (ReportGuestInfo done); call on DfcQue0, where the mouse polling uses the buffer.
TInt X64VmmHostTimeMs(TInt64& aMs)
	{
	if (TheKind != EAbsVmmDev)
		return KErrNotSupported;
	TUint32 t[2] = { 0, 0 };
	if (VmmRequest(EReqGetHostTime, t, sizeof(t), t) != 0)
		return KErrGeneral;
	aMs = MAKE_TINT64(t[1], t[0]);
	return KErrNone;
	}

TInt X64AbsMouseInit()
	{
	NKern::ThreadEnterCS();
	TheKind = VmmDevInit() ? EAbsVmmDev : VmMouseInit() ? EAbsVmMouse : EAbsNone;
	NKern::ThreadLeaveCS();
	return TheKind;
	}

TInt X64AbsMouseRead(SAbsPacket* aOut, TInt aMax)
	{
	if (aMax <= 0)
		return 0;
	switch (TheKind)
		{
	case EAbsVmmDev:	return VmmDevRead(aOut);
	case EAbsVmMouse:	return VmMouseRead(aOut, aMax);
	default:			return 0;
		}
	}
