// xe_nkern.cpp -- Phorepoc amd64/x32: the nanokernel-API tail that only the FULL kernel
// (ekern) references, so the nkernsmp-only image never needed it. amd64 siblings of
// nkernsmp/x86/{ncutils.cpp,ncutils.cia,ncsched.cia,ncutilf.cia,ncthrd.cpp} pieces.
// Surface list: docs/phorepoc/ekern-x32-backend-surface.txt. X64_TODO = honest first cut.

// the SMP NThreadBase field aliases (iCurrent, i_NThread_Initial ...), as nkernsmp/ncthrd.cpp does (X4.64)
#define __INCLUDE_NTHREADBASE_DEFINES__
#include <nk_priv.h>
#include <nk_plat.h>
#include <ncern.h>

extern "C" TSubScheduler& SubScheduler();		// x64s_ncutils.cpp

/******************************************************************************
 * Boot-time nanokernel init (nkernsmp/x86/ncutils.cpp NKern::Init0)
 ******************************************************************************/
extern "C" __attribute__((weak)) void InterruptInit0() {}	// board: X4.2c
void NKern::Init0(TAny* a)
	{
	SVariantInterfaceBlock* v = (SVariantInterfaceBlock*)a;
	TheScheduler.iVIB = v;
	__NK_ASSERT_ALWAYS(v && v->iVer==0 && v->iSize==sizeof(SVariantInterfaceBlock));
	TInt i;
	for (i=0; i<KMaxCpus; ++i)
		{
		TSubScheduler& ss = TheSubSchedulers[i];
		ss.iSSX.iCpuFreqRI.Set(v->iCpuFreqR[i]);
		ss.iSSX.iTimerFreqRI.Set(v->iTimerFreqR[i]);
		ss.iSSX.iTimestampOffset.i64 = 0;
		v->iCpuFreqR[i] = 0;
		v->iTimerFreqR[i] = 0;
		}
	TheScheduler.iSX.iTimerMax = (v->iMaxTimerClock / 128);
	// FPU/SSE is enabled by the board before the kernel starts (kmain: CR0/CR4 + fninit)
	InterruptInit0();
	}

TUint32 NKern::IdleGenerationCount()
	{
	return TheScheduler.iIdleGenerationCount;
	}

EXPORT_C TInt NKern::TimesliceTicks(TUint32 aMicroseconds)
	{
	TUint32 mf32 = (TUint32)TheScheduler.iSX.iTimerMax;
	TUint64 mf(mf32);
	TUint64 ticks = mf*TUint64(aMicroseconds) + UI64LIT(999999);
	ticks /= UI64LIT(1000000);
	if (ticks > TUint64(TInt(KMaxTInt)))
		return KMaxTInt;
	return (TInt)ticks;
	}

// ncsched.cia: is the kernel locked on THIS cpu (exactly aCount times if aCount!=0)?
EXPORT_C TBool NKern::KernelLocked(TInt aCount)
	{
	TInt irq = NKern::DisableAllInterrupts();	// no migration between lookup and read
	TInt c = SubScheduler().iKernLockCount;
	NKern::RestoreInterrupts(irq);
	if (!c)
		return EFalse;
	return (aCount==0 || aCount==c);
	}

// ncutils.cia: stop the other CPUs (crash IPI) and enter the crash handler. X64_TODO: the
// cross-CPU CrashState handshake + the crash debugger hand-off; first cut halts this CPU.
extern "C" char __code_base[], __utext_base[];		// ekern.lds: the kernel's .text
void KPrintf(const char* aFmt, ...);						// xk_entry.cpp
extern "C" void X64_CrashWho(NThreadBase* aN);				// ekern/xk_threaddump.cpp
void NKern::NotifyCrash(const TAny* /*a0*/, TInt /*a1*/)
	{
	// Kern::Fault has already printed the category + reason. (Calling NKFault here would
	// recurse: the kernel's NKFault ends in Kern::Fault -> NotifyCrash.)
	NKern::DisableAllInterrupts();
	// X4.44: who and where -- the current thread and every kernel-text address on its stack
	// (return addresses, plus some stale ones), for `addr2line -e ekern.elf32` / nm. Bounded.
	TUint32* sp;
	__asm__ __volatile__("mov %0, esp" : "=r"(sp));
	// KPrintf (xk_entry.cpp) is lock-free, straight to COM1; Kern::Printf can block in a crash
	KPrintf("CRASH nthread %08x; kernel-text words on its stack:", (TUint32)(TLinAddr)NKern::CurrentThread());
	NThreadBase* t = NKern::CurrentThread();
	X64_CrashWho(t);			// ekern/xk_threaddump.cpp: process::thread + the user rip / stack (X4.64)
	const TLinAddr top = (TLinAddr)t->iStackBase + t->iStackSize;
	TInt shown = 0;
	for (TInt i=0; i<512 && shown<24 && (TLinAddr)(sp + i) < top; ++i)
		{
		const TUint32 w = sp[i];
		if (w >= (TUint32)(TLinAddr)__code_base && w < (TUint32)(TLinAddr)__utext_base)
			KPrintf("  [sp+%03x] %08x", i*4, w), ++shown;
		}
	for (;;) { __asm__ __volatile__("cli; hlt"); }
	}

// nccpu.cpp: start one application processor. X64_TODO(X4.2c): the board already brings every
// AP up (smp.S INIT-SIPI-SIPI trampoline); this becomes the hand-over of aInfo (initial stack,
// SubScheduler) to the AP that kmain_smp parked for it.
__attribute__((weak)) TInt NKern::BootAP(volatile SAPBootInfo*)		// real one: ekern/xk_smp.cpp
	{
	return KErrNotSupported;
	}

// ncthrd.cpp. X64_TODO: needs the amd64 user-mode register frame (SThreadExcStack sibling);
// reports "no registers available" rather than garbage.
EXPORT_C void NKern::ThreadGetUserContext(NThread*, TAny*, TUint32& aAvailRegistersMask)
	{
	aAvailRegistersMask = 0;
	}

// nkernsmp/x86/ncthrd.cpp, the SMP logic (X4.64). The first version here disabled interrupts on THIS cpu
// and stored the new head with a plain write -- but the target may be running on another CPU, where the
// return-to-user path swaps the list to 0 and runs it. Between our read of the head and our store, the
// callbacks it ran got re-linked and ran AGAIN: a client request completed twice ->
// TClientRequest::MakeFree found it free -> KERN 124 (EClientRequestCallbackInWrongState), every
// ~15-20 apps in the regression. Now: a CAS loop on the head, and an IPI when the target runs elsewhere
// so it enters the kernel and sees the callback.
extern "C" void send_resched_ipi(TInt aCpu);				// x64smp/x64s_ncsched.cpp
TInt NKern::QueueUserModeCallback(NThreadBase* aThread, TUserModeCallback* aCallback)
	{
	__e32_memory_barrier();
	if (aCallback->iNext != KUserModeCallbackUnqueued)
		return KErrInUse;
	if (aThread->i_NThread_Initial)
		return KErrArgument;
	TInt result = KErrDied;
	NKern::Lock();
	TUserModeCallback* listHead = aThread->iUserModeCallbacks;
	do	{
		if (TLinAddr(listHead) & 3)
			goto done;	// thread exiting
		aCallback->iNext = listHead;
		} while (!__e32_atomic_cas_ord_ptr(&aThread->iUserModeCallbacks, &listHead, aCallback));
	result = KErrNone;
	if (!listHead)	// the first callback: make a thread running on another CPU notice it (see ncthrd.cpp)
		{
		TInt current = aThread->iCurrent;
		if (current)
			{
			TInt cpu = current & NSchedulable::EReadyCpuMask;
			if (cpu != NKern::CurrentCpu())
				send_resched_ipi(cpu);
			}
		}
done:
	NKern::Unlock();
	return result;
	}

/******************************************************************************
 * Spin locks (ncutilf.cia)
 ******************************************************************************/
EXPORT_C TRWSpinLock::TRWSpinLock(TUint /*aOrder*/) { iLock = 0; }

// Release + immediately re-acquire if someone else is waiting or an IRQ is pending. We
// cannot see waiters in the simple xchg lock, so always flash: correct, merely eager.
EXPORT_C TBool TSpinLock::FlashIrq()
	{
	UnlockIrq();
	LockIrq();
	return ETrue;
	}
EXPORT_C TBool TSpinLock::FlashIrqRestore(TInt aIrq)
	{
	UnlockIrqRestore(aIrq);
	LockIrq();
	return ETrue;
	}

// __e32_atomic_tas_ord32 used to live here: x64smp/x64s_atomics.cpp now carries the whole
// tas/xor family (X4.9b), so the kernel picks it up from the shared backend like every other
// atomic.

// cutils.cia: aRequest ? "are interrupts enabled" : "are interrupts disabled"
TBool InterruptsStatus(TBool aRequest)
	{
	TUint64 f;
	__asm__ __volatile__("pushfq; pop %0" : "=r"(f));
	TBool on = (TBool)((f >> 9) & 1);
	return aRequest ? on : !on;
	}

/******************************************************************************
 * BTrace (ncutils.cpp). Tracing is OFF in this port for now: the Out* family
 * reports "not traced"; the handler/filter registration is real so a trace
 * handler can be installed later without touching callers.
 ******************************************************************************/
EXPORT_C TBool BTrace::OutX(TUint32, TUint32, TUint32, TUint32) { return EFalse; }
EXPORT_C TBool BTrace::OutN(TUint32, TUint32, TUint32, const TAny*, TInt) { return EFalse; }
EXPORT_C TBool BTrace::OutNX(TUint32, TUint32, TUint32, const TAny*, TInt) { return EFalse; }
EXPORT_C TBool BTrace::OutBig(TUint32, TUint32, const TAny*, TInt) { return EFalse; }

static TInt BTraceDefaultControl(BTrace::TControl, TAny*, TAny*) { return KErrNotSupported; }

EXPORT_C void BTrace::SetHandlers(BTrace::THandler aNewHandler, BTrace::TControlFunction aNewControl, BTrace::THandler& aOldHandler, BTrace::TControlFunction& aOldControl)
	{
	BTrace::TControlFunction nc = aNewControl ? aNewControl : &BTraceDefaultControl;
	TInt irq = NKern::DisableAllInterrupts();
	BTrace::THandler oldh = BTraceData.iHandler;
	BTrace::TControlFunction oldc = BTraceData.iControl;
	BTraceData.iHandler = aNewHandler;
	BTraceData.iControl = nc;
	NKern::RestoreInterrupts(irq);
	aOldHandler = oldh;
	aOldControl = oldc;
	}

EXPORT_C TInt BTrace::SetFilter(TUint aCategory, TInt aValue)
	{
	if (!IsSupported(aCategory))
		return KErrNotSupported;
	TUint8* filter = BTraceData.iFilter+aCategory;
	TUint oldValue = *filter;
	if (TUint(aValue)<=1u)
		oldValue = __e32_atomic_swp_ord8(filter, (TUint8)aValue);
	return oldValue;
	}

/******************************************************************************
 * C++ runtime the kernel image needs (nk_entry.cpp on IA-32)
 ******************************************************************************/
extern "C" void __cxa_pure_virtual()
	{
	NKFault("pure virtual", 0);
	}

// kmain_smp.c supplies the PIT-calibrated value on the board; 0 = "not calibrated".
extern "C" __attribute__((weak)) TUint64 ktsc_hz() { return 0; }
