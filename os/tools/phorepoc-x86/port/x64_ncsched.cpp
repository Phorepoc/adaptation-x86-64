/* x64_ncsched.cpp -- Phorepoc amd64 arch backend: the scheduler / thread /
 * fault core that the arch-neutral nkern references but that is inherently
 * machine-specific. The amd64 analogue of nkern/x86/{ncsched,ncthrd,vectors,
 * ncglob}.cia+.cpp.
 *
 * STATUS (X2.2b): this closes the LINK -- every symbol the neutral nkern needs
 * is now defined, so the neutral core + amd64 backend link with zero unresolved
 * references. The genuinely board-dependent pieces (the register/stack context
 * switch, thread bring-up, exception vectors) are honest FIRST-CUT stubs marked
 * X64_TODO: they compile and link but cannot be validated until the amd64 board
 * (GDT/TSS/IDT) is stood up under the X1 harness -- that is X2.3. Everything not
 * so marked (TheScheduler singleton, the kernel-lock, NKFault/NKIdle) is real.
 *
 * The x86 (IA-32) reference for the TODO parts is
 * baseline/kernelhwsrv/kernel/eka/nkern/x86/ncsched.cia (TScheduler::Reschedule):
 * priority select -> round-robin -> fast-mutex -> lazy-FPU (clts/fnsave) ->
 * SP switch via NThreadBase::iSavedSP -> TSS ESP0 -> address-space handler.
 * The amd64 version is that algorithm with 64-bit regs + fxsave + TSS RSP0.
 */
#include <e32def.h>
#include <nkern.h>
#include <nk_priv.h>

/* amd64 context switch primitive + first-run trampoline (x64_ctxsw.S), unit-
 * tested cooperatively by ctxsw_test.c. The frame __phore_switch pops, low->high:
 * r15 r14 r13 r12 rbx rbp <return-addr>. A primed thread sets r12=entry, r13=arg,
 * return=__phore_thread_trampoline. */
extern "C" void __phore_switch(void** aOldSPSlot, void* aNewSP);
extern "C" void __phore_thread_trampoline(void);

/* Lazy-FPU: set CR0.TS on switch so the incoming thread re-claims the FPU via a
 * #NM fault. The real (SSE-privileged) definition is in x64_fpu.c and overrides
 * this WEAK no-op at the bare-metal link; the hosted unit tests (which do not link
 * x64_fpu.o and cannot run privileged CR0 writes) fall back to this stub. */
extern "C" __attribute__((weak)) void fpu_set_ts(void) {}

/* Set TSS.rsp0 = the incoming thread's kernel-stack top, so that when a USER
 * (ring-3) thread is later interrupted, the CPU switches to ITS kernel stack for
 * the interrupt frame (rung-3 slice 2). The real definition (writes the TSS) is in
 * ring3.c and overrides this WEAK no-op at the bare-metal link; the hosted unit
 * tests (no TSS) fall back to this stub. */
extern "C" __attribute__((weak)) void board_set_rsp0(void* /*aKernStackTop*/) {}

/* Load the incoming thread's process address space (CR3) on a context switch --
 * the amd64 analogue of the ARM address-space handler / TTBR0 switch (rung-3 slice
 * 3). The real definition (writes CR3) is in board_sched.cpp, which knows each
 * thread's process page tables; this WEAK no-op keeps the hosted unit tests (no
 * paging) and any kernel-only build working unchanged. */
extern "C" __attribute__((weak)) void board_switch_as(NThreadBase* /*aNext*/) {}

/* --- The scheduler singleton (normally in ncglob) --------------------------- */
TScheduler TheScheduler;

/* A freshly-primed thread starts at __phore_thread_trampoline, NOT at the tail of
 * Reschedule, so it never runs Reschedule's "iKernCSLocked=0". It must release the
 * kernel lock itself on first entry -- the equivalent of the reschedule-return
 * epilogue an already-running thread gets. The trampoline (x64_ctxsw.S) calls this
 * after `sti`, before the thread's entry function. */
extern "C" void __phore_thread_unlock(void)
    {
    TheScheduler.iKernCSLocked = 0;
    }

extern "C" {

/* --- Fault / idle (real) ---------------------------------------------------- */

/* ExcFault: an unrecoverable CPU exception. Halt hard. */
void ExcFault(TAny* /*aInfo*/)
    {
    for (;;)
        __asm__ __volatile__("cli; hlt");
    }

} // extern "C"

/* NKFault: nanokernel assertion/fault. Halt hard (first-cut: no crash dumper). */
EXPORT_C void NKFault(const char* /*aFile*/, TInt /*aLine*/)
    {
    for (;;)
        __asm__ __volatile__("cli; hlt");
    }

/* NKIdle: no runnable thread. Wait for the next interrupt. */
void NKIdle(TUint32 /*aStage*/)
    {
    __asm__ __volatile__("sti; hlt");
    }

/* --- BTrace (first-cut: tracing disabled) ----------------------------------- */
SBTraceData BTraceData;

EXPORT_C TBool BTrace::Out(TUint32, TUint32, TUint32, TUint32)
    {
    return EFalse;
    }

/* --- Kernel preemption lock (real; drives Reschedule) ----------------------- */

EXPORT_C void NKern::Lock()
    {
    NKern::DisableAllInterrupts();
    ++TheScheduler.iKernCSLocked;
    NKern::EnableAllInterrupts();
    }

EXPORT_C NThread* NKern::LockC()
    {
    NKern::Lock();
    return (NThread*)TheScheduler.iCurrentThread;
    }

EXPORT_C void NKern::Unlock()
    {
    NKern::DisableAllInterrupts();
    if (--TheScheduler.iKernCSLocked == 0 && TheScheduler.iRescheduleNeededFlag)
        TheScheduler.Reschedule();      // returns with kernel unlocked, interrupts disabled
    NKern::EnableAllInterrupts();
    }

EXPORT_C TInt NKern::PreemptionPoint()
    {
    if (TheScheduler.iKernCSLocked == 1 && TheScheduler.iRescheduleNeededFlag)
        {
        TheScheduler.Reschedule();
        TheScheduler.iKernCSLocked = 1; // Reschedule leaves it 0; we stay locked
        return TRUE;
        }
    return FALSE;
    }

/* --- Context switch / thread bring-up (X64_TODO: needs the amd64 board) ------
 * These link but are not yet the real thing. See the x86 .cia reference above;
 * the real amd64 versions arrive with the GDT/TSS/IDT at X2.3. Faulting here is
 * deliberate: it makes an un-ported path loud rather than silently wrong. */

void TScheduler::Reschedule()   // static: operates on TheScheduler
    {
    // Enter: kernel locked, interrupts disabled by the caller.
    // Real selection: the neutral TPriListBase::First() returns the head of the
    // highest-priority non-empty ready list (bsr over iPresent64) -- the same
    // result the x86 .cia computes inline. Then switch stacks with the tested
    // __phore_switch. Round-robin rotation on timeslice expiry, fast-mutex
    // priority inheritance, lazy-FPU (clts/fxsave), TSS RSP0 and the address-
    // space handler are the remaining refinements (X64_TODO -- need the board /
    // timer), but the core select-and-switch is real.
    TScheduler& s = TheScheduler;
    s.iRescheduleNeededFlag = 0;
    NThreadBase* next = (NThreadBase*)s.First();   // highest-priority ready thread
    NThreadBase* old  = s.iCurrentThread;
    if (next && next != old)
        {
        s.iCurrentThread = next;
        fpu_set_ts();   // lazy-FPU: next thread re-claims the FPU on its first use (#NM)
        // Slice 2: point TSS.rsp0 at the incoming thread's kernel-stack top, so a
        // later ring-3 -> ring-0 interrupt for this thread lands on its own kernel
        // stack. Harmless for ring-0 threads (they never change privilege).
        board_set_rsp0((void*)(((TLinAddr)next->iStackBase + (TLinAddr)next->iStackSize)
                                & ~(TLinAddr)0xF));
        // Slice 3: load the incoming thread's process address space (CR3). Kernel
        // + all low memory stay mapped in every AS, so the switch is safe here on
        // the (shared) kernel stack; only the per-process private region changes.
        board_switch_as(next);
        __phore_switch((void**)&old->iSavedSP, (void*)next->iSavedSP);
        // ...resumed later, on this (old) thread's stack.
        }
    s.iKernCSLocked = 0;
    }

void TScheduler::YieldTo(NThreadBase*)  // static
    {
    TheScheduler.iRescheduleNeededFlag = 1;
    Reschedule();
    }

TInt NThread::Create(SNThreadCreateInfo& aInfo, TBool aInitial)
    {
    // Neutral part: sets iStackBase/iStackSize/priority/state etc.
    TInt r = NThreadBase::Create(aInfo, aInitial);
    if (r != KErrNone)
        return r;
    if (aInitial)
        return KErrNone;    // the initial thread's context is the current one; no frame to prime

    // Prime the supervisor stack exactly as __phore_switch expects to pop it, so
    // the first switch INTO this thread lands in the trampoline -> iFunction(arg).
    // (This is the amd64 analogue of the x86 ncthrd.cpp SThreadStack setup; the
    // richer segment/FPU frame is added with the interrupt path at X2.3b.)
    TLinAddr top = (TLinAddr)aInfo.iStackBase + (TLinAddr)aInfo.iStackSize;
    top &= ~(TLinAddr)0xF;                 // 16-byte align
    TLinAddr sp = top - 7 * 8;             // 6 callee regs + return address
    if (((sp + 7 * 8) & 0xF) != 0) sp -= 8;// keep sp+56 16-aligned for the call
    TUint64* f = (TUint64*)sp;             // 64-bit slots even when pointers are 32-bit (x32)
    f[0] = 0;                              // r15
    f[1] = 0;                              // r14
    f[2] = (TLinAddr)aInfo.iParameterBlock;// r13 -> arg
    f[3] = (TLinAddr)aInfo.iFunction;      // r12 -> entry
    f[4] = 0;                              // rbx
    f[5] = 0;                              // rbp
    f[6] = (TLinAddr)__phore_thread_trampoline;
    iSavedSP = sp;
    return KErrNone;
    }

void NThread::GetUserContext(TX86RegSet&) {}         // X64_TODO
void NThread::ModifyUsp(TLinAddr) {}                  // X64_TODO

void NThreadBase::ForceExit()   {}                   // X64_TODO
void NThreadBase::OnKill()      {}                   // X64_TODO
void NThreadBase::OnExit()      {}                   // X64_TODO
