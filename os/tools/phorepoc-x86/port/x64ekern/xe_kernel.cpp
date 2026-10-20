// Portions Copyright (c) Nokia Corporation and/or its subsidiary(-ies)
// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// xe_kernel.cpp -- Phorepoc amd64/x32: the kernel-level (ekern) arch backend. amd64 siblings of
// the IA-32 MSVC-asm files kernel/x86/{ckernel,cexec,cutils,csched}.cia + klib/x86/cumem.cia.
// C++ wherever the original was asm only for speed; naked asm only where register state IS the
// semantics (the exception traps, the drop to ring 3). Built -masm=intel like the baseline TUs.
// Surface list: docs/phorepoc/ekern-x32-backend-surface.txt. X64_TODO = honest first cut.

#include <x86_mem.h>
#include <u32std.h>

/******************************************************************************
 * User-memory access (klib/x86/cumem.cia, kernel/x86/csched.cia)
 *
 * IA-32 EKA2 enforced "with user permissions" by SEGMENTATION: it loaded the ring-3
 * data selector (limit = user area) into DS/ES and let the CPU fault. Long mode has
 * no segment limits, so the amd64 rule is: range-check against the user/kernel
 * split, then copy; page-level U/S + the #PF path cover unmapped addresses.
 ******************************************************************************/

// Top of the user-visible linear range. Everything the multiple AND flexible x86 maps
// give ring 3 lies below their kernel section; the memory model may tighten this.
__attribute__((weak)) TLinAddr X64_UserLimit = 0xC0000000u;

// "Previous mode": did the current exec call come from ring 3? (IA-32 kept the caller's
// RPL in GS.) The real one is ekern/xk_irq.cpp (the gate frame's saved CS); this weak default
// only serves images without an exec gate.
extern "C" __attribute__((weak)) TBool X64_CallerIsUser() { return EFalse; }

// A user pointer reached into kernel space. IA-32 took a #GP here and ran the normal
// exception path (kill the thread / fire its TExcTrap). The real one is ekern/xk_irq.cpp
// (a synthesized #GP through Exc::Dispatch); this weak default faults loudly.
extern "C" __attribute__((weak)) void X64_UserRangeFault(TLinAddr aAddr)
	{
	NKFault("user ptr in kernel range", (TInt)aAddr);
	}

// Fault-recovering copy. 0 = ok, -1 = faulted. IA-32 used the thread's "magic" exception handler
// (cutils.cia); the kernel's own exception trap does the same job in C++: a #PF inside the copy
// reaches Exc::Dispatch -> this TExcTrap -> TExcTrap::Exception longjmps back here.
extern "C" TInt safe_copy(TAny* aDst, const TAny* aSrc, TUint64 aLen)
	{
	TInt r = KErrNone;
	XTRAP(r, XT_DEFAULT, memcpy(aDst, aSrc, (TUint)aLen));
	return r==KErrNone ? 0 : -1;
	}

static inline void UserCheck(const TAny* aAddr, TInt aLength)
	{
	TLinAddr s = (TLinAddr)aAddr;
	TLinAddr e = s + (TUint)aLength;
	if (aLength<0 || e<s || e>X64_UserLimit)
		X64_UserRangeFault(s);
	}

static inline void KUCheck(const TAny* aAddr, TInt aLength)
	{
	if (X64_CallerIsUser())
		UserCheck(aAddr, aLength);
	}

extern "C" {

EXPORT_C void umemget(TAny* aKernAddr, const TAny* aUserAddr, TInt aLength)
	{ UserCheck(aUserAddr, aLength); memcpy(aKernAddr, aUserAddr, aLength); }
EXPORT_C void umemget32(TAny* aKernAddr, const TAny* aUserAddr, TInt aLength)
	{ UserCheck(aUserAddr, aLength); memcpy(aKernAddr, aUserAddr, aLength); }
EXPORT_C void umemput(TAny* aUserAddr, const TAny* aKernAddr, TInt aLength)
	{ UserCheck(aUserAddr, aLength); memcpy(aUserAddr, aKernAddr, aLength); }
EXPORT_C void umemput32(TAny* aUserAddr, const TAny* aKernAddr, TInt aLength)
	{ UserCheck(aUserAddr, aLength); memcpy(aUserAddr, aKernAddr, aLength); }
EXPORT_C void umemset(TAny* aUserAddr, const TUint8 aValue, TInt aLength)
	{ UserCheck(aUserAddr, aLength); memset(aUserAddr, aValue, aLength); }

EXPORT_C void kumemget(TAny* aKernAddr, const TAny* aAddr, TInt aLength)
	{ KUCheck(aAddr, aLength); memcpy(aKernAddr, aAddr, aLength); }
EXPORT_C void kumemget32(TAny* aKernAddr, const TAny* aAddr, TInt aLength)
	{ KUCheck(aAddr, aLength); memcpy(aKernAddr, aAddr, aLength); }
EXPORT_C void kumemput(TAny* aAddr, const TAny* aKernAddr, TInt aLength)
	{ KUCheck(aAddr, aLength); memcpy(aAddr, aKernAddr, aLength); }
EXPORT_C void kumemput32(TAny* aAddr, const TAny* aKernAddr, TInt aLength)
	{ KUCheck(aAddr, aLength); memcpy(aAddr, aKernAddr, aLength); }
EXPORT_C void kumemset(TAny* aAddr, const TUint8 aValue, TInt aLength)
	{ KUCheck(aAddr, aLength); memset(aAddr, aValue, aLength); }

void uumemcpy(TAny* aUserDst, const TAny* aUserSrc, TInt aLength)
	{ UserCheck(aUserSrc, aLength); UserCheck(aUserDst, aLength); memmove(aUserDst, aUserSrc, aLength); }
void uumemcpy32(TAny* aUserDst, const TAny* aUserSrc, TInt aLength)
	{ uumemcpy(aUserDst, aUserSrc, aLength); }

}

// The "Safe" family never kills the caller: NULL = ok, else the address that faulted.
// X64_TODO: safe_copy reports only "faulted", so the START address is returned, not the
// exact faulting byte (callers only test for non-NULL).
static TAny* SafeCopy(TAny* aDest, const TAny* aSrc, TInt aSize, const TAny* aBlame)
	{
	return safe_copy(aDest, aSrc, (TUint64)(TUint)aSize) ? (TAny*)aBlame : NULL;
	}
static inline TBool UserRangeBad(const TAny* aAddr, TInt aSize)
	{
	TLinAddr s = (TLinAddr)aAddr, e = s + (TUint)aSize;
	return (aSize<0 || e<s || e>X64_UserLimit);
	}

TAny* K::USafeRead(const TAny* aSrc, TAny* aDest, TInt aSize)
	{ return UserRangeBad(aSrc,aSize) ? (TAny*)aSrc : SafeCopy(aDest,aSrc,aSize,aSrc); }
TAny* K::USafeWrite(TAny* aDest, const TAny* aSrc, TInt aSize)
	{ return UserRangeBad(aDest,aSize) ? aDest : SafeCopy(aDest,aSrc,aSize,aDest); }
EXPORT_C TAny* Kern::SafeRead(const TAny* aSrc, TAny* aDest, TInt aSize)
	{ return SafeCopy(aDest,aSrc,aSize,aSrc); }
EXPORT_C TAny* Kern::SafeWrite(TAny* aDest, const TAny* aSrc, TInt aSize)
	{ return SafeCopy(aDest,aSrc,aSize,aDest); }
EXPORT_C TAny* Kern::KUSafeRead(const TAny* aSrc, TAny* aDest, TInt aSize)
	{ return X64_CallerIsUser() ? K::USafeRead(aSrc,aDest,aSize) : Kern::SafeRead(aSrc,aDest,aSize); }
EXPORT_C TAny* Kern::KUSafeWrite(TAny* aDest, const TAny* aSrc, TInt aSize)
	{ return X64_CallerIsUser() ? K::USafeWrite(aDest,aSrc,aSize) : Kern::SafeWrite(aDest,aSrc,aSize); }

// cutils.cia: atomic inc/dec of a counter that only moves while positive; previous value back.
EXPORT_C TInt Kern::KUSafeInc(TInt& aValue)
	{
	KUCheck(&aValue, sizeof(TInt));
	TInt o;
	do	{ o = aValue; if (o<=0) return o; }
	while (!__sync_bool_compare_and_swap(&aValue, o, o+1));
	return o;
	}
EXPORT_C TInt Kern::KUSafeDec(TInt& aValue)
	{
	KUCheck(&aValue, sizeof(TInt));
	TInt o;
	do	{ o = aValue; if (o<=0) return o; }
	while (!__sync_bool_compare_and_swap(&aValue, o, o-1));
	return o;
	}

/******************************************************************************
 * Exception traps (kernel/x86/ckernel.cia) -- setjmp/longjmp for kernel code.
 * iState = { rip, rsp, rbx, rbp, r12, r13, r14, r15 } as 8 qwords (EXC_TRAP_CTX_SZ is
 * raised to 16 words in x64smp/nk_plat.h). The naked entry saves the CALLER's
 * non-volatile context, then tail-jumps to a C++ helper for the bookkeeping, so the
 * helper's plain `ret` returns 0 straight to Trap()'s caller.
 ******************************************************************************/
extern "C" {
void DefaultExcTrapHandler(TExcTrap* xt, DThread*, TAny*);	// kernel/x86/ckernel.cpp
void IpcExcHandler(TExcTrap* xt, DThread* aThread, TAny* aContext);

TInt X64_ExcTrapEnter(TExcTrap* aTrap, TExcTrapHandler aHandler)
	{
	DThread* t = TheCurrentThread;
	aTrap->iHandler = aHandler ? aHandler : &DefaultExcTrapHandler;
	aTrap->iThread = t;
	t->iExcTrap = aTrap;
	return 0;
	}
TInt X64_IpcExcTrapEnter(TIpcExcTrap* aTrap, DThread* aClient)
	{
	X64_ExcTrapEnter(aTrap, &IpcExcHandler);
	aTrap->iThread->iIpcClient = aClient;
	return 0;
	}
void X64_ExcTrapLeave(TExcTrap* aTrap)
	{
	DThread* t = aTrap->iThread;
	t->iExcTrap = NULL;
	t->iPagingExcTrap = NULL;
	}
}

#include "xe_exctrap_asm.h"		// X64_SAVE_NONVOLATILE / X64_RESUME_NONVOLATILE (run-tested: exctrap_test.c)

EXPORT_C __attribute__((naked)) TInt TExcTrap::Trap()
	{
	__asm__(X64_SAVE_NONVOLATILE "xor esi, esi\n" "jmp X64_ExcTrapEnter\n");
	}
EXPORT_C __attribute__((naked)) TInt TExcTrap::Trap(TExcTrapHandler /*aHandler*/)
	{
	__asm__(X64_SAVE_NONVOLATILE "jmp X64_ExcTrapEnter\n");		// rsi = aHandler already
	}
__attribute__((naked)) TInt TIpcExcTrap::Trap(DThread* /*aClient*/)
	{
	__asm__(X64_SAVE_NONVOLATILE "jmp X64_IpcExcTrapEnter\n");	// rsi = aClient already
	}
EXPORT_C __attribute__((naked)) void TExcTrap::Exception(TInt /*aResult*/)
	{
	__asm__("push rdi\n" "push rsi\n" "sub rsp, 8\n"			// entry rsp%16==8 -> 0 for the call
			"call X64_ExcTrapLeave\n"
			"add rsp, 8\n" "pop rsi\n" "pop rdi\n"
			X64_RESUME_NONVOLATILE);
	}
TBool TIpcExcTrap::IsTIpcExcTrap()
	{
	return iHandler == &IpcExcHandler;
	}

extern "C" TInt X64_PagingExcTrapEnter(TPagingExcTrap* aTrap)
	{
	DThread* t = TheCurrentThread;
	aTrap->iThread = t;
	t->iPagingExcTrap = aTrap;
	return 0;
	}
extern "C" void X64_PagingExcTrapLeave(TPagingExcTrap* aTrap)
	{
	aTrap->iThread->iPagingExcTrap = NULL;
	}
__attribute__((naked)) TInt TPagingExcTrap::Trap()
	{
	__asm__(X64_SAVE_NONVOLATILE "jmp X64_PagingExcTrapEnter\n");
	}
__attribute__((naked)) void TPagingExcTrap::Exception(TInt /*aResult*/)
	{
	__asm__("push rdi\n" "push rsi\n" "sub rsp, 8\n"
			"call X64_PagingExcTrapLeave\n"
			"add rsp, 8\n" "pop rsi\n" "pop rdi\n"
			X64_RESUME_NONVOLATILE);
	}

// Deliver an exception to a user-side handler: push the register record on the USER stack.
// X64_TODO: TX86ExcInfo (include/kernel/x86/x86.h) is the IA-32 register set; the amd64 record
// (r8-r15, 64-bit rip/rsp) + its user-side consumer are authored with the ring-3 exception path.
void PushExcInfoOnUserStack(TX86ExcInfo*, TInt)
	{
	NKFault("X64_TODO PushExcInfoOnUserStack", 0);
	}
void DumpExcInfo(TX86ExcInfo& a)
	{
	Kern::Printf("Exc %d code %08x fault addr %08x", a.iExcId, a.iExcErrorCode, a.iFaultAddress);
	Kern::Printf("eip %08x cs %04x eflags %08x esp3 %08x", a.iEip, a.iCs, a.iEflags, a.iEsp3);
	Kern::Printf("eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x ebp %08x", a.iEax, a.iEbx, a.iEcx, a.iEdx, a.iEsi, a.iEdi, a.iEbp);
	if ((a.iCs & 3) == 3)
		{
		// Poor man's user backtrace (bounded): stack words that point into ROM code. Frames are 64-bit,
		// so look at every qword whose high half is zero.
		TUint64 w[128];										// 1 KB (was 96 bytes: too short to see who called a faulting euser helper)
		TUint32 hit[24];
		TInt found = 0;
		memclr(hit, sizeof(hit));
		const TUint32 from = a.iEsp3 & ~7u;
		TUint32 bytes = 0x1000u - (from & 0xFFFu);			// never past the end of the stack's last page
		if (bytes > sizeof(w))
			bytes = sizeof(w);
		if (!K::USafeRead((const TAny*)from, w, bytes))
			for (TInt i=0; i<(TInt)(bytes>>3) && found<24; ++i)
				if ((w[i]>>32)==0 && (TUint32)w[i]>=KRomLinearBase && (TUint32)w[i]<KRomLinearBase+0x08000000u)  // 128 MB: the ROM passed 32 MB (X4.64)
					hit[found++] = (TUint32)w[i];
		if (found)
			for (TInt r=0; r<48 && r<(TInt)(bytes>>3); r+=8)		// the raw stack too: callers outside the ROM (RAM-loaded code) are not "hits"
				Kern::Printf("user stack raw %08x: %08x %08x %08x %08x %08x %08x %08x %08x", from+r*8,
					(TUint32)w[r], (TUint32)w[r+1], (TUint32)w[r+2], (TUint32)w[r+3], (TUint32)w[r+4], (TUint32)w[r+5], (TUint32)w[r+6], (TUint32)w[r+7]);
		for (TInt r=0; r<24; r+=12)
			Kern::Printf("user stack -> code: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
					hit[r], hit[r+1], hit[r+2], hit[r+3], hit[r+4], hit[r+5], hit[r+6], hit[r+7], hit[r+8], hit[r+9], hit[r+10], hit[r+11]);
		}
	}

/******************************************************************************
 * Thread entry (kernel/x86/ckernel.cia DThread::EpocThreadFunction)
 ******************************************************************************/
// Drop to ring 3: rdi = user rip, rsi = user rsp, edx -> rbx = "not the first thread" flag.
// Selectors are the board GDT's (0x1B = ring-3 code | RPL3, 0x23 = ring-3 data | RPL3);
// RFLAGS = IF only. Everything else reaches user mode zeroed.
extern "C" __attribute__((naked)) void X64_EnterUser(TLinAddr /*aRip*/, TLinAddr /*aRsp*/, TUint /*aNotFirst*/)
	{
	__asm__("mov ebx, edx\n"
			"push 0x23\n" "push rsi\n" "push 0x202\n" "push 0x1B\n" "push rdi\n"
			"xor eax, eax\n" "xor ecx, ecx\n" "xor edx, edx\n" "xor esi, esi\n" "xor edi, edi\n"
			"xor ebp, ebp\n" "xor r8d, r8d\n" "xor r9d, r9d\n" "xor r10d, r10d\n" "xor r11d, r11d\n"
			"xor r12d, r12d\n" "xor r13d, r13d\n" "xor r14d, r14d\n" "xor r15d, r15d\n"
			"iretq\n");
	}

void DThread::EpocThreadFunction(TAny* aPtr)
	{
	SThreadCreateInfo& info = *(SThreadCreateInfo*)aPtr;
	DThread* t = TheCurrentThread;
	if (t->iThreadType != EThreadUser)
		Kern::Exit((*info.iFunction)(info.iPtr));		// kernel thread: never returns

	// User thread: paint the stack, copy the create-info block to its top (the user-side
	// entry point finds it at rsp), then enter the process entry point (first thread) or
	// re-entry point (any other thread).
	t->iUserThreadState = EUserThreadRunning;
	TLinAddr base = t->iUserStackRunAddress;
	TUint size = (TUint)t->iUserStackSize & ~0xFu;
	memset((TAny*)base, 0x29, size);
	TLinAddr sp = (base + size - (TUint)info.iTotalSize) & ~(TLinAddr)0xF;
	memcpy((TAny*)sp, &info, info.iTotalSize);
	DProcess* p = t->iOwningProcess;
	TBool first = (t->iFlags & KThreadFlagOriginal);
	TLinAddr pc = first ? (TLinAddr)p->iCodeSeg->iEntryPtVeneer : (TLinAddr)p->iReentryPoint;
	X64_EnterUser(pc, sp, first ? 0 : 1);
	}

// cutils.cia: IA-32 kernel threads already run in supervisor mode -- a plain call.
TInt A::CallSupervisorFunction(TSupervisorFunction aFunction, TAny* aParameter)
	{
	return (*aFunction)(aParameter);
	}

// cutils.cia: calibrated busy-wait, aInterval in nanoseconds (X86_NanoWaitCal = PICOSECONDS per loop
// pass, measured by the variant: ekern/xk_irq.cpp X64_NanoWaitCalibration).
void K::DoNanoWait(TUint32 aInterval)
	{
	TInt64 n = (TInt64)aInterval * 1000;
	TInt cal = X86_NanoWaitCal > 0 ? X86_NanoWaitCal : 1;
	do	{ __asm__ __volatile__("pause"); n -= cal; } while (n >= 0);
	}

/******************************************************************************
 * Executive tables (kernel/x86/cexec.cia). On x86 these are plain C arrays built
 * from the generated exec_kernel.h, so they port verbatim; entries are 32-bit
 * function addresses, valid under x32.
 ******************************************************************************/
GLREF_C TInt CalcKernelHeapUsed();
void GetLatencyValues(TInt aMode, TInt& aCount, TInt* aDest);
void KernMsgTest();
void InvalidExecHandler();
void InvalidFastExec();
void PreprocessHandler();

#define __GEN_KERNEL_EXEC_CODE__
#include "execs.h"

void InvalidFastExec()
	{
	InvalidExecHandler();
	}

// cexec.cia: IA-32 routed Kern::HalFunction through `int 0x21` "to get the correct permissions
// when calling from supervisor mode". ExecHandler::HalFunction itself checks nothing -- each HAL
// handler asks Kern::CurrentThreadHasCapability, which reads the current thread either way -- so
// the win32 shape (kernel/win32/cexec.cpp) is exact: call it directly. The device number rides in
// the group's top half, as both originals pack it. First caller: ELOCD's dmasupport.cpp (X4.17).
EXPORT_C TInt Kern::HalFunction(TInt aGroup, TInt aFunction, TAny* a1, TAny* a2)
	{
	return ExecHandler::HalFunction(aGroup, aFunction, a1, a2);
	}
EXPORT_C TInt Kern::HalFunction(TInt aGroup, TInt aFunction, TAny* a1, TAny* a2, TInt aDeviceNumber)
	{
	return ExecHandler::HalFunction(aGroup | (aDeviceNumber<<16), aFunction, a1, a2);
	}

// The slow-exec preprocessor turns a handle argument into an object pointer before the
// handler runs (170 lines of register-convention asm on IA-32). X64_TODO: authored together
// with the int 0x80 exec dispatcher, which fixes the argument registers (X4.3). The C++
// reference to adapt is kernel/win32/cexec.cpp Win32PreprocessHandler.
void PreprocessHandler()
	{
	NKFault("X64_TODO PreprocessHandler", 0);
	}

/******************************************************************************
 * C++ runtime: the kernel heap
 ******************************************************************************/
TAny* operator new(TUint aSize) __NO_THROW		{ return Kern::Alloc(aSize); }
TAny* operator new[](TUint aSize) __NO_THROW	{ return Kern::Alloc(aSize); }
void operator delete(TAny* aPtr) __NO_THROW		{ Kern::Free(aPtr); }
void operator delete[](TAny* aPtr) __NO_THROW	{ Kern::Free(aPtr); }
