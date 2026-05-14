/*
 * Copyright 2026, PhD Research Project — multikernel-AMP MVP-Q (x86_64)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AP (core 1) SIPI trampoline. The BSP copies this blob to physical
 * address TRAMPOLINE_PADDR (0x8000 — SIPI vector 0x08) and writes
 * K1's entry paddr + K1's MBI paddr to the data slots at the end of
 * the blob (ap_k1_entry / ap_k1_mbi) before issuing INIT+STARTUP IPI.
 *
 * On SIPI, AP wakes at TRAMPOLINE_PADDR in 16-bit real mode, segments
 * undefined, paging off. We:
 *   1. Load a small flat 32-bit GDT (code + data covering 0..4G).
 *   2. Set CR0.PE → protected mode.
 *   3. Far-jump to the 32-bit code segment.
 *   4. Reload data segments, set up an AP-local stack.
 *   5. Load Multiboot magic in EAX, K1 MBI paddr in EBX.
 *   6. Indirect jump to K1's entry paddr.
 *
 * The "trampoline" lives at a fixed paddr (0x8000) and uses
 * absolute addresses via cs/ds:offset arithmetic that works because the
 * trampoline is loaded at exactly TRAMPOLINE_PADDR.
 */

#define TRAMPOLINE_PADDR    0x8000
#define AP_STACK_TOP        0x9000
#define MULTIBOOT_MAGIC     0x2BADB002

#define ABS(label)          ((label - ap_trampoline_start) + TRAMPOLINE_PADDR)

    .section .ap_trampoline, "ax"
    .globl ap_trampoline_start
    .globl ap_trampoline_end
    .globl ap_k1_entry
    .globl ap_k1_mbi

    .code16
ap_trampoline_start:
    cli
    cld

    /* Zero data segments so 16-bit absolute references work via DS:0. */
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    /* Load the trampoline's GDT (32-bit-flat code + data). */
    lgdtl   ABS(ap_gdt_ptr)

    /* Enter protected mode. */
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0

    /* Far jump to flush the 16-bit pipeline and land in 32-bit code. */
    ljmpl   $0x08, $ABS(ap_protected_mode)

    .code32
ap_protected_mode:
    /* Reload data segments with the 32-bit flat data descriptor. */
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    /* AP-local boot stack — top of an unused page below the trampoline. */
    movl    $AP_STACK_TOP, %esp

    /* Set Multiboot v1 calling convention for K1's _start:
     *   EAX = MULTIBOOT_MAGIC
     *   EBX = K1 MBI paddr (BSP wrote ap_k1_mbi before SIPI) */
    movl    ABS(ap_k1_mbi), %ebx
    movl    $MULTIBOOT_MAGIC, %eax
    /* Indirect jump to K1's _start (BSP wrote ap_k1_entry). */
    jmp     *ABS(ap_k1_entry)

    /* -------- GDT for the trampoline (32-bit flat) -------- */
    .align 8
ap_gdt:
    .quad 0x0000000000000000        /* 0x00 null */
    .quad 0x00CF9A000000FFFF        /* 0x08 code32 base=0 lim=4G */
    .quad 0x00CF92000000FFFF        /* 0x10 data32 base=0 lim=4G */
ap_gdt_end:

    .align 4
ap_gdt_ptr:
    .word ap_gdt_end - ap_gdt - 1
    .long ABS(ap_gdt)

    /* -------- Patch slots for the BSP -------- */
    /* Written by the BSP before SIPI; consumed by ap_protected_mode. */
    .align 4
    .globl ap_k1_entry
ap_k1_entry:
    .long 0xDEADBEEF
    .globl ap_k1_mbi
ap_k1_mbi:
    .long 0xCAFEBABE

    .align 16
ap_trampoline_end:

    /* Non-executable stack marker for the elfloader binary. */
    .section .note.GNU-stack, "", @progbits
