// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

/* sched_test.cpp -- cooperative scheduling on the REAL EKA2 nkern structures,
 * amd64. Links the actual arch-neutral nkern objects (TScheduler, TPriListBase
 * ready lists, First()/Add()/RotateReadyList()) + our amd64 backend (real
 * TScheduler::Reschedule = First() + __phore_switch, real NThread::Create-style
 * stack priming) and runs two real NThreads that cooperatively yield to each
 * other and then return to the main context.
 *
 * Hosted (Linux, in the phorepoc-x86 image) so it RUNS -- cooperative yields need
 * no timer/IDT. This is the amd64 analogue of a Track-B "two threads scheduled"
 * milestone, minus preemption. printf is declared locally (headers are -nostdinc)
 * and resolved by glibc at the final link.
 */
#include <e32def.h>
#include <nkern.h>
#include <nk_priv.h>

extern "C" int printf(const char*, ...);
extern "C" void __phore_thread_trampoline(void);

/* Two real thread stacks. */
static TUint64 stackA[2048];
static TUint64 stackB[2048];

/* The three participants. NThread default-constructs via NThreadBase(). */
static NThread thrMain;   // the initial/booting context (its SP is captured on first switch)
static NThread thrA;
static NThread thrB;

/* Prime a thread the way NThread::Create does, without pulling the full
 * NThreadBase::Create machinery (we set only what the scheduler + switch read). */
static void setup(NThread& t, TUint64* stk, TInt words, TInt pri,
                  void (*entry)(void*), void* arg)
    {
    t.iPriority   = (TUint8)pri;
    t.iStackBase  = (TLinAddr)stk;
    t.iStackSize  = words * (TInt)sizeof(TUint64);
    TLinAddr top  = ((TLinAddr)stk + t.iStackSize) & ~(TLinAddr)0xF;
    TLinAddr sp   = top - 7 * 8;
    if (((sp + 7 * 8) & 0xF) != 0) sp -= 8;
    TLinAddr* f = (TLinAddr*)sp;
    f[0] = 0; f[1] = 0;
    f[2] = (TLinAddr)arg;      // r13 -> arg
    f[3] = (TLinAddr)entry;    // r12 -> entry
    f[4] = 0; f[5] = 0;
    f[6] = (TLinAddr)__phore_thread_trampoline;
    t.iSavedSP = sp;
    }

/* Cooperative yield on the real scheduler: rotate our priority ring so the next
 * ready thread becomes head, then Reschedule (selects it + switches to it). */
static void yield(NThread& self)
    {
    TheScheduler.RotateReadyList(self.iPriority);
    TheScheduler.iRescheduleNeededFlag = 1;
    TScheduler::Reschedule();
    }

/* Leave for good: remove self from the ready list, then Reschedule to whoever
 * is next (eventually the lower-priority main context). */
static void leave(NThread& self)
    {
    TheScheduler.Remove(&self);
    TheScheduler.iRescheduleNeededFlag = 1;
    TScheduler::Reschedule();
    }

static void threadA(void*)
    {
    for (int i = 0; i < 3; i++) { printf("  [A] run %d\n", i); yield(thrA); }
    printf("  [A] leaving\n"); leave(thrA);
    }
static void threadB(void*)
    {
    for (int i = 0; i < 3; i++) { printf("  [B] run %d\n", i); yield(thrB); }
    printf("  [B] leaving\n"); leave(thrB);
    }

int main(void)
    {
    printf("== amd64 cooperative scheduling on the real EKA2 nkern ==\n");

    // main context: lower priority so A/B run first; stays current until we switch.
    thrMain.iPriority = 5;
    TheScheduler.iCurrentThread = &thrMain;
    TheScheduler.Add(&thrMain);

    setup(thrA, stackA, 2048, 10, threadA, 0);
    setup(thrB, stackB, 2048, 10, threadB, 0);
    TheScheduler.Add(&thrA);
    TheScheduler.Add(&thrB);

    // Kick off scheduling: pick the highest-priority ready thread and run it.
    TheScheduler.iRescheduleNeededFlag = 1;
    TScheduler::Reschedule();       // switches to A; returns here once A & B have left

    printf("== back in main; current thread pri=%d ==\n",
           (int)TheScheduler.iCurrentThread->iPriority);
    bool ok = (TheScheduler.iCurrentThread == &thrMain);
    printf("%s\n", ok ? "SCHED OK" : "SCHED FAIL");
    return ok ? 0 : 1;
    }
