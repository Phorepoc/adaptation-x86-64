/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

/* boot.S -- Phorepoc x86_64 bring-up: multiboot2 entry -> long mode -> kmain.
 *
 * This is NET-NEW authoring (Symbian/EKA2 has no x86 board support anywhere,
 * see docs/phorepoc/multiarch-kernel-port.md). It is the x86_64 analogue of the
 * ARM tools/qemu-syborg/mini_kernel.S "PHOREPOC KERNEL ALIVE" milestone.
 *
 * GRUB (multiboot2) loads us in 32-bit protected mode, paging off.
 * We identity-map the low 4 GiB with 2 MiB pages (covers the VBE/BGA framebuffer,
 * which VirtualBox/qemu place below 4 GiB), enable long mode, and jump to kmain.
 * AT&T syntax on purpose -- the long-mode far jump template is well-known here.
 */

#ifndef FB_W
#define FB_W 1024
#define FB_H 768
#endif
.set MB2_MAGIC,   0xe85250d6
.set MB2_ARCH,    0            /* i386 */

/* ---- Multiboot2 header (must be 8-byte aligned, within first 32 KiB) ---- */
.section .multiboot, "a"
.align 8
mb_header:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long mb_header_end - mb_header
    .long -(MB2_MAGIC + MB2_ARCH + (mb_header_end - mb_header))
    /* framebuffer request tag: ask GRUB for a linear graphics framebuffer */
    .align 8
    .short 5              /* type = framebuffer */
    .short 0              /* flags = 0 (required) */
    .long 20              /* size */
    .long FB_W            /* width  (0 = no preference) */
    .long FB_H            /* height -- make_ekern.sh: SCREEN x SCALE (X4.35) */
    .long 32              /* depth  */
    /* end tag */
    .align 8
    .short 0
    .short 0
    .long 8
mb_header_end:

/* ---- page tables + stack ---- */
.section .bss, "aw", @nobits
.align 4096
pml4:  .skip 4096
pdpt:  .skip 4096
pd:    .skip 16384            /* 4 PDs -> 2048 * 2 MiB = 4 GiB identity map */
.align 16
stack_bottom: .skip 16384
stack_top:

/* ---- 64-bit GDT ---- */
.section .rodata
.align 8
gdt64:
    .quad 0                          /* null */
    .quad 0x00209A0000000000         /* code64: present, exec, L=1 */
    .quad 0x0000920000000000         /* data:   present, writable  */
gdt64_end:
gdt64_ptr:
    .short gdt64_end - gdt64 - 1
    .quad gdt64

/* ---- 32-bit entry from GRUB ---- */
.section .text
.code32
.global _start
_start:
    movl $stack_top, %esp
    movl %ebx, %edi                  /* save multiboot2 info ptr -> kmain arg (rdi) */

    /* PML4[0] = PDPT | present|rw|user.  The USER bit (0x4) is set at EVERY
     * paging level so ring-3 code can execute + read/write the identity map.
     * This is a single flat address space shared by kernel and user -- a
     * deliberate slice-1 simplification; per-process CR3 comes in rung-3 slice 3. */
    movl $pdpt, %eax
    orl  $7, %eax
    movl %eax, pml4

    /* PDPT[0..3] = (pd + k*4096) | present|rw|user */
    xorl %ecx, %ecx
1:
    movl $pd, %eax
    movl %ecx, %ebx
    shll $12, %ebx
    addl %ebx, %eax
    orl  $7, %eax
    movl %eax, pdpt(,%ecx,8)
    movl $0, pdpt+4(,%ecx,8)
    incl %ecx
    cmpl $4, %ecx
    jne  1b

    /* PD[0..2047] = i*2MiB | present|rw|user|PS  (identity map low 4 GiB) */
    xorl %ecx, %ecx
2:
    movl $0x200000, %eax
    mull %ecx                        /* edx:eax = 2MiB * ecx (fits in 32 bits) */
    orl  $0x87, %eax
    movl %eax, pd(,%ecx,8)
    movl $0, pd+4(,%ecx,8)
    incl %ecx
    cmpl $2048, %ecx
    jne  2b

    /* Mark the LAPIC MMIO page uncacheable.  0xFEE00000 / 2MiB = PD index 2039;
     * set PCD|PWT (bits 4,3 = 0x18) on that one 2 MiB entry so LAPIC register
     * reads/writes are strongly ordered UC, not cached.  (IOAPIC at 0xFEC00000
     * sits in the neighbouring 2 MiB page and is left cacheable for now.) */
    orl  $0x18, pd+2039*8
    movl $pml4, %eax
    movl %eax, %cr3

    /* CR4.PAE = 1 */
    movl %cr4, %eax
    orl  $(1<<5), %eax
    movl %eax, %cr4

    /* EFER.LME = 1 */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $(1<<8), %eax
    wrmsr

    /* CR0.PG = 1 */
    movl %cr0, %eax
    orl  $(1<<31), %eax
    movl %eax, %cr0

    lgdt gdt64_ptr
    ljmp $0x08, $long_mode_start

/* ---- 64-bit ---- */
.code64
long_mode_start:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movq $stack_top, %rsp
    /* %edi holds the multiboot2 info ptr, zero-extended into %rdi */
    call kmain
3:  cli
    hlt
    jmp  3b
