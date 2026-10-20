/* exctrap_test.c -- hosted run-test of the TExcTrap register save/resume sequences
 * (xe_exctrap_asm.h), the same way ctxsw_test.c proved the context switch. Hosted = LP64
 * (an x32 process needs an x32 libc + CONFIG_X86_X32 host kernel); the sequences touch only
 * 64-bit registers and qword slots, so they are bit-identical under both ABIs.
 *   bash tools/phorepoc-x86/port/x64ekern/build_exctrap_test.sh   ->  EXCTRAP OK
 */
#include <stdio.h>
#include "xe_exctrap_asm.h"

struct trap { unsigned int iState[16]; };	/* TUint32 iState[EXC_TRAP_CTX_SZ] */

/* shape of TExcTrap::Trap(): save, then "bookkeeping helper returns 0 to OUR caller" */
__attribute__((naked, noinline, returns_twice)) int trap_enter(struct trap *t)
{
	__asm__(X64_SAVE_NONVOLATILE "xor eax, eax\n" "ret\n");
}
/* shape of TExcTrap::Exception(aResult) after its bookkeeping call */
__attribute__((naked, noinline, noreturn)) void trap_raise(struct trap *t, int result)
{
	__asm__(X64_RESUME_NONVOLATILE);
}

/* Trash every non-volatile register, then raise: the resume must bring them all back. */
__attribute__((naked, noinline, noreturn)) void clobber_and_raise(struct trap *t, int result)
{
	__asm__("mov rbx, 0x1111111111111111\n" "mov rbp, 0x2222222222222222\n"
			"mov r12, 0x3333333333333333\n" "mov r13, 0x4444444444444444\n"
			"mov r14, 0x5555555555555555\n" "mov r15, 0x6666666666666666\n"
			"sub rsp, 0x200\n"
			"jmp trap_raise\n");
}

__attribute__((noinline)) static void deep(struct trap *t, int depth, int result)
{
	volatile char pad[64];
	pad[0] = (char)depth;
	if (depth)
		deep(t, depth - 1, result);
	else
		clobber_and_raise(t, result);
	pad[1] = 0;				/* keep this a real (non-tail) call */
}

/* Many live values across the trap so the compiler parks them in rbx/rbp/r12-r15. */
__attribute__((noinline)) static int scenario(unsigned long long seed, int result)
{
	struct trap t;
	unsigned long long a = seed * 3 + 1, b = seed * 5 + 2, c = seed * 7 + 3,
					   d = seed * 11 + 4, e = seed * 13 + 5, f = seed * 17 + 6;
	int r = trap_enter(&t);
	if (r == 0)
		{
		deep(&t, 5, result);
		return -1000;			/* never: deep() raises */
		}
	if (a != seed * 3 + 1 || b != seed * 5 + 2 || c != seed * 7 + 3 ||
		d != seed * 11 + 4 || e != seed * 13 + 5 || f != seed * 17 + 6)
		return -2000;			/* a non-volatile register was not restored */
	return r;
}

int main(void)
{
	int ok = 1, i;
	for (i = 1; i <= 4; i++)
		{
		int want = -5 * i;		/* KErrXxx-style negative results */
		int got = scenario(0x0123456789abcdefULL * i, want);
		printf("  trap %d: raised %d -> Trap() returned %d\n", i, want, got);
		if (got != want) ok = 0;
		}
	puts(ok ? "EXCTRAP OK" : "EXCTRAP FAIL");
	return !ok;
}
