/* Portions Copyright (c) Nokia Corporation and/or its subsidiary(-ies)
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

// tools/phorepoc-x86/port/x64smp/nk_plat.h
//
// PHOREPOC amd64 SMP nanokernel platform header -- the 64-bit sibling of EKA2's
// e32/include/nkernsmp/x86/nk_plat.h. Authored, not RE'd: the arch backend header
// the arch-NEUTRAL nkernsmp TUs include via <nk_plat.h> (from nk_priv.h:518).
// Selected by -I tools/phorepoc-x86/port/x64smp so the baseline tree is untouched.
//
// SMP slice 2b, step 1: the goal here is to let the NEUTRAL nkernsmp core compile
// for amd64. The neutral core embeds TSubSchedulerX/TSchedulerX BY VALUE and only
// asserts their SIZE (256 / 128 bytes) -- it never reads their fields (verified:
// no iSSX./iSX. references in nkernsmp/*.cpp). So they are sized opaquely for now;
// the amd64 SMP BACKEND (ncsched/nccpu/ncirq/nctimer, step 2) will replace the
// padding with the real per-CPU fields (iTss, iAPICID, iIrqStackTop, freq ratios,
// timestamp offset, ...) modelled on the 32-bit x86 backend.
//
// Derived from e32/include/nkernsmp/x86/nk_plat.h; adapted to the SysV amd64 model
// (8-byte pointers, 64-bit register frames, [rsp]-as-qword barriers).

/**
 @file
 @internalComponent
*/

#ifndef __NK_X64SMP_H__
#define __NK_X64SMP_H__
#include <nk_cpu.h>

// TExcTrap/TPagingExcTrap::iState is TUint32[EXC_TRAP_CTX_SZ]. cpudefs.h says 10 for IA-32 (8 regs +
// esp + eip, 4 bytes each). The SysV amd64 non-volatile set is rip rsp rbx rbp r12-r15 = 8 x 8 bytes
// (r12-r15/rbx can hold full 64-bit values even under x32), so the trap context needs 16 words.
// This header is the arch header and is always included before kern_priv.h, so override it here.
#undef  EXC_TRAP_CTX_SZ
#define EXC_TRAP_CTX_SZ	16

class TSubScheduler;
class TScheduler;
struct TX86Tss;			// amd64 TSS (defined by the backend); referenced only by pointer

// --- Per-CPU / global arch extras -------------------------------------------
// Opaque, correctly-SIZED placeholders (see file header). The neutral core embeds
// these by value and only static-asserts their size; it never touches the fields.

// TSubScheduler member data -- must be exactly 256 bytes.
// SMP slice 2b step 2: the padding is now partly REAL per-CPU backend fields the
// amd64 SMP backend (x64s_ncsched.cpp) reads -- the APIC ID (IPI target), the IRQ
// nest/count pair (send_resched_ipi_and_wait handshake) and the timestamp offset
// (per-core TSC skew). The rest stays reserved so the 256-byte size holds.
#ifdef __ILP32__
// x32 ABI (THE Path C ABI): 4-byte pointers, so the IA-32 layout is simply valid -- take it
// field-for-field from include/nkernsmp/x86/nk_plat.h. kernel/x86/{cinit,ckernel,cutils}.cpp
// read iTss / iExcInfo / iCrashState, and the 256 / 128 byte size asserts hold as written.
struct TSubSchedulerX
	{
	TUint32				iSSXP[9];
	volatile TUint32	iIrqCount;				// count of interrupts handled
	TAny*				iExcInfo;				// pointer to exception info for crash debugger
	volatile TInt		iCrashState;			// 0=normal, 1=this CPU faulted, 2=this CPU has received an NMI and halted
	TUint32				iAPICID;				// Local APIC ID for this CPU (starts at -1)
	volatile TInt		iIrqNestCount;			// IRQ nest count for this CPU (starts at -1)
	TLinAddr			iIrqStackTop;			// Top of IRQ stack for this CPU
	TX86Tss*			iTss;					// Address of TSS for this CPU
	SRatioInv			iCpuFreqRI;				// Ratio of CPU frequency to maximum possible CPU frequency
	SRatioInv			iTimerFreqRI;			// Ratio of CPU local timer frequency to maximum possible

	volatile TUint64HL	iTimestampOffset;		// 64 bit value to add to CPU TSC to give NKern::Timestamp()

	TUint32				iSSXP2[36];
	TUint64				iSSXP3;					// one 64 bit value to guarantee alignment
	};

// TScheduler member data
struct TSchedulerX
	{
	TUint64				iTimerMax;				// Maximum per-CPU timer frequency (after prescaling)
	TUint32				iSXP[30];
	};
#else	// LP64 fallback (ABI=-m64): opaque, nkernsmp-only -- ekern is not built for it
struct TSubSchedulerX
	{
	TUint32		iAPICID;			// local APIC ID of this CPU (IPI destination)
	TInt		iIrqNestCount;		// -1 outside an ISR, increments while nested
	TUint32		iIrqCount;			// bumped on each IRQ entry (IPI-and-wait handshake)
	TUint32		iSSXPad0;
	TUint64		iTimestampOffset;	// TSC offset of this core vs the boot processor
	TUint64		iSSXP[29];			// reserved -> total 4*4 + 8 + 29*8 = 256 bytes
	};

// TScheduler member data -- must be exactly 128 bytes (32*4 in the x86 assert).
struct TSchedulerX
	{
	TUint64		iSXP[16];			// 16 * 8 = 128 bytes
	};
#endif

// --- Interrupt / IPI vector assignments (amd64) -----------------------------
#define CRASH_IPI_VECTOR			0x27
#define RESCHED_IPI_VECTOR			0x28
#define TIMESLICE_VECTOR			0x29
#define GENERIC_IPI_VECTOR			0x2A
#define TRANSFERRED_IRQ_VECTOR		0x2E
#define SPURIOUS_INTERRUPT_VECTOR	0x2F

extern "C" TSubScheduler* SubSchedulerLookupTable[256];	// look up subscheduler from APIC ID

#define IRQ_STACK_SIZE	4096

// The nanokernel scheduler/DFC/etc. are C++ (not machine-coded) in this port.
//#define __SCHEDULER_MACHINE_CODED__
//#define __DFC_MACHINE_CODED__
//#define __MSTIM_MACHINE_CODED__
//#define __PRI_LIST_MACHINE_CODED__
//#define __FAST_SEM_MACHINE_CODED__
//#define __FAST_MUTEX_MACHINE_CODED__

// --- Per-thread arch context ------------------------------------------------
class TX86RegSet;
class NThread : public NThreadBase
	{
public:
	TInt Create(SNThreadCreateInfo& anInfo, TBool aInitial);
	void Stillborn();
	void GetUserContext(TX86RegSet& aContext, TUint32& aAvailRegMask);
	void SetUserContext(const TX86RegSet& aContext, TUint32& aRegMask);
	void GetSystemContext(TX86RegSet& aContext, TUint32& aAvailRegMask);
	void CompleteContextSave();
public:
	TUint64	iCoprocessorState[64];	// state of FPU, SSE, SSE2 (fxsave/xsave area)
	};

__ASSERT_COMPILE(!(_FOFF(NThread,iCoprocessorState)&7));

// --- On-stack register frames (amd64) ---------------------------------------
// Unreferenced by the neutral core; the amd64 backend (step 2) drives these. Laid
// out for the SysV amd64 model: all slots 64-bit, general regs are r*x/r8..r15.

// Reschedule frame, relative to saved RSP.
struct SThreadReschedStack
	{
	TUint64 iCR0;
	TUint64 iReschedFlag;
	TUint64 iRip;
	TUint64 iReason;
	};

// Registers pushed on stack for all exceptions other than slow exec.
struct SThreadExcStack
	{
	TUint64	iRax;
	TUint64	iRbx;
	TUint64	iRcx;
	TUint64	iRdx;
	TUint64	iRsi;
	TUint64	iRdi;
	TUint64	iRbp;
	TUint64	iR8;
	TUint64	iR9;
	TUint64	iR10;
	TUint64	iR11;
	TUint64	iR12;
	TUint64	iR13;
	TUint64	iR14;
	TUint64	iR15;
	TUint64	iVector;
	TUint64	iError;
	TUint64	iRip;			// hardware iret frame from here down
	TUint64	iCs;
	TUint64	iRflags;
	TUint64	iRsp3;			// always pushed in long mode
	TUint64	iSs3;
	};

// Stack structure at thread creation.
struct SThreadInitStack
	{
	enum {EVector=0xfffffffeu};
	SThreadReschedStack		iR;
	SThreadExcStack			iX;
	};

extern "C" {
GLREF_D TLinAddr X86_IrqHandler;
GLREF_D SCpuIdleHandler CpuIdleHandler;
GLREF_D TUint32 X86_CPUID;
GLREF_D TBool X86_UseGlobalPTEs;
GLREF_D TUint64 DefaultCoprocessorState[64];
}

/** Ensure the ordering of explicit memory writes.
	On amd64 stores are ordered, so this is a no-op. */
#define	wmb()
#define	smp_wmb()	wmb()

/** Ensure the ordering of explicit memory accesses.
	Use the compiler's full barrier so this is independent of the asm dialect
	(the baseline SMP TUs are assembled -masm=intel for the __crash `int 0xff`). */
#define	mb()		__sync_synchronize()
#define smp_mb()	mb()

/** Send reschedule IPIs to the CPUs identified by the bit mask. */
extern "C" void send_resched_ipis(TUint32 aMask);

#endif
