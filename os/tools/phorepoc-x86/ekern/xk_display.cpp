// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// xk_display.cpp -- the Phorepoc amd64 board's display HAL (EHalGroupDisplay). X4.18.
// Ours: the 2010 drop has no x86 baseport, so this is the board side of a contract whose user
// side IS in the source, unmodified -- drivers/edisp/epoc/generic/wd_generic.cpp:
//
//   CScreenDriver::New()  `#ifdef __X86__`: HAL::Get(EDisplayMode) == 0 -> NewScreenDriverVgaText()
//                          (epoc/vga/vgatext.cpp: UserSvr::ScreenInfo(), 80x25 cells written at
//                          iScreenAddress+0x18000, i.e. physical 0xB8000 through the 0xA0000
//                          window); any other mode -> CScreenDriverTemplate, a framebuffer driver
//                          that renders the console into EDisplayMemoryAddress itself.
//
// Two modes, then:
//   0  VGA text        the 128K VGA window at physical 0xA0000, mapped user-RW
//   1  linear 32 bpp   the framebuffer GRUB set up for boot.S's multiboot2 request (xboot.cpp
//                      records it in the super page, xk_board.h), mapped user-RW
// The current mode is 1 whenever GRUB gave us a framebuffer (then the VGA is in a graphics mode
// and text-mode writes would be invisible), else 0. Mapping the panel user-RW is what the Syborg
// board does too (svpframebuffer.cpp: DPlatChunkHw::New(.., EMapAttrUserRw|..)).
#include <kernel/kern_priv.h>
#include <videodriver.h>
#include "xk_board.h"

static const TPhysAddr KVgaWindowPhys = 0xA0000;
static const TInt KVgaWindowSize = 0x20000;

static DPlatChunkHw* TheVgaChunk;
static DPlatChunkHw* TheFbChunk;
struct SFb { TUint32 iPhys, iPitch, iWidth, iHeight, iBpp, iRgb; };
static SFb TheFb;
static TInt TheMode;
static TInt TheDisplayState = 1;			// on

static TLinAddr ScreenAddress();			// X4.23: the shadow screen, when there is one
static TInt ScreenPitch();

// X4.26: the SCREEN the HAL reports may be a centred sub-rectangle of the framebuffer -- S60's
// layout packs exist for phone resolutions (nHD 360x640), not for 1024x768. make_ekern.sh
// SCREEN=WxH -> XK_SCREEN_W/H; 0 = the whole framebuffer. Same pitch, so only the base moves.
#ifndef XK_SCREEN_W
#define XK_SCREEN_W 0
#define XK_SCREEN_H 0
#endif
// X4.35: pixel doubling. A phone screen (360x640) is a postage stamp on a modern monitor, and S60's
// layouts are fixed-resolution, so the software keeps its nHD screen and the DISPLAY scales it: each
// screen pixel is an NxN block of the framebuffer -- a panel with bigger pixels, the way a 2x "retina"
// panel runs legacy content. The scaling lives in the shadow-screen flusher (X4.23), which already
// copies every change to the framebuffer, so it costs nothing where nothing changes. make_ekern.sh
// SCALE=n -> XK_SCALE (and asks GRUB for a framebuffer that big); 0 = the largest integer that fits.
#ifndef XK_SCALE
#define XK_SCALE 0
#endif
TInt X64ScreenW, X64ScreenH;				// what the HAL reports (the i8042 mouse area follows it)
// The shadow screen's words per line: rows padded to 64 bytes, as the real S^3 hardware lays them out (the 701's HAL
// reports 1472 bytes for its 360-px rows); games that write ScreenInfo memory assume that stride (SkyForce: 368 px)
static inline TInt ScreenWpl() { return (X64ScreenW + 15) & ~15; }
TInt X64Scale = 1;							// framebuffer pixels per screen pixel, each way
TInt X64ScreenOx, X64ScreenOy;				// framebuffer pixel of the screen's (0,0)
TInt X64FbW, X64FbH;						// the framebuffer, in its own pixels
static TInt TheScreenOffset;				// bytes from the framebuffer base to the screen's (0,0)

static TInt ModeInfo(TInt aMode, TVideoInfoV01& a)
	{
	memclr(&a, sizeof(a));
	a.iIsPixelOrderRGB = ETrue;
	a.iIsPixelOrderLandscape = ETrue;
	a.iDisplayMode = aMode;
	if (aMode == 0)
		{
		a.iSizeInPixels.iWidth = 640;			// 80x25 cells of 8x16
		a.iSizeInPixels.iHeight = 400;
		a.iIsPalettized = ETrue;
		a.iBitsPerPixel = 4;
		a.iVideoAddress = TheVgaChunk ? TheVgaChunk->LinearAddress() + 0x18000 : 0;
		a.iOffsetBetweenLines = 160;
		}
	else if (aMode == 1 && TheFbChunk)
		{
		a.iSizeInPixels.iWidth = X64ScreenW;
		a.iSizeInPixels.iHeight = X64ScreenH;
		a.iIsPalettized = EFalse;
		a.iBitsPerPixel = TheFb.iBpp;
		a.iVideoAddress = ScreenAddress();
		a.iOffsetBetweenLines = ScreenPitch();
		}
	else
		return KErrArgument;
	// the physical size: a 3.5" 640x360 phone panel's density = 210 dpi = 1440/210 = 48/7 twips per pixel
	// (43 x 77 mm). Was 15 (96 dpi, a desktop monitor): Qt converts point sizes with it (S4.4c: Orbit's default
	// 7 pt font came out 9 px instead of ~20)
	a.iSizeInTwips.iWidth = a.iSizeInPixels.iWidth * 48 / 7;
	a.iSizeInTwips.iHeight = a.iSizeInPixels.iHeight * 48 / 7;
	return KErrNone;
	}

static TInt DisplayHal(TAny*, TInt aFunction, TAny* a1, TAny* a2)
	{
	switch (aFunction)
		{
	case EDisplayHalScreenInfo:
		{
		TPckgBuf<TScreenInfoV01> v;
		TVideoInfoV01 vi;
		ModeInfo(TheMode, vi);
		v().iWindowHandleValid = EFalse;
		v().iWindowHandle = NULL;
		// vgatext.cpp adds 0x18000 to this: in mode 0 it is the base of the 0xA0000 window
		DPlatChunkHw* c = TheMode ? TheFbChunk : TheVgaChunk;
		v().iScreenAddressValid = c != NULL;
		v().iScreenAddress = !c ? NULL : TheMode ? (TAny*)ScreenAddress() : (TAny*)c->LinearAddress();
		v().iScreenSize = vi.iSizeInPixels;
		Kern::InfoCopy(*(TDes8*)a1, v);
		return KErrNone;
		}
	case EDisplayHalMode:
		kumemput32(a1, &TheMode, sizeof(TheMode));
		return KErrNone;
	case EDisplayHalModeCount:
		{
		TInt n = TheFbChunk ? 2 : 1;
		kumemput32(a1, &n, sizeof(n));
		return KErrNone;
		}
	case EDisplayHalSetMode:
		{
		TInt m = (TInt)a1;
		if (m != 0 && !(m == 1 && TheFbChunk))
			return KErrArgument;
		TheMode = m;
		return KErrNone;
		}
	case EDisplayHalColors:
		{
		TInt n = TheMode ? (1 << 24) : 16;
		kumemput32(a1, &n, sizeof(n));
		return KErrNone;
		}
	case EDisplayHalState:
		kumemput32(a1, &TheDisplayState, sizeof(TheDisplayState));
		return KErrNone;
	case EDisplayHalSetState:
		TheDisplayState = a1 ? 1 : 0;
		return KErrNone;
	case EDisplayHalCurrentModeInfo:
	case EDisplayHalSpecifiedModeInfo:
		{
		TInt mode = TheMode;
		if (aFunction == EDisplayHalSpecifiedModeInfo)
			{
			kumemget32(&mode, a1, sizeof(mode));
			a1 = a2;
			}
		TPckgBuf<TVideoInfoV01> v;
		TInt r = ModeInfo(mode, v());
		if (r == KErrNone)
			Kern::InfoCopy(*(TDes8*)a1, v);
		return r;
		}
	case EDisplayHalWsRegisterSwitchOnScreenHandling:
	case EDisplayHalWsSwitchOnScreen:
		return KErrNone;
	default:
		return KErrNotSupported;
		}
	}

// ---- X4.23: measure the framebuffer, and shadow it when it is slow ---------------------------------
//
// Boot-time bandwidth probe, timed with the TSC (calibrated against the PIT with the local-APIC timer,
// xk_irq.cpp) -- NOT the system tick, which VirtualBox-on-Hyper-V ("NEM") stops delivering while a
// vCPU runs a long loop (measured: 0 ticks over ~25 s). A fixed amount of work per test, so bounded.
//   QEMU (TCG):              framebuffer = RAM speed, ~3.6 GB/s write, ~7 GB/s read
//   VirtualBox 7.2 on NEM:   framebuffer ~100x slower than RAM (guest VRAM accesses are emulated)
extern TUint32 X64_TscPerMs;				// xk_irq.cpp
static inline TUint64 Rdtsc()
	{ TUint32 lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)); return (TUint64)hi<<32 | lo; }

// MB/s for writes, reads and an in-buffer copy (what a console scroll is) over aBytes, aPasses times
static void Bandwidth(const char* aWhat, volatile TUint32* aBuf, TInt aBytes, TInt aPasses, TUint32* aMBs)
	{
	const TInt words = aBytes / 8;					// two halves: copy src/dst
	TUint32 sum = 0;
	for (TInt test=0; test<3; ++test)
		{
		const TUint64 t0 = Rdtsc();
		for (TInt pass=0; pass<aPasses; ++pass)
			{
			if (test == 0)
				for (TInt i=0; i<words*2; ++i) aBuf[i] = i;
			else if (test == 1)
				for (TInt i=0; i<words*2; ++i) sum += aBuf[i];
			else
				for (TInt i=0; i<words; ++i) aBuf[words+i] = aBuf[i];
			}
		const TUint64 cycles = Rdtsc() - t0;
		const TUint64 kb = (TUint64)aPasses * ((test == 2 ? aBytes/2 : aBytes) >> 10);
		const TUint64 us = X64_TscPerMs ? cycles * 1000 / X64_TscPerMs : 0;
		aMBs[test] = us ? (TUint32)(kb * 1000 / us) : 0;			// KB/ms = MB/s
		}
	Kern::Printf("[display] bandwidth %s: write %d MB/s, read %d MB/s, copy %d MB/s", aWhat, aMBs[0], aMBs[1], aMBs[2]);
	(void)sum;
	}


// The shadow: WSERV/scdv draw into cached RAM that the HAL reports as the screen (TheScreen: exactly the
// screen, X64ScreenW x X64ScreenH, pitch W*4), and a flusher -- an LCD controller's DMA, in software --
// copies what changed to the framebuffer every KFlushMs, each screen pixel as an X64Scale-square block at
// (X64ScreenOx, X64ScreenOy). What the framebuffer holds is mirrored in TheMirror (screen-sized), so
// finding a change never READS the framebuffer (a console scroll is a read of the whole screen: the worst
// case there is). Per row, the first..last differing 32-bit word span is copied. The cost is two RAM
// scans of the screen per flush.
static const TInt KFlushMs = 20;
static DPlatChunkHw* TheScreenChunk;		// user-RW, cached: what the HAL hands out in shadow mode
static TUint32* TheScreen;
static TUint32* TheMirror;					// supervisor copy of what the framebuffer shows of the screen
static TDfcQue* TheFlushQ;
static TUint32 TheFlushes, TheFlushedRows;

static void FlushTimerIsr(TAny*);
static void FlushDfcFn(TAny*);
static NTimer TheFlushTimer(FlushTimerIsr, NULL);
static TDfc TheFlushDfc(FlushDfcFn, NULL, 1);
static void ModeDfcFn(TAny*);								// X4.45: a host resize, on the same queue
static TDfc TheModeDfc(ModeDfcFn, NULL, 2);

static void FlushTimerIsr(TAny*)
	{
	TheFlushDfc.Add();
	}

// ---- X4.33: a software mouse cursor, composited by the flusher (shadow mode only) --------------------
// Symbian^3 is a touch UI and draws no pointer; with a mouse (VirtualBox by hand) you would click blind.
// A hardware cursor in software: the arrow is drawn into the FRAMEBUFFER only -- the shadow (what WSERV
// drew) and the mirror (what the framebuffer holds of it) never see it -- so the diff logic is unchanged.
// Each flush: if the pointer moved, the old rectangle is restored from the shadow; if it moved or any
// row under it was rewritten, the arrow is drawn again. Shown from the first mouse movement on (a touch
// device's pen never shows it). Positions are FRAMEBUFFER pixels (X4.35: the arrow is not scaled -- it
// is a host-sized pointer over a magnified screen); -1 hides it again (X4.35: when the mouse is
// absolute, the host draws its own pointer -- xk_absmouse.cpp).
static const TInt KCurW = 12, KCurH = 19;
static const char* const KCursor[KCurH] = {		// X = outline (black), o = fill (white), . = transparent
	"X...........", "XX..........", "XoX.........", "XooX........", "XoooX.......", "XooooX......",
	"XoooooX.....", "XooooooX....", "XoooooooX...", "XooooooooX..", "XoooooooooX.", "XooooooXXXXX",
	"XoooXooX....", "XooXXooX....", "XoX..XooX...", "XX...XooX...", "X.....XooX..", ".......XoX..",
	"........XX.." };
// the same arrow as VirtualBox's host pointer shape (xk_absmouse.cpp, X4.81)
const char* const* X64CursorBitmap(TInt& aW, TInt& aH)
	{
	aW = KCurW;
	aH = KCurH;
	return KCursor;
	}

static volatile TInt TheCursorX = -1, TheCursorY = -1;	// framebuffer coordinates; -1 = hidden
static TInt TheDrawnX = -1, TheDrawnY = -1;				// where the flusher last drew it

void X64SetCursorPos(TInt aX, TInt aY)					// xk_keyboard.cpp, on every pointer move
	{
	TheCursorY = aY;
	TheCursorX = aX;
	}

// what the framebuffer shows at (aX,aY) without the cursor: the screen's pixel, or the black border
static inline TUint32 ScreenPixelAt(TInt aX, TInt aY)
	{
	const TInt x = aX - X64ScreenOx, y = aY - X64ScreenOy;
	if (x < 0 || y < 0)
		return 0;
	const TInt sx = x / X64Scale, sy = y / X64Scale;
	return (sx < X64ScreenW && sy < X64ScreenH) ? TheScreen[sy*ScreenWpl() + sx] : 0;
	}

// the cursor rectangle at framebuffer (aX,aY), clipped: the arrow, or (aDraw false) what is under it
static void CursorRect(volatile TUint32* aFb, TInt aWpl, TInt aX, TInt aY, TBool aDraw)
	{
	for (TInt r=0; r<KCurH; ++r)
		{
		const TInt y = aY + r;
		if (y < 0 || y >= (TInt)TheFb.iHeight)
			continue;
		for (TInt c=0; c<KCurW; ++c)
			{
			const TInt x = aX + c;
			if (x < 0 || x >= (TInt)TheFb.iWidth)
				continue;
			const char p = aDraw ? KCursor[r][c] : '.';
			aFb[y*aWpl + x] = p == 'X' ? 0xFF000000u : p == 'o' ? 0xFFFFFFFFu : ScreenPixelAt(x, y);
			}
		}
	}

static void FlushDfcFn(TAny*)
	{
	volatile TUint32* fb = (volatile TUint32*)TheFbChunk->LinearAddress();
	const TInt wpl = TheFb.iPitch / 4;							// framebuffer words per line
	const TInt w = X64ScreenW, s = X64Scale;
	volatile TUint32* origin = fb + X64ScreenOy*wpl + X64ScreenOx;
	TBool underCursor = EFalse;
	for (TInt y=0; y<X64ScreenH; ++y)
		{
		const TUint32* src = TheScreen + y*ScreenWpl();
		TUint32* m = TheMirror + y*w;
		TInt first = 0, last = w - 1;
		while (first <= last && src[first] == m[first])
			++first;
		if (first > last)
			continue;
		while (src[last] == m[last])
			--last;
		volatile TUint32* row = origin + y*s*wpl;
		if (s == 1)
			for (TInt x=first; x<=last; ++x)
				row[x] = m[x] = src[x];
		else
			for (TInt k=0; k<s; ++k)								// s framebuffer lines per screen line
				{
				volatile TUint32* line = row + k*wpl;
				for (TInt x=first; x<=last; ++x)
					{
					const TUint32 p = src[x];
					for (TInt j=0; j<s; ++j)
						line[x*s + j] = p;
					}
				}
		if (s != 1)
			for (TInt x=first; x<=last; ++x)
				m[x] = src[x];
		++TheFlushedRows;
		const TInt fy = X64ScreenOy + y*s;							// framebuffer rows fy .. fy+s-1
		if (fy + s > TheDrawnY && fy < TheDrawnY + KCurH)
			underCursor = ETrue;
		}
	const TInt nx = TheCursorX, ny = TheCursorY;
	const TBool moved = nx != TheDrawnX || ny != TheDrawnY;
	if (moved && TheDrawnX >= 0)
		CursorRect(fb, wpl, TheDrawnX, TheDrawnY, EFalse);		// put back what WSERV drew there
	if (nx >= 0 && (moved || underCursor))
		CursorRect(fb, wpl, nx, ny, ETrue);
	TheDrawnX = nx;  TheDrawnY = ny;
	++TheFlushes;
	TheFlushTimer.OneShot(NKern::TimerTicks(KFlushMs));
	}

static TInt StartShadow()
	{
	const TInt size = (ScreenWpl() * X64ScreenH * 4 + 0xFFF) & ~0xFFF;
	TPhysAddr phys;
	TInt r = Epoc::AllocPhysicalRam(size, phys);
	if (r == KErrNone)
		r = DPlatChunkHw::New(TheScreenChunk, phys, size, EMapAttrUserRw | EMapAttrCachedMax);
	if (r != KErrNone)
		return r;
	TheScreen = (TUint32*)TheScreenChunk->LinearAddress();
	TheMirror = (TUint32*)Kern::Alloc(size);
	if (!TheMirror)
		return KErrNoMemory;
	memclr(TheScreen, size);
	memclr(TheMirror, size);
	memclr((TAny*)TheFbChunk->LinearAddress(), TheFb.iPitch * TheFb.iHeight);	// the mirror's premise: fb == 0
	_LIT(KFlushQName, "DisplayFlush");
	r = Kern::DfcQCreate(TheFlushQ, 26, &KFlushQName);
	if (r != KErrNone)
		return r;
	TheFlushDfc.SetDfcQ(TheFlushQ);
	TheModeDfc.SetDfcQ(TheFlushQ);
	TheFlushTimer.OneShot(NKern::TimerTicks(KFlushMs));
	return KErrNone;
	}

// the screen memory the HAL reports: the shadow when there is one, else the framebuffer itself
static TLinAddr ScreenAddress()
	{
	return TheScreenChunk ? TheScreenChunk->LinearAddress() : TheFbChunk->LinearAddress() + TheScreenOffset;
	}
static TInt ScreenPitch()
	{
	return TheScreenChunk ? ScreenWpl() * 4 : TheFb.iPitch;
	}

// ---- X4.35: mode setting, Bochs VBE "DISPI" -------------------------------------------------------------
// GRUB can only pick from the VBE BIOS's mode list (QEMU: asked 720x1280, got 1280x800), and a scaled
// phone screen wants a framebuffer of exactly SCREEN x SCALE. QEMU's std-vga and VirtualBox's VBoxVGA both
// descend from the Bochs VBE adapter, whose DISPI registers (index 0x1CE / data 0x1CF) take ANY size --
// the interface Linux's bochs-drm drives. Same linear framebuffer, same physical address; the host
// window follows the mode. On anything else port 0x1CF reads back no DISPI id and nothing changes.
static inline void Outw(TUint16 aPort, TUint16 aVal)
	{ __asm__ __volatile__("out dx, ax" : : "d"(aPort), "a"(aVal)); }
static inline TUint16 Inw(TUint16 aPort)
	{ TUint16 v; __asm__ __volatile__("in ax, dx" : "=a"(v) : "d"(aPort)); return v; }
enum { EDispiId = 0, EDispiXres = 1, EDispiYres = 2, EDispiBpp = 3, EDispiEnable = 4, EDispiVirtW = 6,
	   EDispiVirtH = 7, EDispiXoff = 8, EDispiYoff = 9 };
static const TUint16 KDispiEnabled = 0x01, KDispiLfb = 0x40;
static TUint16 Dispi(TInt aIndex)				{ Outw(0x1CE, (TUint16)aIndex); return Inw(0x1CF); }
static void Dispi(TInt aIndex, TInt aVal)		{ Outw(0x1CE, (TUint16)aIndex); Outw(0x1CF, (TUint16)aVal); }

static void SetMode(TInt aW, TInt aH)
	{
	const TUint16 id = Dispi(EDispiId);
	if (id < 0xB0C0 || id > 0xB0CF || Dispi(EDispiXres) != TheFb.iWidth || Dispi(EDispiYres) != TheFb.iHeight)
		{
		Kern::Printf("[display] no Bochs DISPI mode setting (id %04x): keeping %dx%d", id, TheFb.iWidth, TheFb.iHeight);
		return;
		}
	Dispi(EDispiEnable, 0);
	Dispi(EDispiXres, aW);
	Dispi(EDispiYres, aH);
	Dispi(EDispiBpp, 32);
	Dispi(EDispiVirtW, aW);
	Dispi(EDispiVirtH, aH);
	Dispi(EDispiXoff, 0);
	Dispi(EDispiYoff, 0);
	Dispi(EDispiEnable, KDispiEnabled | KDispiLfb);
	const TInt w = Dispi(EDispiXres), h = Dispi(EDispiYres);
	Kern::Printf("[display] DISPI %04x: mode %dx%d -> %dx%d", id, TheFb.iWidth, TheFb.iHeight, w, h);
	if (w != aW || h != aH || Dispi(EDispiBpp) != 32)
		{
		Dispi(EDispiEnable, 0);						// refused (too big for the VRAM?): back to GRUB's
		Dispi(EDispiXres, TheFb.iWidth);
		Dispi(EDispiYres, TheFb.iHeight);
		Dispi(EDispiBpp, 32);
		Dispi(EDispiVirtW, TheFb.iWidth);
		Dispi(EDispiVirtH, TheFb.iHeight);
		Dispi(EDispiEnable, KDispiEnabled | KDispiLfb);
		return;
		}
	TheFb.iWidth = w;
	TheFb.iHeight = h;
	TheFb.iPitch = w * 4;
	}

static void PlaceScreen()
	{
	X64ScreenOx = (TheFb.iWidth - X64ScreenW * X64Scale) / 2;
	X64ScreenOy = (TheFb.iHeight - X64ScreenH * X64Scale) / 2;
	TheScreenOffset = X64ScreenOy * TheFb.iPitch + X64ScreenOx * 4;
	}

// ---- X4.45: the host resizes the display (VirtualBox "auto-resize guest display") ----------------------
// VMMDev delivers the host window's size (xk_absmouse.cpp: display-change request); the framebuffer
// takes that mode through DISPI, the phone screen keeps its size and gets the largest integer scale
// that fits, centred. It runs on the flusher's own DFC queue, so no flush is ever half-way through a
// mode. Only with the shadow screen (the flusher owns the framebuffer; WSERV's screen address and
// pitch never change), which every scaled screen has.
static const TInt KFbMapMax = 0x1000000;		// 16 MB mapped: 2048x2048 at 32 bpp
static TInt TheReqW, TheReqH;

void X64DisplayRequestMode(TInt aW, TInt aH)
	{
	if (!TheScreenChunk || aW <= 0 || aH <= 0)
		return;
	TheReqW = aW;  TheReqH = aH;
	TheModeDfc.Enque();
	}

static void ModeDfcFn(TAny*)
	{
	const TInt w = Max(TheReqW, X64ScreenW), h = Max(TheReqH, X64ScreenH);	// never smaller than the phone
	if (w * h * 4 > KFbMapMax || (w == (TInt)TheFb.iWidth && h == (TInt)TheFb.iHeight))
		return;
	SetMode(w, h);									// TheFb follows (or keeps the old mode if refused)
	X64FbW = TheFb.iWidth;  X64FbH = TheFb.iHeight;
	X64Scale = Max(1, Min(X64FbW / X64ScreenW, X64FbH / X64ScreenH));
	PlaceScreen();
	memclr((TAny*)TheFbChunk->LinearAddress(), TheFb.iPitch * TheFb.iHeight);
	const TInt n = X64ScreenW * X64ScreenH;			// the mirror no longer matches: redraw everything
	for (TInt i=0; i<n; ++i)
		TheMirror[i] = ~TheScreen[(i / X64ScreenW) * ScreenWpl() + i % X64ScreenW];
	TheDrawnX = TheDrawnY = -1;						// the cursor's saved background is gone too
	Kern::Printf("[display] host resize: %dx%d asked -> framebuffer %dx%d, screen x%d at (%d,%d)",
					TheReqW, TheReqH, X64FbW, X64FbH, X64Scale, X64ScreenOx, X64ScreenOy);
	}

void X64StartDisplay()
	{
	const TInt* w = TheSuperPage().iReserved;
	TheFb.iPhys = w[KX64SuperFbPhys];
	TheFb.iPitch = w[KX64SuperFbPitch];
	TheFb.iWidth = w[KX64SuperFbWidth];
	TheFb.iHeight = w[KX64SuperFbHeight];
	TheFb.iBpp = w[KX64SuperFbFormat] & 0xFF;
	TheFb.iRgb = (w[KX64SuperFbFormat] >> 8) & 1;
	if (TheFb.iPhys && TheFb.iBpp == 32 && XK_SCALE > 1 && XK_SCREEN_W > 0 &&
		((TInt)TheFb.iWidth != XK_SCREEN_W * XK_SCALE || (TInt)TheFb.iHeight != XK_SCREEN_H * XK_SCALE))
		SetMode(XK_SCREEN_W * XK_SCALE, XK_SCREEN_H * XK_SCALE);
	X64FbW = X64ScreenW = TheFb.iWidth;
	X64FbH = X64ScreenH = TheFb.iHeight;
	if (XK_SCREEN_W > 0 && XK_SCREEN_W <= (TInt)TheFb.iWidth && XK_SCREEN_H > 0 && XK_SCREEN_H <= (TInt)TheFb.iHeight)
		{
		X64ScreenW = XK_SCREEN_W;
		X64ScreenH = XK_SCREEN_H;
		// X4.35: the scale -- as asked, or the largest integer that fits (GRUB may not have found the
		// mode boot.S asked for; the screen then just comes out smaller)
		const TInt fit = Min(TheFb.iWidth / XK_SCREEN_W, TheFb.iHeight / XK_SCREEN_H);
		X64Scale = (XK_SCALE > 0 && XK_SCALE < fit) ? XK_SCALE : fit;
		}
	PlaceScreen();
	NKern::ThreadEnterCS();
	TInt r = DPlatChunkHw::New(TheVgaChunk, KVgaWindowPhys, KVgaWindowSize,
							   EMapAttrUserRw | EMapAttrFullyBlocking);
	if (r != KErrNone)
		TheVgaChunk = NULL;
	Kern::Printf("[display] VGA window %08x+%x -> lin %08x (%d)", KVgaWindowPhys, KVgaWindowSize,
				 TheVgaChunk ? TheVgaChunk->LinearAddress() : 0, r);
	if (TheFb.iPhys && TheFb.iRgb && TheFb.iBpp == 32)
		{
		// X4.45: map room for the largest mode a host resize may ask for (DISPI refuses modes
		// bigger than the VRAM, so nothing is written past what the adapter really has)
		TInt size = Max(TheFb.iPitch * TheFb.iHeight, KFbMapMax);
		r = DPlatChunkHw::New(TheFbChunk, TheFb.iPhys, size, EMapAttrUserRw | EMapAttrBufferedNC);
		if (r != KErrNone)
			TheFbChunk = NULL;
		Kern::Printf("[display] framebuffer %dx%dx%d pitch %d @ %08x -> lin %08x (%d)",
					 TheFb.iWidth, TheFb.iHeight, TheFb.iBpp, TheFb.iPitch, TheFb.iPhys,
					 TheFbChunk ? TheFbChunk->LinearAddress() : 0, r);
		}
	else
		Kern::Printf("[display] no usable GRUB framebuffer (phys %08x bpp %d) -- VGA text",
					 TheFb.iPhys, TheFb.iBpp);
	if (TheFbChunk)
		{
		// measure (a few MB of RAM traffic, 1 MB on the framebuffer), then decide: shadow the framebuffer
		// when writing it is more than 4x slower than writing RAM, or reading it more than 4x slower
		TUint32 ram[3] = {0,0,0}, fb[3];
		TAny* buf = Kern::Alloc(256*1024);
		if (buf)
			Bandwidth("RAM        ", (volatile TUint32*)buf, 256*1024, 16, ram);
		Kern::Free(buf);
		Bandwidth("framebuffer", (volatile TUint32*)TheFbChunk->LinearAddress(), 256*1024, 4, fb);
		memclr((TAny*)TheFbChunk->LinearAddress(), 256*1024);
		// make_ekern.sh FBSHADOW=0|1 overrides the measurement (A/B testing); default -1 = decide.
		// A scaled screen always has the shadow: the flusher is what scales (X4.35).
		if (X64Scale > 1 || XK_FB_SHADOW == 1 || (XK_FB_SHADOW < 0 && (fb[0]*4 < ram[0] || fb[1]*4 < ram[1])))
			{
			r = StartShadow();
			Kern::Printf("[display] shadow screen at %08x (%s), flushed x%d every %d ms -> %d",
							TheScreenChunk ? TheScreenChunk->LinearAddress() : 0,
							X64Scale > 1 ? "scaled" : "slow framebuffer", X64Scale, KFlushMs, r);
			if (r != KErrNone)
				{
				TheScreenChunk = NULL;
				X64Scale = 1;								// no flusher: draw 1:1 into the framebuffer
				PlaceScreen();
				}
			}
		else
			Kern::Printf("[display] framebuffer is fast: drawn directly");
		}
	NKern::ThreadLeaveCS();
	TheMode = TheFbChunk ? 1 : 0;
	r = Kern::AddHalEntry(EHalGroupDisplay, DisplayHal, NULL);
	Kern::Printf("[display] mode %d, screen %dx%d x%d at (%d,%d) of %dx%d, AddHalEntry(EHalGroupDisplay) -> %d",
					TheMode, X64ScreenW, X64ScreenH, X64Scale, X64ScreenOx, X64ScreenOy, X64FbW, X64FbH, r);
	}
