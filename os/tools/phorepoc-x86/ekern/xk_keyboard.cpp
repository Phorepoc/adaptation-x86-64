// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_keyboard.cpp -- the Phorepoc amd64 board's PS/2 controller (i8042): keyboard AND mouse. X4.18,
// interrupt-driven since X4.21.
//
// Ours, like xk_display.cpp: the 2010 drop has no x86 baseport. The shape is the Syborg keyboard
// extension (adapt.Qemu baseport/syborg/keyboard/syborg_keyboard.cpp) -- bytes in, TRawEvent
// EKeyDown/EKeyUp through Kern::AddEvent, an EHalGroupKeyboard HAL entry -- and its scancode table
// is used AS IS: syborg's KConvertCode is indexed by PC scancode SET 1, which is what an i8042 in
// its default translated mode delivers. Set-1 details handled here: bit 7 = break (key up), and an
// 0xE0 prefix for the grey keys, whose low 7 bits are the keypad key they duplicate (E0 48 = the
// grey up arrow = keypad 8/up), so the prefix is simply dropped.
//
// Interrupts (X4.21): ISA irq 1 (keyboard) and 12 (aux = mouse) through the IOAPIC (xk_interrupt.cpp).
// Both run ONE ISR, which drains the controller into a ring -- the status register's AUX bit says
// whose byte each one is, whichever line fired -- and queues a DFC on DfcQue0 (Kern::AddEvent takes a
// DMutex: it needs a DThread). The polled version lost the tail of any burst longer than its buffer.
//
// Mouse: a standard 3-byte PS/2 stream (defaults, then "enable data reporting"). Relative motion is
// integrated into an absolute position clamped to the framebuffer, and delivered the way WSERV wants
// a mouse (UserHal::MachineInfo -> EXYInputMouse, because EHalGroupMouse answers EMouseHalMouseInfo):
// EPointerMove at the new position, EButton1/2/3Down/Up at the current one.
//
// X4.35: the position is tracked in FRAMEBUFFER pixels and reported in SCREEN pixels (the display may
// scale the screen, xk_display.cpp), and a hypervisor's absolute pointer (xk_absmouse.cpp: VirtualBox's
// VMMDev, QEMU's vmmouse) replaces the PS/2 motion when there is one -- "mouse integration": the host
// no longer grabs the pointer. Its position is polled on each PS/2 packet and every KAbsPollMs.
#include <kernel/kern_priv.h>
#include <e32keys.h>
#include <assp.h>
#include "xk_board.h"
#include "xk_interrupt.h"
#include "xk_absmouse.h"

// The PC set-1 -> EPOC standard-key table, verbatim from the Syborg baseport: make_ekern.sh cuts
// the initializer out of syborg_keyboard.h (the header itself drags in Syborg's class/includes).
static const TUint8 KConvertCode[] =
#include "xk_kbdtable.inc"
;

static inline TUint8 Inb(TUint16 aPort)
	{ TUint8 v; __asm__ __volatile__("in al, dx" : "=a"(v) : "d"(aPort)); return v; }
static inline void Outb(TUint16 aPort, TUint8 aVal)
	{ __asm__ __volatile__("out dx, al" : : "d"(aPort), "a"(aVal)); }

static const TUint16 KI8042Data = 0x60, KI8042Status = 0x64, KI8042Cmd = 0x64;
static const TUint8 KStatusOutputFull = 0x01, KStatusInputFull = 0x02, KStatusAuxData = 0x20;
static const TUint8 KCfgKbdIrq = 0x01, KCfgAuxIrq = 0x02, KCfgKbdClockOff = 0x10, KCfgAuxClockOff = 0x20;
static const TInt KRingSize = 256;			// power of two
static const TInt KOutSize = 8;
static const TInt KRetryMs = 20;
static const TInt KAbsPollMs = 20;
static const TInt KAbsIrqPollMs = 500;		// X4.43: with the VMMDev interrupt working

extern TInt X64Scale, X64ScreenOx, X64ScreenOy, X64FbW, X64FbH;	// xk_display.cpp (X4.35)
extern void X64SetCursorPos(TInt aX, TInt aY);		// xk_display.cpp: the software cursor (X4.33)

class DPcI8042
	{
public:
	DPcI8042() : iDfc(EventDfc, this, 1), iRetry(RetryIsr, this), iAbsDfc(AbsDfc, this, 1), iAbsTimer(AbsTimerIsr, this) {}
	TInt Create();
	TBool Mouse() const						{ return iMouse; }
	static TInt KeyboardHal(TAny* aPtr, TInt aFunction, TAny* a1, TAny* a2);
	static TInt MouseHal(TAny* aPtr, TInt aFunction, TAny* a1, TAny* a2);
private:
	static void Isr(TAny* aPtr);
	static void EventDfc(TAny* aPtr);
	static void RetryIsr(TAny* aPtr);
	static void AbsDfc(TAny* aPtr);
	static void AbsTimerIsr(TAny* aPtr);
	void AbsPoll();
	void PointerTo(TInt aFx, TInt aFy);
	void Buttons(TUint8 aNow);
	void Emit(const TRawEvent& aEvent);
	TBool Flush();
	void KeyByte(TUint8 aByte);
	void MouseByte(TUint8 aByte);
	TBool InitMouse();
private:
	TDfc iDfc;
	NTimer iRetry;							// event queue full: try again in KRetryMs
	TDfc iAbsDfc;
	NTimer iAbsTimer;
	TInt iAbsKind;							// TAbsMouseKind: which absolute device answered
	TBool iAbsActive;						// the host is giving absolute positions now
	TRawEvent iOut[KOutSize];				// one byte makes at most 4 events (a mouse packet)
	TInt iOutCount;
	volatile TUint16 iRing[KRingSize];		// byte | 0x100 = from the aux (mouse) port
	volatile TUint32 iHead;					// written by the ISR only
	volatile TUint32 iTail;					// written by the DFC only
	volatile TUint32 iDropped;
	TBool iExtended;
	TBool iOn;
	TBool iMouse;
	TUint8 iPacket[3];
	TInt iPacketLen;
	TInt iX, iY, iWidth, iHeight;			// the pointer and the mouse area, in SCREEN pixels
	TInt iFx, iFy;							// the pointer in framebuffer pixels
	TUint8 iButtons;
	TInt iTraced;
	TInt iButtonTraced;
public:
	TInt iIrqCount;
	TInt iStalls;							// times the kernel event queue was full
	};

static DPcI8042* TheI8042;

// Bounded controller handshakes (boot time, thread context, before the interrupts are enabled).
static TBool WaitWrite()
	{
	for (TInt i=0; i<100000; ++i)
		if (!(Inb(KI8042Status) & KStatusInputFull))
			return ETrue;
	return EFalse;
	}
static TInt ReadByte()
	{
	for (TInt i=0; i<20; ++i)
		{
		for (TInt j=0; j<10000; ++j)
			if (Inb(KI8042Status) & KStatusOutputFull)
				return Inb(KI8042Data);
		Kern::NanoWait(100000);				// 0.1 ms
		}
	return -1;
	}
static void Command(TUint8 aCmd)				{ WaitWrite(); Outb(KI8042Cmd, aCmd); }
static void Data(TUint8 aByte)					{ WaitWrite(); Outb(KI8042Data, aByte); }

// the aux device: 0xD4 routes the next data byte to it; it answers 0xFA (ACK)
static TBool AuxCommand(TUint8 aByte)
	{
	Command(0xD4);
	Data(aByte);
	return ReadByte() == 0xFA;
	}

TBool DPcI8042::InitMouse()
	{
	Command(0xA8);							// enable the aux port
	if (!AuxCommand(0xF6))					// set defaults (100 samples/s, 4 counts/mm, stream mode)
		return EFalse;
	return AuxCommand(0xF4);				// enable data reporting
	}

TInt DPcI8042::Create()
	{
	iOn = ETrue;
	iDfc.SetDfcQ(Kern::DfcQue0());
	extern TInt X64ScreenW, X64ScreenH;		// xk_display.cpp: the screen the HAL reports (X4.26)
	iWidth = X64ScreenW ? X64ScreenW : 640;
	iHeight = X64ScreenH ? X64ScreenH : 480;
	iX = iWidth/2;  iY = iHeight/2;
	iFx = X64ScreenOx + iX*X64Scale;  iFy = X64ScreenOy + iY*X64Scale;
	if (!X64FbW)
		X64FbW = iWidth, X64FbH = iHeight, X64Scale = X64Scale ? X64Scale : 1;

	// quiesce: both ports' interrupts off while we talk to the controller, drain what the firmware left
	Command(0x20);
	TInt cfg = ReadByte();
	if (cfg < 0)
		cfg = 0x45;							// translate + system flag + kbd irq: the usual default
	Command(0x60);  Data((TUint8)(cfg & ~(KCfgKbdIrq|KCfgAuxIrq)));
	for (TInt i=0; i<32 && (Inb(KI8042Status) & KStatusOutputFull); ++i)
		(void)Inb(KI8042Data);
	iMouse = InitMouse();
	if (iMouse)
		{
		iAbsKind = X64AbsMouseInit();
		iAbsDfc.SetDfcQ(Kern::DfcQue0());
		}
	for (TInt i=0; i<32 && (Inb(KI8042Status) & KStatusOutputFull); ++i)
		(void)Inb(KI8042Data);

	TInt r = Kern::AddHalEntry(EHalGroupKeyboard, KeyboardHal, this);
	if (r == KErrNone && iMouse)
		r = Kern::AddHalEntry(EHalGroupMouse, MouseHal, this);
	if (r != KErrNone)
		return r;
	extern TInt X64SwitchHal(TAny*, TInt, TAny*, TAny*);
	(void)Kern::AddHalEntry(EHalGroupPlatformSpecific1, X64SwitchHal, this);	// X4.82: the keyboard "slide" = open

	TInt hk = Interrupt::Bind(EX64IrqKeyboard, Isr, this);
	if (hk < 0)
		return hk;
	TInt hm = 0;
	if (iMouse && (hm = Interrupt::Bind(EX64IrqMouse, Isr, this)) < 0)
		return hm;

	cfg = (cfg | KCfgKbdIrq) & ~KCfgKbdClockOff;
	if (iMouse)
		cfg = (cfg | KCfgAuxIrq) & ~KCfgAuxClockOff;
	Command(0x60);  Data((TUint8)cfg);
	Interrupt::Enable(hk);
	if (iMouse)
		Interrupt::Enable(hm);
	if (iAbsKind == EAbsVmmDev)
		X64AbsMouseBindIrq(&iAbsDfc);		// X4.43: VMMDev motion by interrupt, once its pin is seen
	if (iAbsKind != EAbsNone)
		iAbsTimer.OneShot(NKern::TimerTicks(KAbsPollMs));
	return KErrNone;
	}

// ISR (either line): everything the controller has, into the ring. Never blocks.
void DPcI8042::Isr(TAny* aPtr)
	{
	DPcI8042& k = *(DPcI8042*)aPtr;
	++k.iIrqCount;
	for (TInt n=0; n<32; ++n)
		{
		TUint8 st = Inb(KI8042Status);
		if (!(st & KStatusOutputFull))
			break;
		TUint16 b = Inb(KI8042Data);
		if (st & KStatusAuxData)
			b |= 0x100;
		TUint32 h = k.iHead;
		if (h - k.iTail >= (TUint32)KRingSize)
			{
			++k.iDropped;
			continue;
			}
		k.iRing[h & (KRingSize-1)] = b;
		__e32_memory_barrier();
		k.iHead = h + 1;
		}
	k.iDfc.Add();
	}

// Back-pressure: the kernel's raw-event queue is a small fixed ring (sinit.cpp KMaxEventQueue) and
// Kern::AddEvent just says KErrOverflow when WSERV has not caught up -- the Syborg driver ignored that,
// and a burst then lost key-UPs (a key auto-repeated for ever). So events wait in iOut, the bytes
// behind them stay in iRing, and a retry timer re-runs this DFC until the queue has room.
void DPcI8042::Emit(const TRawEvent& aEvent)
	{
	if (iOutCount < KOutSize)
		iOut[iOutCount++] = aEvent;
	}

TBool DPcI8042::Flush()
	{
	TInt done = 0;
	while (done < iOutCount && Kern::AddEvent(iOut[done]) == KErrNone)
		++done;
	for (TInt i=done; i<iOutCount; ++i)
		iOut[i-done] = iOut[i];
	iOutCount -= done;
	return iOutCount == 0;
	}

void DPcI8042::RetryIsr(TAny* aPtr)
	{
	((DPcI8042*)aPtr)->iDfc.Add();
	}

void DPcI8042::EventDfc(TAny* aPtr)
	{
	DPcI8042& k = *(DPcI8042*)aPtr;
	for (;;)
		{
		if (!k.Flush())
			{
			if (++k.iStalls <= 5)					// bounded diagnostics
				Kern::Printf("[i8042] event queue full: stall %d, %d events + %d bytes waiting, %d irqs",
								k.iStalls, k.iOutCount, k.iHead - k.iTail, k.iIrqCount);
			k.iRetry.OneShot(NKern::TimerTicks(KRetryMs));	// ISR callback: only queues this DFC again
			return;
			}
		if (k.iTail == k.iHead)
			return;
		__e32_memory_barrier();
		TUint16 b = k.iRing[k.iTail & (KRingSize-1)];
		__e32_memory_barrier();
		k.iTail = k.iTail + 1;
		if (b & 0x100)
			k.MouseByte((TUint8)b);
		else
			k.KeyByte((TUint8)b);
		}
	}

void DPcI8042::KeyByte(TUint8 b)
	{
	if (b == 0xE0)
		{
		iExtended = ETrue;
		return;
		}
	if (b == 0xE1 || b == 0xFA || b == 0xFE || b == 0x00 || b == 0xFF)
		{
		iExtended = EFalse;					// pause prefix / controller acks / overrun
		return;
		}
	TBool up = b & 0x80;
	TUint code = b & 0x7F;
	const TBool extended = iExtended;
	iExtended = EFalse;
	if (!iOn)
		return;
	// (audio U5) the media keys a PC keyboard (and QEMU's volumeup/volumedown) sends as E0-prefixed codes of their own
	// -- not grey duplicates of keypad keys: with the prefix dropped, Volume Up typed a 'B' (0x30) and Volume Down a
	// 'C' (0x2E). The phone's volume keys instead.
	// Any other E0 code outside the grey keys (keypad Enter and /, right Ctrl/Alt, Print Screen, the navigation
	// block, the Windows/Menu keys) is a media/ACPI key with no phone equivalent: ignored rather than typed.
	TInt std = EStdKeyNull;
	if (extended && code == 0x30)
		std = EStdKeyIncVolume;
	else if (extended && code == 0x2E)
		std = EStdKeyDecVolume;
	else if (extended && !(code == 0x1C || code == 0x1D || code == 0x35 || code == 0x37 || code == 0x38
						   || (code >= 0x47 && code <= 0x53) || (code >= 0x5B && code <= 0x5D)))
		std = EStdKeyNull;
	else if (code < sizeof(KConvertCode))
		std = KConvertCode[code];
	// board keymap (2026-09-29): F9 = a SHORT press of the phone's power key (the ACPI power button stays the
	// 3 s long press = switch off, xk_power.cpp) -- SysAp's power menu
	if (!extended && std == EStdKeyF9)
		std = EStdKeyDevice2;
	// F10 = the phone's Menu key (EStdKeyApplication0): a press opens the launcher, a long press (akncapserver's
	// KHomeHoldDelay) the task switcher
	else if (!extended && std == EStdKeyF10)
		std = EStdKeyApplication0;
	if (std == EStdKeyNull)
		return;
	TRawEvent e;
	e.Set(up ? TRawEvent::EKeyUp : TRawEvent::EKeyDown, std, 0);
	Emit(e);
	}

void DPcI8042::MouseByte(TUint8 b)
	{
	if (iPacketLen == 0 && !(b & 0x08))
		return;								// byte 0 always has bit 3 set: resynchronise
	iPacket[iPacketLen++] = b;
	if (iPacketLen < 3)
		return;
	iPacketLen = 0;
	const TUint8 f = iPacket[0];
	if (iAbsKind != EAbsNone)
		AbsPoll();							// X4.35: the host's position first -- a click lands there
	if (iAbsActive)
		{
		if (iAbsKind == EAbsVmmDev)			// VirtualBox: motion via VMMDev, buttons via PS/2
			Buttons(f & 0x07);
		return;								// QEMU's vmmouse: this packet is only its wake-up nudge
		}
	if (f & 0xC0)
		return;								// X/Y overflow: drop the packet
	TInt dx = iPacket[1] - ((f & 0x10) ? 256 : 0);
	TInt dy = iPacket[2] - ((f & 0x20) ? 256 : 0);
	if (dx || dy)
		PointerTo(iFx + dx, iFy - dy);		// PS/2 y grows upwards, the screen's downwards
	if (iTraced < 6)							// bounded: the first few packets, to see the path work
		{
		++iTraced;
		Kern::Printf("[i8042] mouse packet %02x %02x %02x -> (%d,%d) buttons %x", f, iPacket[1], iPacket[2], iX, iY, f & 7);
		}
	Buttons(f & 0x07);
	}

// the pointer to framebuffer pixel (aFx,aFy): clamped to the framebuffer, reported in screen pixels
// (a position in the border around a scaled screen clamps to its edge)
void DPcI8042::PointerTo(TInt aFx, TInt aFy)
	{
	iFx = aFx < 0 ? 0 : (aFx >= X64FbW ? X64FbW-1 : aFx);
	iFy = aFy < 0 ? 0 : (aFy >= X64FbH ? X64FbH-1 : aFy);
	TInt x = iFx < X64ScreenOx ? 0 : (iFx - X64ScreenOx) / X64Scale;
	TInt y = iFy < X64ScreenOy ? 0 : (iFy - X64ScreenOy) / X64Scale;
	x = x >= iWidth ? iWidth-1 : x;
	y = y >= iHeight ? iHeight-1 : y;
	// the host draws its own pointer while the mouse is absolute (VMMDev: GUEST_NEEDS_HOST_CURSOR)
	X64SetCursorPos(iAbsActive ? -1 : iFx, iAbsActive ? -1 : iFy);
	if (x == iX && y == iY)
		return;
	iX = x;  iY = y;
	TRawEvent e;
	e.Set(TRawEvent::EPointerMove, iX, iY, 0);	// Z = 0 explicitly: Set(x, y) leaves it stack garbage (see a64touch.cpp)
	Emit(e);
	}

void DPcI8042::Buttons(TUint8 aNow)
	{
	static const TRawEvent::TType KDown[3] = { TRawEvent::EButton1Down, TRawEvent::EButton2Down, TRawEvent::EButton3Down };
	static const TRawEvent::TType KUp[3]   = { TRawEvent::EButton1Up,   TRawEvent::EButton2Up,   TRawEvent::EButton3Up };
	static const TUint8 KBit[3] = { 0x01, 0x04, 0x02 };		// left, middle, right -> button 1, 2, 3
	for (TInt i=0; i<3; ++i)
		{
		if ((aNow ^ iButtons) & KBit[i])
			{
			TRawEvent e;
			e.Set((aNow & KBit[i]) ? KDown[i] : KUp[i], iX, iY, 0);
			Emit(e);
			if (iButtonTraced < 16)				// bounded: where the clicks land (X4.32)
				{
				++iButtonTraced;
				Kern::Printf("[i8042] button %d %s at (%d,%d)", i+1, (aNow & KBit[i]) ? "down" : "up", iX, iY);
				}
			}
		}
	iButtons = aNow;
	}

// X4.35: the absolute device's news -- each packet is a position (framebuffer-relative, 0..0xFFFF) and,
// from QEMU's vmmouse, the buttons. Only when the event queue has room: what stays in the device waits.
void DPcI8042::AbsPoll()
	{
	for (TInt n=0; n<16; ++n)
		{
		if (!Flush() || iOutCount > KOutSize - 4)
			return;
		SAbsPacket p;
		if (X64AbsMouseRead(&p, 1) == 0)
			return;
		if (p.iAbsolute != iAbsActive)
			{
			iAbsActive = p.iAbsolute;
			Kern::Printf("[i8042] pointer is %s", iAbsActive ? "ABSOLUTE (host integration)" : "relative (PS/2)");
			}
		if (p.iAbsolute)
			{
			PointerTo((TInt)(((TUint64)p.iX * X64FbW) >> 16), (TInt)(((TUint64)p.iY * X64FbH) >> 16));
			if (p.iButtons >= 0)
				Buttons((TUint8)p.iButtons);
			}
		if (iAbsKind == EAbsVmmDev)				// VMMDev reports a state, not a queue
			break;
		}
	Flush();
	}

void DPcI8042::AbsTimerIsr(TAny* aPtr)
	{
	((DPcI8042*)aPtr)->iAbsDfc.Add();
	}

void DPcI8042::AbsDfc(TAny* aPtr)
	{
	DPcI8042& k = *(DPcI8042*)aPtr;
	// X4.43: the same DFC serves the VMMDev interrupt and the poll timer. Once the interrupt carries
	// motion, the timer is only a safety net (KAbsIrqPollMs).
	const TBool irq = k.iAbsKind == EAbsVmmDev && X64AbsMouseIrqService() >= 0;
	k.AbsPoll();
	k.iAbsTimer.Cancel();
	k.iAbsTimer.OneShot(NKern::TimerTicks(irq ? KAbsIrqPollMs : KAbsPollMs));
	}

TInt DPcI8042::KeyboardHal(TAny* aPtr, TInt aFunction, TAny* a1, TAny* /*a2*/)
	{
	DPcI8042& k = *(DPcI8042*)aPtr;
	switch (aFunction)
		{
	case EKeyboardHalKeyboardInfo:
		{
		TPckgBuf<TKeyboardInfoV01> v;
		v().iKeyboardType = EKeyboard_Full;
		v().iDeviceKeys = 0;
		v().iAppsKeys = 0;
		Kern::InfoCopy(*(TDes8*)a1, v);
		return KErrNone;
		}
	case EKeyboardHalSetKeyboardState:
		k.iOn = a1 != NULL;
		return KErrNone;
	case EKeyboardHalKeyboardState:
		kumemput32(a1, &k.iOn, sizeof(TBool));
		return KErrNone;
	default:
		return KErrNotSupported;
		}
	}

// X4.82: S60's hardware-switch HAL group (EHalGroupPlatformSpecific1 = 29; SysApAppUi.cpp KHWSwitchGroup /
// KHWSwitchGrip = 2). A keyboard-slide phone reports its slide here and SysAp publishes it at startup as
// KHWRMGripStatus; AknCapServer then turns on QWERTY input with the layout in KAknKeyboardSlideOpen (repository
// 101F876E, board file = 4x12). This board's keyboard is a PC keyboard -- never "closed": grip status 0 = open.
// (Without it the FEP stayed in keypad mode, where Shift alone is the pencil/edit key -- user, 2026-09-26.)
static const TInt KX64HalSwitchGrip = 2;
TInt X64SwitchHal(TAny* /*aPtr*/, TInt aFunction, TAny* a1, TAny* /*a2*/)
	{
	if (aFunction != KX64HalSwitchGrip)
		return KErrNotSupported;
	// SLIDE=1 (2026-10-08): report the slide CLOSED -- a touch-only phone, as the PinePhone: the FEP's QWERTY mode
	// is off and an editor tap should open the virtual keyboard (the VKB investigation)
	const TInt open = XK_SLIDE_CLOSED ? 1 : 0;
	kumemput32(a1, &open, sizeof(TInt));
	return KErrNone;
	}

TInt DPcI8042::MouseHal(TAny* aPtr, TInt aFunction, TAny* a1, TAny* /*a2*/)
	{
	DPcI8042& k = *(DPcI8042*)aPtr;
	switch (aFunction)
		{
	case EMouseHalMouseInfo:
		{
		TPckgBuf<TMouseInfoV01> v;
		v().iMouseButtons = 3;
		v().iOffsetToDisplay.iX = 0;			// (kernel-side TPoint/TSize have no 2-argument ctors)
		v().iOffsetToDisplay.iY = 0;
		v().iMouseAreaSize.iWidth = k.iWidth;
		v().iMouseAreaSize.iHeight = k.iHeight;
		Kern::InfoCopy(*(TDes8*)a1, v);
		return KErrNone;
		}
	case EMouseHalMouseState:
		{
		TBool on = ETrue;
		kumemput32(a1, &on, sizeof(TBool));
		return KErrNone;
		}
	case EMouseHalSetMouseState:
		return KErrNone;
	default:
		return KErrNotSupported;
		}
	}

void X64StartKeyboard()
	{
	TheI8042 = new DPcI8042;
	TInt r = TheI8042 ? TheI8042->Create() : KErrNoMemory;
	Kern::Printf("[i8042] PS/2 keyboard (irq %d -> GSI %d) %s -> %d", EX64IrqKeyboard, X64IsaToGsi(EX64IrqKeyboard),
					(TheI8042 && TheI8042->Mouse()) ? "+ mouse (irq 12)" : "(no mouse)", r);
	extern void X64TimeSyncStart();			// xk_timesync.cpp (X4.42): after the VMMDev is set up
	X64TimeSyncStart();
	}
