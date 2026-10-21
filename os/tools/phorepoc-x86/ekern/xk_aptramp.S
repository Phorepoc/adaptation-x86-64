/* Portions Copyright (c) Nokia Corporation and/or its subsidiary(-ies)
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

/* xk_aptramp.S -- Phorepoc amd64/x32 kernel image: the application-processor start-up trampoline.
 * Kernel-image sibling of the nanokernel demo's smp.S (and of what nkernsmp/x86/nccpu.cpp builds in
 * its KApBootPage): NKern::BootAP (xk_smp.cpp) COPIES these bytes to physical 0x8000, fills the
 * parameter block at their end, and sends INIT-SIPI-SIPI (vector 0x08). The AP climbs
 *   16-bit real -> 32-bit protected -> long mode, on ITS OWN per-CPU PML4 (x64_root.h),
 * then jumps to X64_ApEntry in the high half on the initial stack the kernel allocated for it.
 *
 * It never runs at its link address, so nothing here may need a relocation: every reference is
 * either a difference of two local symbols or the fixed copy address AP_BASE + such a difference.
 * While an AP boots, the kernel's local page directory carries a temporary identity 2 MiB page so
 * that AP_BASE stays mapped across the CR0.PG flip. AT&T syntax.
 */
    .set AP_BASE, 0x8000

    .section .rodata
    .balign 16
    .code16
    .global xk_ap_tramp_start
xk_ap_tramp_start:
    cli
    cld
    movw %cs, %ax
    movw %ax, %ds                                   /* DS base = CS<<4 = AP_BASE */
    lgdtl (tramp_gdtr - xk_ap_tramp_start)
    movl %cr0, %eax
    orl  $1, %eax
    movl %eax, %cr0                                 /* PE=1; segment caches persist */
    ljmpl *(tramp_fptr - xk_ap_tramp_start)

    .code32
prot32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $(1<<5), %eax                              /* CR4.PAE (the rest follows in long mode) */
    movl %eax, %cr4
    movl (AP_BASE + p_cr3 - xk_ap_tramp_start), %eax
    movl %eax, %cr3
    movl $0xC0000080, %ecx                          /* EFER := the boot CPU's (LME, NXE, SCE ...) */
    movl (AP_BASE + p_efer - xk_ap_tramp_start), %eax
    xorl %edx, %edx
    wrmsr
    movl $0x80000011, %eax                          /* CR0: PG | ET | PE */
    movl %eax, %cr0
    ljmp $0x18, $(AP_BASE + long64 - xk_ap_tramp_start)

    .code64
long64:
    movl (AP_BASE + p_cr4 - xk_ap_tramp_start), %eax
    movq %rax, %cr4                                 /* the boot CPU's CR4 (PAE, PGE, OSFXSR ...) */
    movl (AP_BASE + p_cr0 - xk_ap_tramp_start), %eax
    movq %rax, %cr0                                 /* ... and CR0 (WP, NE, MP ...) */
    movl (AP_BASE + p_stack - xk_ap_tramp_start), %esp
    movl (AP_BASE + p_info - xk_ap_tramp_start), %edi
    movl (AP_BASE + p_entry - xk_ap_tramp_start), %eax
    xorl %ebp, %ebp
    jmp  *%rax                                      /* X64_ApEntry(info), high half */

    .balign 8
tramp_gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF                        /* 0x08 code32 */
    .quad 0x00CF92000000FFFF                        /* 0x10 data32 */
    .quad 0x00209A0000000000                        /* 0x18 code64 */
tramp_gdt_end:
tramp_gdtr:
    .word tramp_gdt_end - tramp_gdt - 1
    .long AP_BASE + tramp_gdt - xk_ap_tramp_start
tramp_fptr:
    .long AP_BASE + prot32 - xk_ap_tramp_start
    .word 0x08

/* Parameter block, filled by NKern::BootAP in the COPY (struct SX64ApParams in xk_smp.cpp). */
    .balign 8
    .global xk_ap_params
xk_ap_params:
p_cr3:   .long 0
p_entry: .long 0
p_stack: .long 0
p_info:  .long 0
p_cr4:   .long 0
p_cr0:   .long 0
p_efer:  .long 0
p_alive: .long 0
    .global xk_ap_tramp_end
xk_ap_tramp_end:

    .section .note.GNU-stack, "", @progbits
