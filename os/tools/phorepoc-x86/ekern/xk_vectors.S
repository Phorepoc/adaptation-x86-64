/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

/* xk_vectors.S -- Phorepoc amd64/x32 kernel image: the 256 interrupt-gate entry stubs.
 * Every vector funnels into X64_Trap(frame) (xk_irq.cpp): interrupts are dispatched and return
 * through the iretq below (possibly after a reschedule); CPU exceptions still go to the loud
 * early-trap reporter (xk_entry.cpp) until Exc::Dispatch is wired -- never a silent triple fault.
 *
 * Frame handed to C (low -> high): r15..rax (15 qwords), vector, error code, then the CPU's
 * rip, cs, rflags, rsp, ss. Intel syntax like the rest of the kernel image.
 */
    .intel_syntax noprefix
    .text

    .macro XK_STUB n
    .align 16
xk_vec_\n:
    .if !((\n == 8) || ((\n >= 10) && (\n <= 14)) || (\n == 17) || (\n == 21) || (\n == 29) || (\n == 30))
    push 0                      /* CPU pushed no error code for this vector */
    .endif
    push \n
    jmp  xk_common
    .endm

    .altmacro
    .set i, 0
    .rept 256
    XK_STUB %i
    .set i, i+1
    .endr

xk_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov  rdi, rsp               /* arg0 = frame */
    mov  rbx, rsp               /* rbx is callee-saved: keeps the frame across the C call ...     */
    and  rsp, -16
    cld
    call X64_Trap               /* ... which may Reschedule() away and come back much later,      */
    mov  rsp, rbx               /*     on this same thread stack (xk_irq.cpp)                     */
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdi
    pop  rsi
    pop  rbp
    pop  rbx
    pop  rdx
    pop  rcx
    pop  rax
    add  rsp, 16                /* vector + error code */
    iretq

/* Table of the 256 stub addresses for the IDT builder (32-bit: the image lives below 4 GiB). */
    .section .rodata
    .align 4
    .global xk_vector_table
xk_vector_table:
    .macro XK_ENTRY n
    .long xk_vec_\n
    .endm
    .set i, 0
    .rept 256
    XK_ENTRY %i
    .set i, i+1
    .endr

    .section .note.GNU-stack, "", @progbits
