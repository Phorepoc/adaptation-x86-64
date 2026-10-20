// xk_absmouse.h -- the absolute pointer devices of the hypervisors (xk_absmouse.cpp, X4.35)
#ifndef XK_ABSMOUSE_H
#define XK_ABSMOUSE_H
#include <e32def.h>
class TDfc;

enum TAbsMouseKind { EAbsNone, EAbsVmmDev, EAbsVmMouse };

struct SAbsPacket
	{
	TBool iAbsolute;		// the host is giving absolute positions (else: PS/2 motion is the pointer)
	TInt iX, iY;			// 0..0xFFFF across the whole framebuffer
	TInt iButtons;			// PS/2 bit layout (1 left, 2 right, 4 middle); -1 = they come in the PS/2 stream
	};

TInt X64AbsMouseInit();								// which device (TAbsMouseKind); thread context
TInt X64AbsMouseRead(SAbsPacket* aOut, TInt aMax);	// packets now available (VMMDev: the current state)
TInt X64AbsMouseBindIrq(TDfc* aDfc);				// VMMDev: interrupts queue aDfc (X4.43)
TInt X64AbsMouseIrqService();						// in that DFC, first: ack + settle; the pin or -1
TInt X64VmmHostTimeMs(TInt64& aMs);					// VirtualBox host clock, ms since 1970 UTC (X4.42)

#endif
