/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

/* x64_ctxsw.S -- Phorepoc amd64 context switch primitive.
 *
 * The heart of TScheduler::Reschedule on amd64: save the current thread's
 * callee-saved registers + RSP into its NThreadBase::iSavedSP slot, load the
 * next thread's saved RSP, restore its callee-saved set, and return onto its
 * stack. System V amd64 callee-saved set = rbx, rbp, r12, r13, r14, r15.
 *
 * This is the amd64 analogue of the SP-switch core of nkern/x86/ncsched.cia
 * ("mov [esi+iSavedSP], esp" / "mov esp, [ebx+iSavedSP]"), factored out so it is
 * unit-testable on its own (cooperative ping-pong) before the full preemptive
 * scheduler (IDT/timer) exists.
 *
 * AT&T syntax.
 */
    .text

/* void __phore_switch(void** aOldSPSlot, void* aNewSP);
 *   rdi = aOldSPSlot, rsi = aNewSP.
 * Saves current context, writes RSP to *aOldSPSlot, switches to aNewSP,
 * restores that context, and returns onto the new stack. */
    .global __phore_switch
    .type   __phore_switch, @function
__phore_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
#ifdef __ILP32__            /* x32 ABI: iSavedSP is a 4-byte slot */
    movl  %esp, (%rdi)
    movl  %esi, %esi        /* zero-extend the incoming 32-bit SP */
#else
    movq  %rsp, (%rdi)      /* *aOldSPSlot = RSP (this context is now parked) */
#endif
    movq  %rsi, %rsp        /* RSP = new context's saved SP                   */
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    ret                     /* returns to the parked/primed RIP on the new stack */
    .size __phore_switch, . - __phore_switch

/* SMP reschedule, in two halves (nkernsmp/x86/ncsched.cia TScheduler::Reschedule). On SMP the
 * outgoing thread may be picked up by ANOTHER cpu the moment SelectNextThread releases it, so its
 * context must be parked -- and this cpu must be OFF its stack -- BEFORE the selection runs:
 *
 * void __phore_resched_park(void** aOldSPSlot, void* const* aTempSPSlot, void (*aFn)(void*), void* aArg);
 *   rdi, rsi, rdx, rcx.  Parks the current context exactly as __phore_switch does (*aOldSPSlot = RSP),
 *   then moves to the temporary stack *aTempSPSlot -- read AFTER the park, because for a cpu's initial
 *   thread the two slots are the same one -- and calls aFn(aArg), which never returns: it ends in
 *   __phore_resume. The parked context resumes as if this function had returned.
 *
 * void __phore_resume(void* aNewSP);   rdi.  The second half of __phore_switch. */
    .global __phore_resched_park
    .type   __phore_resched_park, @function
__phore_resched_park:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
#ifdef __ILP32__
    movl  %esp, (%rdi)
    movl  (%rsi), %esi      /* zero-extends */
#else
    movq  %rsp, (%rdi)
    movq  (%rsi), %rsi
#endif
    movq  %rsi, %rsp
    andq  $-16, %rsp
    movq  %rcx, %rdi
    callq *%rdx
    ud2
    .size __phore_resched_park, . - __phore_resched_park

    .global __phore_resume
    .type   __phore_resume, @function
__phore_resume:
#ifdef __ILP32__
    movl  %edi, %edi
#endif
    movq  %rdi, %rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    ret
    .size __phore_resume, . - __phore_resume

/* void __phore_thread_trampoline(void);
 * First-run entry for a freshly primed thread. NThread::Create lays the initial
 * stack so the 6 popped callee regs carry r12=entry, r13=arg, and the ret lands
 * here. We hand arg in rdi and call entry; if it returns we spin (a real thread
 * would NKern::ThreadExit -- wired later). */
    .global __phore_thread_trampoline
    .type   __phore_thread_trampoline, @function
__phore_thread_trampoline:
    movq  %r13, %rdi        /* arg */
    callq *%r12             /* entry(arg) */
1:  jmp   1b
    .size __phore_thread_trampoline, . - __phore_thread_trampoline

    .section .note.GNU-stack, "", @progbits
