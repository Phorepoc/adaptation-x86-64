/* ctxsw_test.c -- unit test for the Phorepoc amd64 context switch primitive
 * (x64_ctxsw.S). Cooperative ping-pong: a main context creates two "threads",
 * each with a stack primed the way NThread::Create will prime it, and they hand
 * control back and forth via __phore_switch, printing as they run. Proves the
 * amd64 SP-switch + first-run trampoline that TScheduler::Reschedule is built on.
 *
 * Hosted (Linux, in the phorepoc-x86 docker image) so it actually RUNS -- no
 * board/IDT needed for a cooperative switch. gcc -m64.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern void __phore_switch(void** aOldSPSlot, void* aNewSP);
extern void __phore_thread_trampoline(void);

#define STACK_WORDS 4096
static uint64_t stackA[STACK_WORDS];
static uint64_t stackB[STACK_WORDS];

/* Saved-SP slots, the stand-ins for NThreadBase::iSavedSP. */
static void* spMain;
static void* spA;
static void* spB;

static int gCount = 0;

/* Prime a stack so the first __phore_switch into it lands in the trampoline,
 * which calls entry(arg). Mirrors what NThread::Create will do. Returns the
 * initial saved-SP. Layout the switch pops (low->high): r15 r14 r13 r12 rbx rbp
 * then the return address. We set r12=entry, r13=arg, ret=trampoline. */
static void* prime(uint64_t* stackBase, size_t words, void (*entry)(void*), void* arg)
{
    uint64_t* top = stackBase + words;      /* stack grows down from here */
    /* Align so that when the trampoline executes `call entry`, rsp%16==0. */
    uintptr_t sp = (uintptr_t)top & ~(uintptr_t)0xF;
    sp -= 7 * 8;                            /* 6 callee regs + return address */
    if (((sp + 7 * 8) & 0xF) != 0) sp -= 8; /* keep new_sp+56 16-aligned      */
    uint64_t* f = (uint64_t*)sp;
    f[0] = 0;                               /* r15 */
    f[1] = 0;                               /* r14 */
    f[2] = (uint64_t)arg;                   /* r13 -> arg  */
    f[3] = (uint64_t)entry;                 /* r12 -> entry */
    f[4] = 0;                               /* rbx */
    f[5] = 0;                               /* rbp */
    f[6] = (uint64_t)__phore_thread_trampoline;
    return f;
}

static void threadA(void* arg)
{
    const char* name = (const char*)arg;
    for (int i = 0; i < 3; i++) {
        printf("  [%s] tick %d\n", name, i);
        gCount++;
        __phore_switch(&spA, spB);          /* yield to B */
    }
    printf("  [%s] done, returning to main\n", name);
    __phore_switch(&spA, spMain);           /* back to main */
}

static void threadB(void* arg)
{
    const char* name = (const char*)arg;
    for (int i = 0; i < 3; i++) {
        printf("  [%s] tock %d\n", name, i);
        gCount++;
        __phore_switch(&spB, spA);          /* yield to A */
    }
    printf("  [%s] done\n", name);
    __phore_switch(&spB, spA);              /* let A finish + return to main */
}

int main(void)
{
    printf("== amd64 context-switch ping-pong ==\n");
    spA = prime(stackA, STACK_WORDS, threadA, (void*)"A");
    spB = prime(stackB, STACK_WORDS, threadB, (void*)"B");
    __phore_switch(&spMain, spA);           /* start A; returns here when A is done */
    printf("== back in main, %d context switches into threads ==\n", gCount);
    printf("%s\n", gCount == 6 ? "CTXSW OK" : "CTXSW FAIL");
    return gCount == 6 ? 0 : 1;
}
