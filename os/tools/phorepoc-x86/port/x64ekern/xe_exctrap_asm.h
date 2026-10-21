/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// xe_exctrap_asm.h -- the register save/resume sequences behind TExcTrap / TPagingExcTrap
// (Intel syntax). In a header so the hosted run-test exctrap_test.c exercises the EXACT
// instruction sequences the kernel uses. rdi = trap object (iState at offset 0), esi = result.
#ifndef XE_EXCTRAP_ASM_H
#define XE_EXCTRAP_ASM_H

#define X64_SAVE_NONVOLATILE \
	"mov rax, [rsp]\n"       "mov [rdi], rax\n"    /* rip = our return address          */ \
	"lea rax, [rsp+8]\n"     "mov [rdi+8], rax\n"  /* rsp as it will be after we return */ \
	"mov [rdi+16], rbx\n"    "mov [rdi+24], rbp\n" \
	"mov [rdi+32], r12\n"    "mov [rdi+40], r13\n" \
	"mov [rdi+48], r14\n"    "mov [rdi+56], r15\n"

// eax = aResult; resume at the saved rip as if Trap() had just returned it.
#define X64_RESUME_NONVOLATILE \
	"mov eax, esi\n" \
	"mov rbx, [rdi+16]\n"    "mov rbp, [rdi+24]\n" \
	"mov r12, [rdi+32]\n"    "mov r13, [rdi+40]\n" \
	"mov r14, [rdi+48]\n"    "mov r15, [rdi+56]\n" \
	"mov rsp, [rdi+8]\n"     "jmp [rdi]\n"

#endif
