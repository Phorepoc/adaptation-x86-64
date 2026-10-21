/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// xk_interrupt.h -- the board's device-interrupt constants and hooks (xk_interrupt.cpp). X4.21.
#ifndef XK_INTERRUPT_H
#define XK_INTERRUPT_H

const TUint KX64VecIrqBase = 0x50;		// IOAPIC pin (GSI) n -> vector 0x50+n
const TInt  KX64MaxGsi     = 24;		// pins handled (the PC IOAPIC has 24)

// ISA interrupt ids (Interrupt::Bind): translated through the MADT overrides
const TInt EX64IrqKeyboard = 1;
const TInt EX64IrqMouse    = 12;
const TInt EX64IrqIdePri   = 14;
const TInt EX64IrqIdeSec   = 15;
// X4.43: a PCI INTx the chipset's PIRQ router sent to ISA IRQ n (the PCI interrupt-line register):
// Interrupt::Bind(EX64IrqPciRouted | n) = that ISA pin, but level-triggered, active low, as PCI is
const TInt EX64IrqPciRouted = 0x40;

void X64InterruptInit1();				// variant Init1, boot CPU, interrupts off
void X64InterruptInit2AP();				// variant Init2AP, on each application processor
TInt X64IsaToGsi(TInt aId);

#endif
