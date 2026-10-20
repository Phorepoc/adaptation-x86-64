/* Portions Copyright (c) Nokia Corporation and/or its subsidiary(-ies)
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

// tools/phorepoc-x86/port/x64/nk_plat.h
//
// PHOREPOC amd64 nanokernel platform header -- the 64-bit sibling of EKA2's
// e32/include/nkern/x86/nk_plat.h. Authored, not RE'd: this is the arch backend
// header the arch-NEUTRAL nkern TUs include via <nk_plat.h>. Kept OUT of the
// baseline tree (selected by -I tools/phorepoc-x86/port/x64) so no baseline edits.
//
// Derived from the x86 (IA-32) version; adapted to the SysV amd64 model:
//   - SThreadStack holds the 64-bit callee-saved set (rbx,rbp,r12-r15) + rip.
//   - mb() barriers operate on [rsp] as a 64-bit slot.
// The NThread coprocessor-state block (FPU/SSE/SSE2) is unchanged in shape.

/**
 @file
 @internalComponent
*/

#ifndef __NK_X64_H__
#define __NK_X64_H__
#include <nk_cpu.h>

#define IRQ_STACK_SIZE	4096

//#define __SCHEDULER_MACHINE_CODED__
//#define __DFC_MACHINE_CODED__
//#define __MSTIM_MACHINE_CODED__
//#define __PRI_LIST_MACHINE_CODED__
//#define __FAST_SEM_MACHINE_CODED__
//#define __FAST_MUTEX_MACHINE_CODED__

// TScheduler member data
#define	i_ExcInfo			iExtras[15]		// pointer to exception info for crash debugger

class TX86RegSet;
class NThread : public NThreadBase
	{
public:
	TInt Create(SNThreadCreateInfo& anInfo, TBool aInitial);
	inline void Stillborn()
		{}
	void GetUserContext(TX86RegSet& aContext);
	void ModifyUsp(TLinAddr aUsp);
public:
	TUint32	i_NThread_Pad1;
	TUint64	iCoprocessorState[64];	// state of FPU, SSE, SSE2 (and AVX slack)
	};

__ASSERT_COMPILE(!(_FOFF(NThread,iCoprocessorState)&7));

// Positions of registers on stack, relative to saved SP.
// amd64 SysV callee-saved set. All slots are 64-bit.
struct SThreadStack
	{
	TUint64 iCR0;
	TUint64 iRbx;
	TUint64 iRbp;
	TUint64 iR12;
	TUint64 iR13;
	TUint64 iR14;
	TUint64 iR15;
	TUint64 iGs;
	TUint64 iFs;
	TUint64 iReschedFlag;
	TUint64 iRip;
	};

extern "C" {
GLREF_D TUint64 X86_IrqStack[IRQ_STACK_SIZE/8];
GLREF_D TLinAddr X86_IrqHandler;
GLREF_D TInt X86_IrqNestCount;
GLREF_D SCpuIdleHandler CpuIdleHandler;
GLREF_D TBool X86_UseGlobalPTEs;
GLREF_D TUint64 DefaultCoprocessorState[64];
}


/** Ensure the ordering of explicit memory writes

	On x86/amd64 stores are ordered, so this is a no-op
*/
#define	wmb()
#define smp_wmb()

/** Ensure the ordering of explicit memory accesses

	Any instruction with the LOCK prefix does this
*/
#ifdef __GCC32__
#define	mb()	__asm__ __volatile__("lock add qword ptr [rsp], 0" : : : "memory")
#else
#define	mb()	do { _asm lock add qword ptr [rsp], 0 } while (0)
#endif
#define smp_mb()

// End of file
#endif
