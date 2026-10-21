/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

// x64_root.h -- the per-CPU long-mode root tables (design: xe_mmu.cpp, docs X4.3b).
#ifndef X64_ROOT_H
#define X64_ROOT_H

struct SX64CpuRoot
	{
	TUint64*	iPdpt;			// linear address of this CPU's PDPT page (kernel mapping)
	TUint64		iPml4Phys;		// physical address of this CPU's PML4 (the CR3 value)
	};
extern SX64CpuRoot X64_CpuRoot[KMaxCpus];

// Bootstrap hand-over (xboot.cpp): SSuperPageBase::iReserved[0] = physical, [1] = linear address of
// the root area; CPU n owns two pages there: PML4 at +n*0x2000, PDPT at +n*0x2000+0x1000.
const TInt KX64SuperRootPhys = 0;
const TInt KX64SuperRootLin  = 1;
// ... and the processors the firmware reported (ACPI MADT, enabled local APICs; boot CPU first):
// iReserved[2] = count (0 = no MADT found: uniprocessor), iReserved[3+n] = local APIC id of CPU n.
const TInt KX64SuperCpuCount = 2;
const TInt KX64SuperCpuApicId = 3;
const TInt KX64MaxBootCpus   = 8;

#endif
