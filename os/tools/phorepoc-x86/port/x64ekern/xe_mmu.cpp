// Portions Copyright (c) Nokia Corporation and/or its subsidiary(-ies)
// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xe_mmu.cpp -- Phorepoc amd64/x32: MMU primitives + address-space switch + the board/variant
// symbols the kernel expects. amd64 siblings of memmodel/epoc/multiple/x86/{xmmu,xsched}.cia.
// X64_TODO = honest first cut.
//
// NOTE (the big one): the C++ in multiple/x86/xmmu.cpp COMPILES under x32 but drives IA-32
// 2-level page tables (4-byte PDE/PTE, 4 MiB chunks). Long mode walks 4-level tables with
// 8-byte entries, so that file needs a real amd64 sibling before the memory model can run:

#include <x86_mem.h>
#include "x64_root.h"

extern "C" TSubScheduler& SubScheduler();		// x64s_ncutils.cpp

static inline TUint64 ReadCr3()  { TUint64 v; __asm__ __volatile__("mov %0, cr3" : "=r"(v)); return v; }
static inline void WriteCr3(TUint64 v) { __asm__ __volatile__("mov cr3, %0" : : "r"(v)); }
static inline TUint64 ReadCr4()  { TUint64 v; __asm__ __volatile__("mov %0, cr4" : "=r"(v)); return v; }
static inline void WriteCr4(TUint64 v) { __asm__ __volatile__("mov cr4, %0" : : "r"(v)); }

// The multiple model declares the TLB primitives extern "C", the flexible model as C++.
#ifdef __MEMMODEL_MULTIPLE__
extern "C" {
#endif

// One page, this CPU.
void DoInvalidateTLBForPage(TLinAddr aLinAddr)
	{
	__asm__ __volatile__("invlpg [%0]" : : "r"(aLinAddr) : "memory");
	}

// All non-global entries, this CPU.
void DoLocalInvalidateTLB()
	{
	WriteCr3(ReadCr3());
	}

// Everything including global pages, this CPU: toggle CR4.PGE (bit 7) around a CR3 reload.
void DoTotalInvalidateTLB()
	{
	TInt irq = NKern::DisableAllInterrupts();
	TUint64 cr4 = ReadCr4();
	WriteCr4(cr4 & ~(TUint64)0x80);
	WriteCr3(ReadCr3());
	WriteCr4(cr4);
	NKern::RestoreInterrupts(irq);
	}

// "Regardless of CPU type" flush (xmmu.cia DoInvalidateTLB). The cross-CPU shootdown is the
// C++ caller's job on SMP (xmmu.cpp TTLBIPI), exactly as on IA-32.
void DoInvalidateTLB()
	{
	if (X86_UseGlobalPTEs)
		DoTotalInvalidateTLB();
	else
		DoLocalInvalidateTLB();
	}

#ifdef __MEMMODEL_MULTIPLE__
}
#else
#endif

// Address-space switch on reschedule. IA-32 entered this from the asm scheduler with the new
// thread in EBX; our Reschedule is C++ and calls the board_switch_as(NThreadBase*) hook, so the
// real work lives in X64_ProcessSwitch and the raw register-convention entry must never run.
//
// LONG-MODE ROOT (flexible model). EKA2 knows ONE level above the page tables: a 16 KiB "page
// directory" per address space = 2048 x 8-byte PDEs = exactly the four long-mode PD pages that cover
// the 4 GiB x32 space. With KPageDirectoryShift(14) > KPageShift the generic allocator (mpdalloc.cpp)
// takes its ARM-style split path: a process owns only the LOCAL half (2 pages, VA 0..2 GiB) and the
// GLOBAL half (2 pages, VA 2..4 GiB) is the kernel's, physically shared by every address space.
// Long mode adds two levels on top (PML4 -> PDPT). They carry no per-process information beyond
// "which two local PD pages", so they are PER-CPU: the bootstrap gives each CPU one PML4 + one PDPT
// whose PDPT[2],[3] point at the shared global PD pages for ever; an address-space switch re-points
// PDPT[0],[1] at the incoming process's local PD and reloads CR3. No per-process memory, no generic
// change, SMP-safe (a CPU only ever edits its own PDPT). DMemModelProcess::iPageDir keeps its generic
// meaning (physical address of the local PD).
SX64CpuRoot X64_CpuRoot[KMaxCpus];			// x64_root.h; filled by the kernel entry / AP bring-up

const TUint64 KX64PdptEntryFlags = 0x07;	// present | write | user: permissions are enforced lower down

static inline void X64_LoadAddressSpace(TPhysAddr aLocalPd)
	{
	SX64CpuRoot& r = X64_CpuRoot[NKern::CurrentCpu()];
	r.iPdpt[0] = (TUint64)aLocalPd | KX64PdptEntryFlags;
	r.iPdpt[1] = ((TUint64)aLocalPd + 0x1000) | KX64PdptEntryFlags;
	WriteCr3(r.iPml4Phys);					// flushes every non-global TLB entry
	}
#ifdef __MEMMODEL_MULTIPLE__
#define X64_PROCESS_ROOT(p)	((p)->iGlobalPageDir)
#else
#define X64_PROCESS_ROOT(p)	((p)->iPageDir)
#endif
extern "C" void X64_ProcessSwitch(NThreadBase* aNew)
	{
	TSubScheduler& ss = SubScheduler();
	DMemModelProcess* p = (DMemModelProcess*)aNew->iAddressSpace;
	if (p && (TAny*)p != ss.iAddressSpace)
		{
		X64_LoadAddressSpace(X64_PROCESS_ROOT(p));
		ss.iAddressSpace = p;
		}
	DMemModelThread* t = (DMemModelThread*)_LOFF(aNew, DThread, iNThread);
	if (t->iAliasLinAddr)
		{	// an IPC alias was live when this thread was switched out: restore its PDE
		TInt irq = NKern::DisableAllInterrupts();	// vs the alias-removal IPI (flexible/x86/xsched.cia)
		*t->iAliasPdePtr = t->iAliasPde;
		DoInvalidateTLBForPage(t->iAliasLinAddr);
		NKern::RestoreInterrupts(irq);
		}
	}

#ifdef __MEMMODEL_FLEXIBLE__
// After temporarily running in another address space: back to the current thread's own process.
void DMemModelThread::RestoreAddressSpace()
	{
	TInt irq = NKern::DisableAllInterrupts();
	NThread* n = NKern::CurrentThread();
	DMemModelProcess* p = (DMemModelProcess*)_LOFF(n, DThread, iNThread)->iOwningProcess;
	SubScheduler().iAddressSpace = p;
	n->iAddressSpace = p;
	X64_LoadAddressSpace(X64_PROCESS_ROOT(p));
	NKern::RestoreInterrupts(irq);
	}

// mm.cpp found a user access it will not service and wants the exception raised AT aAddr, as if the
// user-permission access itself had faulted (IA-32: touch it through the ring-3 segment). Touch it; if
// the page is present for the kernel the CPU will not fault for us -> raise it explicitly.
extern "C" void X64_UserRangeFault(TLinAddr aAddr);
void UserWriteFault(TLinAddr aAddr)
	{
	X64_UserRangeFault(aAddr);
	}
void UserReadFault(TLinAddr aAddr)
	{
	X64_UserRangeFault(aAddr);
	}
#endif

void DoProcessSwitch()
	{
	NKFault("DoProcessSwitch: asm-convention entry; use X64_ProcessSwitch", 0);
	}

/******************************************************************************
 * Board / variant (the PC baseport is net-new: there is no x86 BSP in the 2010 tree)
 ******************************************************************************/
TLinAddr ApTrampolinePage = 0;		// set by the board's AP bring-up (smp.S copies to 0x8000)

// Bind the interrupt controller + install the kernel's vectors. X64_TODO(X4.2c): the board
// already owns IDT/PIC/LAPIC (kmain_smp.c, isr.S); this becomes the hand-over point.
void X86::Init1Interrupts()
	{
	}
