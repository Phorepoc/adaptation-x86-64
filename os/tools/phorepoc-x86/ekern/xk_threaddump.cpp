// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// xk_threaddump.cpp -- Path C debug aid: a one-shot dump of every thread, N seconds after boot (X4.27).
//
// make_ekern.sh DUMP=<seconds>. For the "everything is waiting and nothing says why" kind of hang: per
// thread its name, DThread::iMState, the nanokernel wait state (flags / object type / object) and, for a
// thread that is in the kernel from user mode (i.e. blocked in an exec call -- WaitForRequest, a
// synchronous IPC...), the user rip + the ROM code addresses found on its user stack. That line has the
// "eip X ... code: a b c" shape tools/phorepoc-x86/romsym_x32.py --log symbolizes, so the offline step is
//     py tools/phorepoc-x86/romsym_x32.py --dump .phorepoc-x86-build/ekern/run.log
// Bounded: one dump, 12 stack words per thread. Compiled -fno-access-control (the wait state is private).
#include <kernel/kern_priv.h>

#ifndef XK_DUMP_AT
#define XK_DUMP_AT 0
#endif


static void DumpThread(DThread* aT)
	{
	NThreadBase& n = aT->iNThread;
	const NThreadWaitState::S& w = n.iWaitState.iWtC;
	Kern::Printf("[dump] %O  waitobj %08x  nk wait flags %02x type %d obj %08x  nt %08x rdy %02x cur %02x"
					" last %d held %08x cs %d/%d pri %d", aT,
					(TUint32)(TLinAddr)aT->iWaitObj, w.iWtStFlags, w.iWtObjType, (TUint32)(TLinAddr)w.iWtObj,
					(TUint32)(TLinAddr)&n, n.iSpare1, n.iSpare2, n.iSpare3,
					(TUint32)(TLinAddr)n.iHeldFastMutex, n.iCsCount, n.iCsFunction, n.iPriority);
	// a dead thread's supervisor stack may already be freed: reading its iret frame faulted the kernel
	if (aT->iThreadType != EThreadUser || !n.iStackBase || (w.iWtStFlags & NThreadWaitState::EWtStDead))
		return;
	const TUint64* top = (const TUint64*)(((TLinAddr)n.iStackBase + (TLinAddr)n.iStackSize) & ~(TLinAddr)0xF);
	const TUint64 rip = top[-5], cs = top[-4], rsp = top[-2];			// the ring-3 -> ring-0 iret frame
	if ((cs & 3) != 3)
		return;
	TUint64 st[64];
	TUint32 hit[12];
	TInt found = 0;
	memclr(hit, sizeof(hit));
	if (Kern::ThreadRawRead(aT, (const TAny*)(TLinAddr)(rsp & ~7ull), st, sizeof(st)) == KErrNone)
		for (TInt i=0; i<64 && found<12; ++i)
			if ((st[i]>>32)==0 && (TUint32)st[i]>=0x80400000u && (TUint32)st[i]<0x88000000u)	// ROM past 32 MB (X4.64)
				hit[found++] = (TUint32)st[i];
	Kern::Printf("[dump]   eip %08x rsp %08x code: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
					(TUint32)rip, (TUint32)rsp, hit[0], hit[1], hit[2], hit[3], hit[4], hit[5], hit[6], hit[7],
					hit[8], hit[9], hit[10], hit[11]);
	}

static void Dump()
	{
	// NOT under the container's lock: a hang may be someone holding exactly that (the first version
	// blocked in c.Wait() and printed nothing). Read-only walk, debug only.
	DObjectCon& c = *K::Containers[EThread];
	DMutex* m = c.Lock();
	Kern::Printf("[dump] ===== %d threads at tick %d; thread container mutex held %d by %O, %d waiting =====",
					c.Count(), NKern::TickCount(), m->iHoldCount, m->iCleanup.iThread, m->iWaitCount);
	NFastMutex& sl = TheScheduler.iLock;
	Kern::Printf("[dump] system lock %08x holder %08x", (TUint32)(TLinAddr)&sl, (TUint32)(TLinAddr)sl.iHoldingThread);
	for (TInt cpu=0; cpu<NKern::NumberOfCpus(); ++cpu)
		{
		TSubScheduler& ss = TheSubSchedulers[cpu];
		Kern::Printf("[dump] cpu%d current %08x kernlock %d resched %d dfcpend %d", cpu,
						(TUint32)(TLinAddr)ss.iCurrentThread, ss.iKernLockCount, ss.iRescheduleNeededFlag,
						ss.iDfcPendingFlag);
		}
	NTimerQ& tq = TheTimerQ;
	Kern::Printf("[dump] timerq ms %d present %08x holding %d ordered %d completed %d dfc state %04x",
					tq.iMsCount, tq.iPresent, !tq.iHoldingQ.IsEmpty(), !tq.iOrderedQ.IsEmpty(),
					!tq.iCompletedQ.IsEmpty(), tq.iDfc.i8816.iHState16);
	for (TInt i=0; i<c.Count(); ++i)
		DumpThread((DThread*)c[i]);
	Kern::Printf("[dump] ===== end =====");
	}

// A supervisor thread that sleeps, then dumps. (First versions -- an ISR-callback NTimer queueing a DFC,
// then one long NKern::Sleep -- never fired once the boot had hung: long timers stopped expiring. X4.27.)
static TInt DumpThreadFn(TAny*)
	{
	// 20 ms steps: a timer under 32 ticks expires straight from the tick ISR (nk_timer.cpp), so this
	// fires even if the timer queue's DFC thread is stuck -- which is what a long sleep here showed
	for (TInt i=0; i<XK_DUMP_AT*50; ++i)
		NKern::Sleep(20);
	Kern::Printf("[dump] awake at tick %d", NKern::TickCount());
	Dump();
	return KErrNone;
	}

void X64ArmThreadDump()
	{
	if (XK_DUMP_AT <= 0)
		return;
	_LIT(KDumpName, "X64Dump");
	SThreadCreateInfo info;
	info.iType = EThreadSupervisor;
	info.iFunction = DumpThreadFn;
	info.iPtr = NULL;
	info.iSupervisorStack = NULL;
	info.iSupervisorStackSize = 0;
	info.iInitialThreadPriority = 27;
	info.iName.Set(KDumpName);
	info.iTotalSize = sizeof(info);
	TInt r = Kern::ThreadCreate(info);
	if (r == KErrNone)
		Kern::ThreadResume(*(DThread*)info.iHandle);
	Kern::Printf("[dump] thread dump in %d s -> %d", XK_DUMP_AT, r);
	}

// X4.64: who crashed. NKern::NotifyCrash (port/x64ekern/xe_nkern.cpp) knows only the nanothread; a
// kernel fault in a user thread's context (KERN 124: a client request's user-mode callback run on
// return to user mode) needs the process::thread name and where in user code it was. Called with
// interrupts off; KPrintf is the lock-free COM1 writer, the user stack is the CURRENT thread's
// (USafeRead is exception-safe). Bounded.
void KPrintf(const char* aFmt, ...);
extern "C" void X64_CrashWho(NThreadBase* aN)
	{
	if (!aN)
		return;
	DThread* t = _LOFF(aN, DThread, iNThread);
	TBuf8<KMaxFullName> name;
	t->FullName(name);
	KPrintf("CRASH thread %S type %d", &name, t->iThreadType);
	if (t->iThreadType != EThreadUser || !aN->iStackBase)
		return;
	const TUint64* top = (const TUint64*)(((TLinAddr)aN->iStackBase + (TLinAddr)aN->iStackSize) & ~(TLinAddr)0xF);
	const TUint64 rip = top[-5], cs = top[-4], rsp = top[-2];			// the ring-3 -> ring-0 iret frame
	if ((cs & 3) != 3)
		return;
	TUint64 st[48];
	TUint32 hit[12];
	TInt found = 0;
	memclr(hit, sizeof(hit));
	if (!K::USafeRead((const TAny*)(TLinAddr)(rsp & ~7ull), st, sizeof(st)))
		for (TInt i=0; i<48 && found<12; ++i)
			if ((st[i]>>32)==0 && (TUint32)st[i]>=0x80400000u && (TUint32)st[i]<0x88000000u)
				hit[found++] = (TUint32)st[i];
	KPrintf("CRASH user eip %08x rsp %08x code: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
			(TUint32)rip, (TUint32)rsp, hit[0], hit[1], hit[2], hit[3], hit[4], hit[5], hit[6], hit[7],
			hit[8], hit[9], hit[10], hit[11]);
	}
