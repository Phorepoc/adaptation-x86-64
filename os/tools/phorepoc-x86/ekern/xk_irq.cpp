// Portions Copyright (c) Nokia Corporation and/or its subsidiary(-ies)
// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// xk_irq.cpp -- Phorepoc amd64/x32 kernel image: interrupt entry/exit and the system tick.
// amd64 sibling of the interrupt half of nkernsmp/x86/vectors.cia + the IA-32 variant's timer:
//   - X64_Trap(frame): every IDT vector lands here (xk_vectors.S). CPU exceptions -> the early-trap
//     reporter for now (X64_TODO: Exc::Dispatch); interrupts are counted/nested the way the
//     nanokernel expects (TSubSchedulerX::iIrqNestCount / iIrqCount), dispatched, and on the way
//     OUT of the outermost interrupt a needed reschedule is taken if the kernel is unlocked --
//     that is what makes the kernel preemptive.
//   - the tick: this CPU's local-APIC timer, PIT-calibrated to MsTickPeriod (1 ms), drives the real
//     NTimerQ::Tick() and ages the running thread's timeslice (iTime is in these ticks:
//     NKern::TimesliceTicks with iTimerMax = 1000/s, see xk_entry.cpp TheVib).

#include <x86_mem.h>
#include "xk_interrupt.h"

extern "C" TSubScheduler& SubScheduler();					// x64s_ncutils.cpp
extern "C" void generic_ipi_isr(TSubScheduler* aS);			// nkernsmp/nkern.cpp
extern "C" void run_event_handlers(TSubScheduler* aS);		// nkernsmp/nk_irq.cpp

struct SX64TrapFrame;
extern "C" void X64_EarlyTrap(SX64TrapFrame* aFrame);		// xk_entry.cpp (does not return)

struct SX64TrapFrame
	{
	TUint64 iR15, iR14, iR13, iR12, iR11, iR10, iR9, iR8, iRdi, iRsi, iRbp, iRbx, iRdx, iRcx, iRax;
	TUint64 iVector, iError, iRip, iCs, iRflags, iRsp, iSs;
	};

const TUint KVecTimer		= 0x40;		// local-APIC timer (system tick + timeslice)
const TUint KVecSpurious	= 0xFF;		// local-APIC spurious: no EOI

static inline TUint32 LapicRd(TUint aReg)				{ return *(volatile TUint32*)(0xFEE00000u+aReg); }
static inline void LapicWr(TUint aReg, TUint32 aVal)	{ *(volatile TUint32*)(0xFEE00000u+aReg) = aVal; }
static inline void outb(TUint16 aPort, TUint8 aVal)
	{ __asm__ __volatile__("out dx, al" : : "d"(aPort), "a"(aVal)); }
static inline TUint8 inb(TUint16 aPort)
	{ TUint8 v; __asm__ __volatile__("in al, dx" : "=a"(v) : "d"(aPort)); return v; }

volatile TUint32 X64_TickCount;			// diagnostics: system ticks taken on the boot CPU
static volatile TBool TimerQReady;		// set by the variant's Init3: NTimerQ may be ticked from now on
volatile TUint32 X64_IpiCount[2];		// diagnostics: reschedule / generic IPIs TAKEN (all CPUs)
TBool X64_DebugNmi = XK_DEBUG_NMI;		// make_ekern.sh NMI=1: the SMP watchdog NMIs a stuck cpu

// SMP watchdog (debug aid, bounded: fires once per victim). Every CPU counts its own ticks and, from
// its tick, watches its neighbour's counter: a CPU whose tick has not moved for 3 s is stuck with
// interrupts off (a spin lock that never comes free, a lost IPI handshake...). Say what is known and,
// when built NMI=1 (make_ekern.sh), NMI it so the early-trap reporter prints WHERE -- that halts the
// machine, so it is opt-in.
static volatile TUint32 CpuTicks[KMaxCpus];
static void SmpWatchdog(TSubScheduler& aSS)
	{
	static TUint32 lastSeen[KMaxCpus], stalled[KMaxCpus], fired[KMaxCpus];
	const TInt me = aSS.iCpuNum;
	++CpuTicks[me];
	const TInt ncpus = TheScheduler.iNumCpus;
	if (ncpus < 2 || !TimerQReady)
		return;
	const TInt v = (me+1) % ncpus;				// my neighbour
	if (CpuTicks[v] != lastSeen[me])
		{ lastSeen[me] = CpuTicks[v]; stalled[me] = 0; return; }
	if (fired[v] || ++stalled[me] < 3000)
		return;
	fired[v] = 1;
	TSubScheduler& s = TheSubSchedulers[v];
	Kern::Printf("[smp] WATCHDOG: cpu %d tick stuck at %d; kernlock %d irqnest %d resched %d dfc %d thread %08x IPIs taken: resched %d generic %d",
					v, CpuTicks[v], s.iKernLockCount, s.iSSX.iIrqNestCount, s.iRescheduleNeededFlag, s.iDfcPendingFlag, s.iCurrentThread,
					X64_IpiCount[0], X64_IpiCount[1]);
	if (X64_DebugNmi)
		{
		LapicWr(0x310, s.iSSX.iAPICID<<24);
		LapicWr(0x300, 0x00004400);				// NMI
		}
	}

static void SystemTick(TSubScheduler& aSS)
	{
	if (aSS.iCpuNum == 0)
		{
		++X64_TickCount;
		// bounded heartbeat so the tick is observable on the serial log: once a second for 5 s, then
		// every 10 s up to 60 lines -- "current nanothread" says busy vs idle on a long boot (X4.27)
		static TUint32 shown = 0, next = 1000;
		if (shown < 65 && X64_TickCount == next)
			{
			++shown;
			next += shown < 5 ? 1000 : 10000;
			Kern::Printf("[ekern] heartbeat: %d system ticks, NKern::TickCount()=%d, current nanothread %08x",
							X64_TickCount, NKern::TickCount(), aSS.iCurrentThread);
			}
		}
	SmpWatchdog(aSS);
	if (aSS.iCpuNum == 0 && TimerQReady)
		TheTimerQ.Tick();					// the nanokernel millisecond tick (may queue the timer DFC)
	NThreadBase* t = aSS.iCurrentThread;
	if (t && t->iTime > 0 && --t->iTime == 0)
		aSS.iRescheduleNeededFlag = TRUE;	// timeslice used up: round-robin at this priority
	}

// CPU exception -> the kernel's own handler, Exc::Dispatch (kernel/x86/ckernel.cpp): magic handler,
// the thread's TExcTrap (XTRAP -- longjmps out of here and never returns), user-side exception
// delivery, or panic the thread / fault the system. The register record is the IA-32 TX86ExcInfo
// (include/kernel/x86/x86.h): under x32 every ADDRESS fits its 32-bit fields; general registers are
// passed as their low halves. X64_TODO: a 64-bit record for the debugger / user-side handlers.
// Anything that cannot be a thread-context fault (boot, inside an ISR, kernel locked, NMI/#DF/#MC)
// stays with the loud early-trap reporter.
static void HandleException(SX64TrapFrame* aFrame)
	{
	const TUint vec = (TUint)aFrame->iVector;
	TSubScheduler& ss = SubScheduler();
	if (vec==2 || vec==8 || vec==18 || !K::TheNullThread || !ss.iCurrentThread
			|| ss.iSSX.iIrqNestCount >= 0 || ss.iKernLockCount != 0)
		X64_EarlyTrap(aFrame);

	TUint64 cr2 = 0;
	if (vec == 14)
		__asm__ __volatile__("mov %0, cr2" : "=r"(cr2));
	TX86ExcInfo info;
	memclr(&info, sizeof(info));
	info.iEsp = (TUint32)(TLinAddr)(aFrame+1);  info.iSs = 0x10;
	info.iFaultAddress = (TUint32)cr2;
	info.iEax = (TUint32)aFrame->iRax;  info.iEcx = (TUint32)aFrame->iRcx;  info.iEdx = (TUint32)aFrame->iRdx;
	info.iEbx = (TUint32)aFrame->iRbx;  info.iEsi = (TUint32)aFrame->iRsi;  info.iEdi = (TUint32)aFrame->iRdi;
	info.iEbp = (TUint32)aFrame->iRbp;
	info.iExcId = (TInt)vec;  info.iExcErrorCode = (TInt)aFrame->iError;
	info.iEip = (TUint32)aFrame->iRip;  info.iCs = (TUint32)aFrame->iCs;  info.iEflags = (TUint32)aFrame->iRflags;
	info.iEsp3 = (TUint32)aFrame->iRsp;  info.iSs3 = (TUint32)aFrame->iSs;

	if (aFrame->iRflags & 0x200)
		__asm__ __volatile__("sti");		// the handler runs as the thread did
	Exc::Dispatch(&info, NULL);
	__asm__ __volatile__("cli");

	// handled: resume, honouring what the handler changed (user-side exception delivery redirects eip)
	aFrame->iRip = info.iEip;
	aFrame->iRbx = (aFrame->iRbx & ~(TUint64)0xFFFFFFFFu) | info.iEbx;
	if (aFrame->iCs & 3)
		aFrame->iRsp = info.iEsp3;
	}

// A user-supplied pointer reached outside the user area (xe_kernel.cpp UserCheck). IA-32 got this
// for free: the access went through the ring-3 data segment and the CPU raised #GP, which took the
// normal exception path. Long mode has no segment limits, so raise that #GP by hand: the thread's
// TExcTrap/TIpcExcTrap fires if one is set (XTRAP around a umemget), else the thread is panicked
// KERN-EXEC 3 -- never the whole system. Does not return to the faulting copy.
extern "C" void X64_UserRangeFault(TLinAddr aAddr)
	{
	TX86ExcInfo info;
	memclr(&info, sizeof(info));
	info.iExcId = 13;									// #GP
	info.iFaultAddress = (TUint32)aAddr;
	info.iEip = (TUint32)(TLinAddr)__builtin_return_address(0);
	info.iCs = 0x08;
	info.iSs = 0x10;
	Exc::Dispatch(&info, NULL);
	NKFault("X64_UserRangeFault: exception path returned", (TInt)aAddr);
	}

/******************************************************************************
 * The executive gate: int 0x20 = fast exec, int 0x21 = slow exec -- EKA2's own IA-32 vectors and
 * register convention (nkernsmp/x86/vectors.cia, include/u32exec.h): call number in EAX,
 * arguments in ECX, EDX, EBX, ESI; result back in EAX; everything else preserved. The IA-32
 * handler was asm because the pushed registers WERE the cdecl argument list; under SysV the
 * handlers take register arguments, so the dispatch is plain C++ over the saved frame.
 ******************************************************************************/
typedef TInt (*TX64ExecFn)(TInt, TInt, TInt, TInt);

// "Previous mode" (IA-32 kept the caller's data selector in GS): a user thread is only ever in
// the kernel through a ring-3 -> ring-0 gate, whose iret frame sits at the very top of its
// supervisor stack (TSS.RSP0). Before its first drop to ring 3 that slot still holds the
// thread-creation frame (ring-0 CS). X64_TODO: a kernel-mode fault NESTED inside an exec still
// reads as "user" here (IA-32 reloaded GS on every entry).
extern "C" TBool X64_CallerIsUser()
	{
	NThreadBase* nt = NKern::CurrentThread();		// migration-safe (x64s_ncsched.cpp NCurrentThreadL)
	if (!nt || !K::TheNullThread || TheCurrentThread->iThreadType != EThreadUser)
		return EFalse;
	TLinAddr top = ((TLinAddr)nt->iStackBase + (TLinAddr)nt->iStackSize) & ~(TLinAddr)0xF;
	return (((const TUint64*)top)[-4] & 3) == 3;		// ss, rsp, rflags, [cs], rip
	}

// vectors.cia run_user_mode_callbacks: enter/leave interrupts off, kernel unlocked, not in a CS.
static void RunUserModeCallbacks(NThreadBase* aT)
	{
	for (;;)
		{
		__asm__ __volatile__("sti");
		aT->iCsCount = 1;													// EnterCS
		TUserModeCallback* cb = (TUserModeCallback*)__e32_atomic_swp_ord_ptr(&aT->iUserModeCallbacks, 0);
		while (cb)
			{
			TUserModeCallback* next = (TUserModeCallback*)__e32_atomic_swp_ord_ptr(&cb->iNext, KUserModeCallbackUnqueued);
			(*cb->iFunc)(cb, EUserModeCallbackRun);
			cb = next;
			}
		__asm__ __volatile__("cli");
		if (aT->iCsFunction)
			{
			__asm__ __volatile__("sti");
			NKern::ThreadLeaveCS();
			__asm__ __volatile__("cli");
			}
		else
			aT->iCsCount = 0;
		if (!aT->iUserModeCallbacks)
			return;
		}
	}

// Common tail of every return to ring 3 (interrupts off here).
static inline void ReturnToUserCheck(SX64TrapFrame* aFrame)
	{
	if ((aFrame->iCs & 3) != 3)
		return;
	NThreadBase* t = SubScheduler().iCurrentThread;
	if ((TLinAddr)t->iUserModeCallbacks >= 4)
		RunUserModeCallbacks(t);
	}

// kernel/win32/cexec.cpp Win32PreprocessHandler (the IA-32 one is the same logic in asm): turn the
// handle in argument 0 into the object it names; panics the thread if it is bad. System locked.
static void Preprocess(TInt* aArgs, TUint32 aFlags)
	{
	DThread* t = TheCurrentThread;
	aFlags &= 0x3f;
	if (aFlags >= EIpcMessageD)
		{
		RMessageK* m = RMessageK::MessageK(aArgs[0], t);
		if (m == NULL || (aFlags != EIpcMessageD && m->iFunction == RMessage2::EDisConnect))
			K::PanicCurrentThread(EBadMessageHandle);
		if (aFlags == EIpcClient)
			aArgs[0] = TInt(m->iClient);
		return;
		}
	DObject* obj = Kern::ObjectFromHandle(t, aArgs[0], TObjectType(TInt(aFlags)-1));
	if (obj)
		aArgs[0] = TInt(obj);
	else
		K::PanicCurrentThread(EBadHandle);
	}

volatile TUint32 X64_ExecCount[2];			// diagnostics: fast / slow execs taken

static void FastExec(SX64TrapFrame* aFrame)
	{
	++X64_ExecCount[0];
	NThreadBase* t = SubScheduler().iCurrentThread;
	const TUint n = (TUint32)aFrame->iRax;
	if (n == 0)
		{
		__asm__ __volatile__("sti");
		NKern::WaitForAnyRequest();
		__asm__ __volatile__("cli");
		return;
		}
	const SFastExecTable* tbl = t->iFastExecTable;
	if (n >= (TUint)tbl->iFastExecCount)
		{
		__asm__ __volatile__("sti");
		(*(void (*)())((const TLinAddr*)t->iSlowExecTable)[-2])();		// invalid exec handler
		__asm__ __volatile__("cli");
		return;
		}
	// fast execs run with interrupts off; entry n lives at word n of the table (word 0 = count)
	TX64ExecFn f = (TX64ExecFn)((const TLinAddr*)tbl)[n];
	aFrame->iRax = (TUint32)(*f)((TInt)aFrame->iRcx, 0, 0, 0);
	}

static void SlowExec(SX64TrapFrame* aFrame)
	{
	++X64_ExecCount[1];
	NThreadBase* t = SubScheduler().iCurrentThread;
	__asm__ __volatile__("sti");
	const SSlowExecEntry* tbl = t->iSlowExecTable;				// [-3] count, [-2] invalid, [-1] preprocess
	const TUint n = (TUint32)aFrame->iRax;
	if (n >= (TUint)((const TInt*)tbl)[-3])
		{
		(*(void (*)())((const TLinAddr*)tbl)[-2])();
		__asm__ __volatile__("cli");
		return;
		}
	const TUint32 flags = tbl[n].iFlags;
	TInt a[4] = { (TInt)aFrame->iRcx, (TInt)aFrame->iRdx, (TInt)aFrame->iRbx, (TInt)aFrame->iRsi };
	TInt extra[8];
	if (flags & 0x1c000000u)
		{
		// argument 2 points at a block of further arguments: copy it kernel side, pass the copy
		TInt count = (TInt)((flags>>26)&7) + 1;
		if (a[2])
			umemget32(extra, (const TAny*)a[2], count*sizeof(TInt));
		else
			memclr(extra, sizeof(extra));
		a[2] = (TInt)extra;
		}
	if (flags & 0x80000000u)									// EClaim
		NKern::LockSystem();
	if (flags & 0x20000000u)									// EPreprocess
		Preprocess(a, flags);
	TInt r = (*(TX64ExecFn)tbl[n].iFunction)(a[0], a[1], a[2], a[3]);
	aFrame->iRax = (TUint32)r;
	if (flags & 0x40000000u)									// ERelease
		NKern::UnlockSystem();
	__asm__ __volatile__("cli");
	}

const TUint KVecFastExec = 0x20, KVecSlowExec = 0x21;

extern "C" void X64_Trap(SX64TrapFrame* aFrame)
	{
	const TUint vec = (TUint)aFrame->iVector;
	if (vec < 32)
		{
		HandleException(aFrame);
		ReturnToUserCheck(aFrame);
		return;
		}
	if (vec == KVecFastExec || vec == KVecSlowExec)
		{
		if (vec == KVecFastExec)
			FastExec(aFrame);
		else
			SlowExec(aFrame);
		ReturnToUserCheck(aFrame);
		return;
		}

	TSubScheduler& ss = SubScheduler();
	++ss.iSSX.iIrqNestCount;				// -1 = not in an ISR
	++ss.iSSX.iIrqCount;

	if (vec == KVecTimer)
		{
		LapicWr(0xB0, 0);					// EOI first: a reschedule below may not return for a long time
		SystemTick(ss);
		}
	else if (vec == RESCHED_IPI_VECTOR)
		{
		LapicWr(0xB0, 0);
		ss.iRescheduleNeededFlag = TRUE;	// another CPU made a thread ready for us: the exit path below acts on it
		++X64_IpiCount[0];
		}
	else if (vec == GENERIC_IPI_VECTOR)
		{
		LapicWr(0xB0, 0);
		generic_ipi_isr(&ss);				// nkernsmp/nkern.cpp: run the TGenericIPI queue for this CPU
		++X64_IpiCount[1];
		}
	else if (vec >= KX64VecIrqBase && vec < KX64VecIrqBase + KX64MaxGsi)
		NKern::Interrupt(vec - KX64VecIrqBase);	// IOAPIC pin: NIrq::HwIsr runs the handlers and EOIs (X4.21)
	else if (vec != KVecSpurious && vec != SPURIOUS_INTERRUPT_VECTOR)
		LapicWr(0xB0, 0);					// TRANSFERRED_IRQ_VECTOR etc.: the tail below runs the events

	if (--ss.iSSX.iIrqNestCount >= 0)
		return;								// nested: the outermost exit does the rescheduling

	if (ss.iEventHandlersPending)			// deferred / transferred tied events (nk_irq.cpp)
		{
		++ss.iSSX.iIrqNestCount;
		run_event_handlers(&ss);
		--ss.iSSX.iIrqNestCount;
		}

	// Leaving the outermost interrupt. Preempt if something asked for it and nobody holds the
	// kernel lock; otherwise the flag stays set and NKern::Unlock() takes it later.
	if ((ss.iRescheduleNeededFlag || ss.iDfcPendingFlag || ss.iExIDfcPendingFlag) && ss.iKernLockCount == 0)
		{
		ss.iKernLockCount = 1;
		TScheduler::Reschedule();			// returns (perhaps much later) unlocked, interrupts off
		// vectors.cia irq postamble: IDFCs run inside Reschedule may have readied threads whose home is
		// ANOTHER cpu (sched.cpp ReadyT -> iReschedIPIs |= mask). Kick those cpus now -- without this a
		// timer expiring on cpu 0 never wakes a sleeper that lives on cpu 1.
		TSubScheduler& now = SubScheduler();	// (we may have migrated while switched out)
		TUint32 ipis = __e32_atomic_swp_ord32(&now.iReschedIPIs, 0);
		if (ipis)
			send_resched_ipis(ipis);
		}
	ReturnToUserCheck(aFrame);
	}

static inline TUint64 Rdtsc()
	{ TUint32 lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)); return (TUint64)hi<<32 | lo; }
TUint32 X64_TscPerMs;					// TSC cycles per ms, measured with the local-APIC calibration

// Count local-APIC timer decrements over a 10 ms PIT channel-2 one-shot.
static TUint32 CalibrateLapicTimerPerMs()
	{
	LapicWr(0x3E0, 0x3);					// divide by 16
	outb(0x61, (TUint8)((inb(0x61) & ~0x02) | 0x01));	// gate on, speaker off
	outb(0x43, 0xB0);						// channel 2, lo/hi, mode 0 (one-shot)
	const TUint KPit10ms = 11932;			// 1193182 Hz / 100
	outb(0x42, (TUint8)KPit10ms);  outb(0x42, (TUint8)(KPit10ms>>8));
	LapicWr(0x380, 0xFFFFFFFFu);
	TUint64 tsc0 = Rdtsc();
	while (!(inb(0x61) & 0x20)) {}			// OUT2 goes high when the count expires
	TUint32 elapsed = 0xFFFFFFFFu - LapicRd(0x390);
	X64_TscPerMs = (TUint32)((Rdtsc() - tsc0) / 10);	// the same 10 ms, for the TSC (X4.23)
	LapicWr(0x380, 0);
	return elapsed / 10;
	}

// An application processor's own tick (variant Init2AP): same calibration as the boot CPU -- the
// local APICs of one package count at the same rate. It only ages timeslices (SystemTick).
// The APs come up in A::Init2 -- BEFORE the boot CPU's Init3 starts its own tick -- so the calibration
// is done on first demand (NKern::BootAP asks before it starts an AP; it polls the PIT, no interrupts).
static TUint32 LapicCountsPerMs;
extern "C" TUint32 X64_LapicCountsPerMs()
	{
	if (!LapicCountsPerMs)
		{
		LapicWr(0xF0, 0x100 | KVecSpurious);	// software-enable the local APIC
		TUint32 perMs = CalibrateLapicTimerPerMs();
		LapicCountsPerMs = perMs ? perMs : 1;
		}
	return LapicCountsPerMs;
	}
// Asic::NanoWaitCalibration for K::DoNanoWait (xe_kernel.cpp: n = ns*1000; n -= cal per `pause` pass),
// i.e. PICOSECONDS per loop pass. Measured against the calibrated local-APIC counter -- a guessed
// constant made every Kern::NanoWait 10-100x too long (the 10 ms INIT delay of NKern::BootAP took
// seconds under QEMU). Interrupts are off when the variant asks (A::Init1).
extern "C" TUint32 X64_NanoWaitCalibration()
	{
	const TUint32 perMs = X64_LapicCountsPerMs();
	const TUint32 KPasses = 200000;
	LapicWr(0x3E0, 0x3);
	LapicWr(0x320, 0x10000 | KVecTimer);	// masked one-shot: just a down-counter
	LapicWr(0x380, 0xFFFFFFFFu);
	TInt64 n = KPasses;
	do	{ __asm__ __volatile__("pause"); n -= 1; } while (n >= 0);		// same shape as K::DoNanoWait
	TUint32 elapsed = 0xFFFFFFFFu - LapicRd(0x390);
	LapicWr(0x380, 0);
	TUint64 ps = (TUint64)elapsed * 1000000000ull / perMs / KPasses;	// counts -> ms -> ps, per pass
	if (ps < 1)
		ps = 1;
	Kern::Printf("[ekern] nano-wait calibration: %d ps per pass", (TUint32)ps);
	// X4.23: what "which CPU am I" costs -- a local-APIC ID read (MMIO: emulated under a hypervisor that
	// does not virtualise the APIC) vs STR (the per-CPU TSS selector, a register read)
	{
	TUint64 t0 = Rdtsc();
	TUint32 acc = 0;
	for (TInt i=0; i<1000; ++i)
		acc += LapicRd(0x20);
	TUint64 t1 = Rdtsc();
	for (TInt i=0; i<1000; ++i)
		{ TUint16 sel; __asm__ __volatile__("str %0" : "=r"(sel)); acc += sel; }
	TUint64 t2 = Rdtsc();
	Kern::Printf("[ekern] cost per 'which cpu': local-APIC ID read %d cycles, STR %d cycles (%d)",
					(TInt)((t1-t0)/1000), (TInt)((t2-t1)/1000), acc & 1);
	}
	return (TUint32)ps;
	}

extern "C" void X64_StartApTick()
	{
	LapicWr(0x3E0, 0x3);					// divide by 16
	LapicWr(0x320, KVecTimer | (1u<<17));
	LapicWr(0x380, LapicCountsPerMs);		// BootAP made sure this is calibrated
	}

// The boot CPU's timer hardware: from the variant's Init1, which always runs on the boot CPU with
// interrupts still off. (Init3 cannot do it: under SMP the supervisor thread may by then be running
// on another CPU, whose local APIC it would program instead.)
extern "C" void X64_StartBootCpuTick()
	{
	TUint32 perMs = X64_LapicCountsPerMs();
	Kern::Printf("[ekern] local-APIC timer: %d counts/ms (div 16) -> 1 kHz periodic, vector 0x%02x", perMs, KVecTimer);
	LapicWr(0x3E0, 0x3);
	LapicWr(0x320, KVecTimer | (1u<<17));	// LVT timer: periodic
	LapicWr(0x380, perMs);
	}

// Called by the variant's Init3 (supervisor thread, any CPU): NTimerQ is initialised by now, so the
// boot CPU's tick may start driving it.
extern "C" void X64_StartSystemTick()
	{
	TimerQReady = ETrue;
	Kern::Printf("[ekern] NTimerQ tick on (boot-cpu tick %d, cpu %d)", X64_TickCount, NKern::CurrentCpu());
	}
