/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

// xk_board.h -- boot-time facts the low boot stub (xboot.cpp) hands to the board code in the
// kernel (OURS), in SSuperPageBase::iReserved[] beside the CPU hand-over words (x64_root.h uses
// [0..10]). Not the machine-configuration area: the kernel validates and resets that at NV-RAM
// init, which is how the first attempt lost the framebuffer. X4.18.
#ifndef XK_BOARD_H
#define XK_BOARD_H

// the linear framebuffer GRUB set up (multiboot2 tag 8); iReserved[KX64SuperFbPhys] == 0 = none
const TInt KX64SuperFbPhys   = 16;		// physical base (below 4 GiB on every PC hypervisor)
const TInt KX64SuperFbPitch  = 17;		// bytes per scan line
const TInt KX64SuperFbWidth  = 18;
const TInt KX64SuperFbHeight = 19;
const TInt KX64SuperFbFormat = 20;		// bits per pixel | (framebuffer_type 1 = direct RGB) << 8

// ACPI MADT interrupt source overrides for the 16 ISA irqs: 16 BYTES from iReserved[21] (to [24]),
// byte n = ISA irq n. 0 = no override (GSI n, edge, active high -- the ISA default); else
// KX64IsaOverride | flags | GSI (0-31). X4.21 (xk_interrupt.cpp).
const TInt KX64SuperIsaMap   = 21;
const TUint8 KX64IsaOverride  = 0x80;
const TUint8 KX64IsaActiveLow = 0x40;
const TUint8 KX64IsaLevel     = 0x20;
const TUint8 KX64IsaGsiMask   = 0x1F;

// ACPI soft-off (S5) for xk_power.cpp: the FADT's PM1a/PM1b control-block ports and the DSDT's \_S5
// sleep types, word = port | SLP_TYP << 16 | KX64AcpiValid. 0 = not found (no ACPI: the power
// controller halts instead). The phone's "switch off" on a PC. X4.74.
// ... and the power BUTTON (X4.75): PM1a_EVT_BLK port | PM1_EVT_LEN << 16 (0 = none), SCI_INT | ACPI_ENABLE << 16,
// SMI_CMD (the port that switches the chipset from legacy to ACPI mode; 0 = already ACPI, e.g. hardware-reduced)
const TInt KX64SuperPm1Evt   = 11;
const TInt KX64SuperSci      = 12;
const TInt KX64SuperSmiCmd   = 13;
const TInt KX64SuperPm1a     = 14;
const TInt KX64SuperPm1b     = 15;
const TUint32 KX64AcpiValid  = 0x80000000u;

#endif
