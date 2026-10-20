// xk_power.cpp -- the Phorepoc amd64 board's power controller + power handler. X4.74.
//
// The generic kernel power manager (kernel/eka/kernel/power.cpp) starts with a DummyPowerController
// ("the platform doesn't support power management") whose PowerDown(EPwOff) panics, and it asserts
// a non-empty handler list before any transition: the real SSM's shutdown and restart paths
// (Power::EnableWakeupEvents(EPwOff|EPwRestart) + Power::PowerDown()) then die with
// "Assertion 'iHandlers' failed ... FAULT: Power Management". A phone baseport registers its own
// controller and every driver that owns hardware is a DPowerHandler (the template BSP's
// template_variant/specific/power.cpp, its lcd/keyboard drivers). Here:
//   * DX64PowerController -- the template's shape: standby = nothing to sleep on in a VM, so
//     wakeup events fire at once and PowerDown(EPwStandby) returns; EPwOff = ACPI soft-off (S5:
//     SLP_TYP | SLP_EN into the FADT's PM1a/PM1b control ports the bootstrap found, xboot.cpp);
//     EPwRestart = the PC reset (reset-control register 0xCF9, then the i8042 pulse). Neither returns.
//   * DX64PowerHandler -- the board's one handler: our drivers keep no state that a power-down
//     would lose (the transition only ends in off or reset), so it acknowledges at once.
// CpuIdle is unused on x86 (the null thread halts in NKern::DoIdle).
#include <kernel/kern_priv.h>
#include <kernel/kpower.h>
#include <e32keys.h>
#include <assp.h>
#include "xk_board.h"
#include "xk_interrupt.h"

static inline void Outb(TUint16 aPort, TUint8 aVal)
	{ __asm__ __volatile__("out dx, al" : : "d"(aPort), "a"(aVal)); }
static inline void Outw(TUint16 aPort, TUint16 aVal)
	{ __asm__ __volatile__("out dx, ax" : : "d"(aPort), "a"(aVal)); }

static const TUint16 KAcpiSlpEn = 0x2000;			// PM1x_CNT bit 13; SLP_TYP = bits 10-12

static void __attribute__((noreturn)) Halt()
	{
	for (;;)
		__asm__ __volatile__("cli; hlt");
	}

class DX64PowerController : public DPowerController
	{
public:
	void CpuIdle() {}
	void EnableWakeupEvents() { WakeupEvent(); }	// nothing to sleep on: "one occurred"
	void DisableWakeupEvents() {}
	void AbsoluteTimerExpired() {}
	void PowerDown(TTimeK aWakeupTime);
	};

void DX64PowerController::PowerDown(TTimeK /*aWakeupTime*/)
	{
	if (iTargetState == EPwStandby)
		return;										// the wakeup event already happened
	const TBool off = (iTargetState == EPwOff);
	Kern::Printf("[power] %s", off ? "switch off (ACPI S5)" : "restart");
	if (off)
		{
		const TUint32 a = (TUint32)Kern::SuperPage().iReserved[KX64SuperPm1a];
		const TUint32 b = (TUint32)Kern::SuperPage().iReserved[KX64SuperPm1b];
		if (a & KX64AcpiValid)
			{
			NKern::DisableAllInterrupts();
			if (b & KX64AcpiValid)
				Outw((TUint16)b, (TUint16)(((b >> 16) & 7) << 10 | KAcpiSlpEn));
			Outw((TUint16)a, (TUint16)(((a >> 16) & 7) << 10 | KAcpiSlpEn));
			}
		Kern::Printf("[power] no ACPI soft-off -- halted");
		Halt();
		}
	NKern::DisableAllInterrupts();
	Outb(0xCF9, 0x02);								// reset control: select hard reset...
	Outb(0xCF9, 0x06);								// ... and do it
	Outb(0x64, 0xFE);								// the i8042's CPU reset line
	Halt();
	}

_LIT(KX64PowerHandlerName, "X64Board");

class DX64PowerHandler : public DPowerHandler
	{
public:
	DX64PowerHandler() : DPowerHandler(KX64PowerHandlerName) {}
	void PowerDown(TPowerState) { PowerDownDone(); }
	void PowerUp() { PowerUpDone(); }
	};

// ---- the power BUTTON (X4.75). A PC's power button raises an ACPI fixed event (PM1 status PWRBTN_STS, bit 8)
// on the SCI; it is how QEMU's `system_powerdown` and VirtualBox's "ACPI Shutdown" ask the OS to switch off.
// The phone's equivalent is the power key (EStdKeyDevice2) held down: SysAp's long press
// (HandleLongPowerKeyPressedL) = switch off through SSM. So: the chipset goes to ACPI mode (SMI_CMD <-
// ACPI_ENABLE, unless SCI_EN is already set), PWRBTN_EN on; the SCI (level, shared, as ACPI defines it) clears the
// status and a DFC presses the power key for KPowerKeyHoldMs -- WSERV's key repeats make it SysAp's long press.
static const TUint16 KPm1PwrBtn = 0x0100;				// PM1_STS / PM1_EN bit 8
static const TInt KPowerKeyHoldMs = 3000;

class TX64PowerButton
	{
public:
	TInt Start();
private:
	static void Isr(TAny* aPtr);
	static void PressDfc(TAny* aPtr);
	static void ReleaseIsr(TAny* aPtr);
	static void ReleaseDfc(TAny* aPtr);
	static inline TUint16 Inw(TUint16 aPort)
		{ TUint16 v; __asm__ __volatile__("in ax, dx" : "=a"(v) : "d"(aPort)); return v; }
	TUint16 iSts, iEn;									// PM1a status / enable ports
	TInt iHandle;
	TBool iHeld;
	TDfc iDfc;
	TDfc iReleaseDfc;
	NTimer iRelease;
public:
	TX64PowerButton() : iHeld(EFalse), iDfc(PressDfc, this, 1), iReleaseDfc(ReleaseDfc, this, 1), iRelease(ReleaseIsr, this) {}
	};

TInt TX64PowerButton::Start()
	{
	const TUint32 evt = (TUint32)Kern::SuperPage().iReserved[KX64SuperPm1Evt];
	const TUint32 sci = (TUint32)Kern::SuperPage().iReserved[KX64SuperSci];
	const TUint32 smi = (TUint32)Kern::SuperPage().iReserved[KX64SuperSmiCmd];
	const TUint32 cnt = (TUint32)Kern::SuperPage().iReserved[KX64SuperPm1a];
	if (!(evt & 0xFFFF) || ((evt >> 16) & 0xFF) < 4 || !(sci & 0xFFFF) || (sci & 0xFFFF) >= 16)
		return KErrNotSupported;
	iSts = (TUint16)evt;
	iEn = (TUint16)(iSts + ((evt >> 16) & 0xFF) / 2);
	// legacy -> ACPI mode (SCI_EN = PM1_CNT bit 0): what an ACPI OS does first
	if ((cnt & KX64AcpiValid) && !(Inw((TUint16)cnt) & 1) && smi && ((sci >> 16) & 0xFF))
		{
		Outb((TUint16)smi, (TUint8)(sci >> 16));
		for (TInt i=0; i<300 && !(Inw((TUint16)cnt) & 1); ++i)
			NKern::Sleep(1);
		}
	iDfc.SetDfcQ(Kern::DfcQue0());
	iReleaseDfc.SetDfcQ(Kern::DfcQue0());
	Outw(iSts, KPm1PwrBtn);								// write-1-to-clear a stale press
	iHandle = Interrupt::Bind(EX64IrqPciRouted | (TInt)(sci & 0xF), Isr, this);
	if (iHandle < 0)
		return iHandle;
	Outw(iEn, (TUint16)(Inw(iEn) | KPm1PwrBtn));
	Interrupt::Enable(iHandle);
	Kern::Printf("[power] ACPI power button: PM1 event %04x, SCI irq %d", iSts, sci & 0xFFFF);
	return KErrNone;
	}

void TX64PowerButton::Isr(TAny* aPtr)
	{
	TX64PowerButton& b = *(TX64PowerButton*)aPtr;
	if (Inw(b.iSts) & KPm1PwrBtn)
		{
		Outw(b.iSts, KPm1PwrBtn);
		b.iDfc.Add();
		}
	}

void TX64PowerButton::PressDfc(TAny* aPtr)
	{
	TX64PowerButton& b = *(TX64PowerButton*)aPtr;
	if (b.iHeld)
		return;
	b.iHeld = ETrue;
	TRawEvent e;
	e.Set(TRawEvent::EKeyDown, EStdKeyDevice2);
	Kern::AddEvent(e);
	Kern::Printf("[power] power button -> power key held %d ms (SysAp: switch off)", KPowerKeyHoldMs);
	b.iRelease.OneShot(NKern::TimerTicks(KPowerKeyHoldMs));
	}

void TX64PowerButton::ReleaseIsr(TAny* aPtr)
	{
	((TX64PowerButton*)aPtr)->iReleaseDfc.Add();			// timer callback: ISR context, only queue the DFC
	}

void TX64PowerButton::ReleaseDfc(TAny* aPtr)
	{
	TX64PowerButton& b = *(TX64PowerButton*)aPtr;
	TRawEvent e;
	e.Set(TRawEvent::EKeyUp, EStdKeyDevice2);
	Kern::AddEvent(e);
	b.iHeld = EFalse;
	}

void X64StartPower()
	{
	DX64PowerController* c = new DX64PowerController;
	DX64PowerHandler* h = new DX64PowerHandler;
	if (!c || !h)
		Kern::Fault("X64Power", KErrNoMemory);
	c->Register();
	h->Add();
	TX64PowerButton* button = new TX64PowerButton;
	if (button && button->Start() != KErrNone)
		Kern::Printf("[power] no ACPI power button");
	}
