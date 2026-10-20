// Portions Copyright (c) Nokia Corporation and/or its subsidiary(-ies)
// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// xk_smp.cpp -- Phorepoc amd64/x32 kernel image: application-processor bring-up.
// amd64 sibling of nkernsmp/x86/nccpu.cpp NKern::BootAP + the AP half of k_entry. The generic
// kernel drives it (kernel/x86/cinit.cpp A::InitAPs -> K::InitAP): for every CPU the variant
// reported it creates that CPU's initial (idle) thread and calls NKern::BootAP(info) with the
// thread's supervisor stack and iMain = ApMainGeneric; BootAP must get the CPU to call
// iMain(info) on that stack, in the kernel's address space, in a state equivalent to the boot CPU's.
// ApMainGeneric then does NKern::Init -> NThread::Create(initial) (the subscheduler comes to life),
// M::Init2AP, A::Init2AP (-> our variant starts this CPU's tick) and idles.

#include <x86_mem.h>
#include "../port/x64ekern/x64_root.h"

extern "C" {
extern const TUint8 xk_ap_tramp_start[], xk_ap_tramp_end[], xk_ap_params[];		// xk_aptramp.S
extern TSubScheduler* SubSchedulerLookupTable[256];
}
extern TUint8 X64_CpuOfApic[256];												// xk_entry.cpp
void X64_InstallDescriptorTables(TInt aCpu, TLinAddr aKernelStackTop);
void X64_EnableFpu();
extern "C" TUint32 X64_LapicCountsPerMs();									// xk_irq.cpp

struct SX64ApParams			// the block at the end of the trampoline (xk_aptramp.S p_*)
	{
	TUint32 iCr3, iEntry, iStack, iInfo, iCr4, iCr0, iEfer;
	volatile TUint32 iAlive;
	};

const TLinAddr KApTrampBase = 0x8000;		// SIPI vector 0x08; identity-mapped while an AP boots
static volatile TUint32 ApAlive;			// set by the AP once it runs in the high half

static inline TUint32 LapicRd(TUint aReg)				{ return *(volatile TUint32*)(0xFEE00000u+aReg); }
static inline void LapicWr(TUint aReg, TUint32 aVal)	{ *(volatile TUint32*)(0xFEE00000u+aReg) = aVal; }

static void SendInitIpi(TUint32 aApicId, TUint32 aIcrLow)
	{
	LapicWr(0x310, aApicId<<24);
	LapicWr(0x300, aIcrLow);
	for (TInt i=0; i<100000 && (LapicRd(0x300) & 0x1000); ++i) {}		// delivery status
	}

// First code an AP runs in the high half: on its initial thread's supervisor stack, interrupts
// off, on its own PML4 (= the kernel's address space).
extern "C" void X64_ApEntry(volatile SAPBootInfo* aInfo)
	{
	TInt cpu = X64_CpuOfApic[LapicRd(0x20)>>24];
	X64_InstallDescriptorTables(cpu, (TLinAddr)aInfo->iInitStackBase + aInfo->iInitStackSize);
	X64_EnableFpu();
	LapicWr(0xF0, 0x1FF);						// software-enable this local APIC (spurious = 0xFF)
	LapicWr(0x80, 0);							// task priority 0: accept everything
	ApAlive = 1;
	(*aInfo->iMain)(aInfo);						// ApMainGeneric: never returns
	for (;;) { __asm__ __volatile__("cli; hlt"); }
	}

TInt NKern::BootAP(volatile SAPBootInfo* aInfo)
	{
	const TInt cpu = TheScheduler.iNumCpus;		// the number NThread::Create(initial) will hand out next
	const TUint32 apic = aInfo->iCpu;
	const SSuperPageBase& s = Kern::SuperPage();
	Kern::Printf("[smp] BootAP: cpu %d, APIC id %d, initial stack %08x+%x", cpu, apic, aInfo->iInitStackBase, aInfo->iInitStackSize);
	if (cpu >= KMaxCpus || cpu >= KX64MaxBootCpus)
		return KErrNotSupported;
	X64_LapicCountsPerMs();					// calibrate now: the AP starts its tick (Init2AP) before our Init3 runs

	// This CPU's long-mode root (xboot.cpp made every root a copy of the kernel's address space).
	const TUint32 rootPhys = (TUint32)s.iReserved[KX64SuperRootPhys] + cpu*0x2000;
	const TLinAddr rootLin = (TLinAddr)s.iReserved[KX64SuperRootLin] + cpu*0x2000;
	X64_CpuRoot[cpu].iPml4Phys = rootPhys;
	X64_CpuRoot[cpu].iPdpt = (TUint64*)(rootLin + 0x1000);
	X64_CpuOfApic[apic & 0xFF] = (TUint8)cpu;

	// Temporary identity 2 MiB page at the bottom of the KERNEL's local page directory: lets us write
	// the trampoline at physical 0x8000 and keeps it mapped for the AP across its CR0.PG flip. We must
	// be running in the kernel's address space for that (true for the boot-time supervisor thread).
	TUint64 cr3;
	__asm__ __volatile__("mov %0, cr3" : "=r"(cr3));
	if (((TUint32)cr3 & ~0xFFFu) != (TUint32)X64_CpuRoot[0].iPml4Phys
			|| (X64_CpuRoot[0].iPdpt[0] & ~0xFFFull) != (X64_CpuRoot[cpu].iPdpt[0] & ~0xFFFull))
		{
		Kern::Printf("[smp] BootAP: not in the kernel address space -- refusing");
		return KErrGeneral;
		}
	TPde* pd = (TPde*)KPageDirectoryBase;
	pd[0] = 0x000000u | KPdePtePresent | KPdePteWrite | KPdeLargePage;		// not global: dies with the next CR3 load
	__asm__ __volatile__("mov rax, cr3; mov cr3, rax" : : : "rax");

	const TUint size = (TUint)(xk_ap_tramp_end - xk_ap_tramp_start);
	memcpy((TAny*)KApTrampBase, xk_ap_tramp_start, size);
	SX64ApParams& p = *(SX64ApParams*)(KApTrampBase + (xk_ap_params - xk_ap_tramp_start));
	TUint64 cr0, cr4;
	__asm__ __volatile__("mov %0, cr0" : "=r"(cr0));
	__asm__ __volatile__("mov %0, cr4" : "=r"(cr4));
	TUint32 eferLo, eferHi;
	__asm__ __volatile__("rdmsr" : "=a"(eferLo), "=d"(eferHi) : "c"(0xC0000080u));
	p.iCr3 = rootPhys;
	p.iEntry = (TUint32)(TLinAddr)&X64_ApEntry;
	p.iStack = ((TUint32)aInfo->iInitStackBase + (TUint32)aInfo->iInitStackSize) & ~0xFu;
	p.iInfo = (TUint32)(TLinAddr)aInfo;
	p.iCr4 = (TUint32)cr4;
	p.iCr0 = (TUint32)cr0 & ~0x8u;				// never start with TS set
	p.iEfer = eferLo & ~0x400u;					// LMA is read-only status
	ApAlive = 0;

	// INIT, 10 ms, then two STARTUP IPIs 200 us apart (the MP specification's universal algorithm).
	SendInitIpi(apic, 0x00004500);
	Kern::NanoWait(10000000);
	for (TInt i=0; i<2 && !ApAlive; ++i)
		{
		SendInitIpi(apic, 0x00004600 | (KApTrampBase>>12));
		Kern::NanoWait(200000);
		}
	for (TInt i=0; i<1000 && !ApAlive; ++i)
		Kern::NanoWait(1000000);				// up to ~1 s

	pd[0] = 0;
	__asm__ __volatile__("mov rax, cr3; mov cr3, rax" : : : "rax");
	if (!ApAlive)
		{
		Kern::Printf("[smp] BootAP: cpu %d (APIC %d) did not start", cpu, apic);
		return KErrTimedOut;
		}
	Kern::Printf("[smp] BootAP: cpu %d is running in the high half", cpu);
	return KErrNone;
	}
