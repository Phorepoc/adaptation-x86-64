// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// xk_interrupt.cpp -- the Phorepoc amd64 board's device interrupts: the variant's Interrupt API over
// the nanokernel's NIrq layer, on an IOAPIC. X4.21.
//
// Layering, top down:
//   - drivers call Interrupt::Bind/Enable/... (include/kernel/x86/assp.h: "an implementation for each
//     of the functions ... must be provided by the Variant"). OURS -- the 2010 drop has no x86 variant.
//   - which is a thin shim over NKern::InterruptInit/Bind/Enable (the REAL nkernsmp/nk_irq.cpp: handler
//     lists, tied events, level-triggered masking). The SMP shape, as in the NaviEngine variant: Bind
//     returns a HANDLE, which Enable/Disable/Unbind accept (as they accept the plain id).
//   - whose hardware half is the REAL IA-32 nkernsmp/x86/ncirq.cpp, compiled unmodified: TIoApic at
//     0xFEC00000 (identity-mapped by xboot.cpp), redirection entries in flat LOGICAL destination mode
//     (NIrq::HwInit1/HwInit2AP give CPU n logical id 1<<n; every pin starts aimed at CPU 0), EOI to the
//     local APIC.
//   - and the entry: xk_irq.cpp sends vectors KX64VecIrqBase + GSI to NKern::Interrupt(GSI), i.e.
//     NIrq::HwIsr. (IA-32 put device vectors at 0x30 and up behind X86_IrqHandler; our local-APIC tick
//     already sits at 0x40, so devices start at 0x50.)
//
// Interrupt ids: 0-15 are ISA irqs (keyboard 1, mouse 12, IDE 14/15 ...), translated through the ACPI
// MADT's interrupt source overrides (xboot.cpp -> xk_board.h) to an IOAPIC pin = GSI, with the
// override's polarity/trigger; no override = GSI n, edge, active high. Ids >= 16 are GSIs (PCI:
// level, active low). NIrq[i] is IOAPIC pin i; it is initialised on first Bind.
#include <kernel/kern_priv.h>
#include <nk_irq.h>
#include <assp.h>
#include "xk_board.h"
#include "xk_interrupt.h"

static TUint8 IsaMap[16];				// copied from the super page at Init1 (xk_board.h encoding)
static TUint32 PinReady;				// NIrq[pin] initialised

void X64InterruptInit1()
	{
	const TUint8* m = (const TUint8*)&Kern::SuperPage().iReserved[KX64SuperIsaMap];
	for (TInt i=0; i<16; ++i)
		IsaMap[i] = m[i];
	NIrq::HwInit1();					// boot CPU: flat logical destination, logical id 1
	*(volatile TUint32*)0xFEC00000u = 1;						// IOAPIC version register
	TInt pins = 1 + ((*(volatile TUint32*)0xFEC00010u >> 16) & 0xFF);
	Kern::Printf("[irq] IOAPIC: %d pins, device vectors 0x%02x+GSI; ISA irq0 -> GSI %d, irq1 -> %d, irq12 -> %d, irq14 -> %d",
					pins, KX64VecIrqBase, X64IsaToGsi(0), X64IsaToGsi(1), X64IsaToGsi(12), X64IsaToGsi(14));
	}

void X64InterruptInit2AP()
	{
	NIrq::HwInit2AP();					// this CPU: logical id 1<<cpu
	}

TInt X64IsaToGsi(TInt aId)
	{
	if (aId >= EX64IrqPciRouted && aId < EX64IrqPciRouted + 16)
		aId -= EX64IrqPciRouted;
	if (aId < 0 || aId >= KX64MaxGsi)
		return -1;
	if (aId < 16 && (IsaMap[aId] & KX64IsaOverride))
		return IsaMap[aId] & KX64IsaGsiMask;
	return aId;
	}

// id or handle -> what NKern's interrupt calls take (a handle passes through unchanged)
static TInt ToNk(TInt aId)
	{
	if (aId & NKern::EIrqCookieMask)
		return aId;
	return X64IsaToGsi(aId);
	}

static TInt EnsureInit(TInt aId)
	{
	TInt gsi = X64IsaToGsi(aId);
	if (gsi < 0)
		return KErrArgument;
	if (PinReady & (1u<<gsi))
		return gsi;
	TUint32 flags;
	if (aId >= EX64IrqPciRouted)
		{
		// PCI INTx via the PIRQ router: level. The MADT says the polarity when it overrides that ISA
		// IRQ (QEMU: 5/9/10/11 level, active HIGH); no override (VirtualBox) = PCI's active low (X4.47)
		TUint8 e = IsaMap[aId - EX64IrqPciRouted];
		TBool high = (e & KX64IsaOverride) && !(e & KX64IsaActiveLow);
		flags = NKern::EIrqInit_LevelLow | (high ? 2 : 0) | NKern::EIrqInit_Shared;	// 2 = high; PCI shares
		}
	else if (aId < 16)
		{
		TUint8 e = IsaMap[aId];
		TBool level = (e & KX64IsaOverride) && (e & KX64IsaLevel);
		TBool low = (e & KX64IsaOverride) && (e & KX64IsaActiveLow);
		flags = (level ? NKern::EIrqInit_LevelLow : NKern::EIrqInit_FallingEdge) | (low ? 0 : 2);	// 2 = high/rising
		}
	else
		flags = NKern::EIrqInit_LevelLow | NKern::EIrqInit_Shared;		// PCI INTx (shareable)
	TInt r = NKern::InterruptInit(gsi, flags, KX64VecIrqBase + gsi, gsi, NULL);
	if (r != KErrNone)
		return r;
	__e32_atomic_ior_ord32(&PinReady, 1u<<gsi);
	return gsi;
	}

EXPORT_C TInt Interrupt::Bind(TInt aId, TIsr aIsr, TAny* aPtr)
	{
	TInt gsi = EnsureInit(aId);
	if (gsi < 0)
		return gsi;
	return NKern::InterruptBind(gsi, aIsr, aPtr, 0, NULL);			// > 0: the handle
	}

EXPORT_C TInt Interrupt::Unbind(TInt aId)
	{
	TInt id = ToNk(aId);
	return id < 0 ? KErrArgument : NKern::InterruptUnbind(id);
	}

EXPORT_C TInt Interrupt::Enable(TInt aId)
	{
	TInt id = ToNk(aId);
	TInt r = id < 0 ? KErrArgument : NKern::InterruptEnable(id);
	return r > 0 ? KErrNone : r;
	}

EXPORT_C TInt Interrupt::Disable(TInt aId)
	{
	TInt id = ToNk(aId);
	TInt r = id < 0 ? KErrArgument : NKern::InterruptDisable(id);
	return r > 0 ? KErrNone : r;
	}

EXPORT_C TInt Interrupt::Clear(TInt /*aId*/)
	{
	return KErrNone;						// edge: nothing latched; level: the device clears its source
	}

EXPORT_C TInt Interrupt::SetPriority(TInt /*aId*/, TInt /*aPriority*/)
	{
	return KErrNotSupported;
	}
