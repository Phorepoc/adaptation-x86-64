// xu_setjmp.cpp -- Path C (amd64/x32): setjmp/longjmp for ESTLIB, in place of the ARM naked-asm
// genericopenlibs/cstdlib/LSIGNAL/JMP_MARM.CIA (registry x32_drop/x32_extra). X4.26.
//
// jmp_buf (cstdlib setjmp.h) is 16 x unsigned long = 64 bytes under x32 -- exactly the SysV
// callee-saved state: rbx rbp r12 r13 r14 r15 (full 64-bit: x32 code may keep 64-bit values there),
// the caller's rsp, and the return rip. Arguments arrive in edi/esi (x32 pointers are zero-extended).
// longjmp returns aRetval, or 1 when aRetval == 0 (as the ARM version's movs/moveq).
__asm__(
	".att_syntax\n"					// the x32 user line compiles with -masm=intel
	".text\n"
	".globl setjmp\n"
	".type setjmp, @function\n"
	"setjmp:\n"
	"	mov %rbx, 0(%rdi)\n"
	"	mov %rbp, 8(%rdi)\n"
	"	mov %r12, 16(%rdi)\n"
	"	mov %r13, 24(%rdi)\n"
	"	mov %r14, 32(%rdi)\n"
	"	mov %r15, 40(%rdi)\n"
	"	lea 8(%rsp), %rdx\n"			// rsp as it will be after our ret
	"	mov %rdx, 48(%rdi)\n"
	"	mov (%rsp), %rdx\n"				// the return address
	"	mov %rdx, 56(%rdi)\n"
	"	xor %eax, %eax\n"
	"	ret\n"
	".size setjmp, .-setjmp\n"
	".globl longjmp\n"
	".type longjmp, @function\n"
	"longjmp:\n"
	"	mov %esi, %eax\n"
	"	test %eax, %eax\n"
	"	jnz 1f\n"
	"	mov $1, %eax\n"
	"1:\n"
	"	mov 0(%rdi), %rbx\n"
	"	mov 8(%rdi), %rbp\n"
	"	mov 16(%rdi), %r12\n"
	"	mov 24(%rdi), %r13\n"
	"	mov 32(%rdi), %r14\n"
	"	mov 40(%rdi), %r15\n"
	"	mov 48(%rdi), %rsp\n"
	"	jmp *56(%rdi)\n"
	".size longjmp, .-longjmp\n"
	".intel_syntax noprefix\n"
	);
