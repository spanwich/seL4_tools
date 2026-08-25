/*
 * Copyright 2026, PhD Research Project — multikernel-AMP MVP-Q (x86_64)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AP SIPI trampoline — generalised for serialised N-AP bringup (Task C).
 *
 * The BSP stages this blob once at TRAMPOLINE_PADDR (0x8000 — SIPI vector
 * 0x08). For each AP i (1..N-1) it writes K_i's entry paddr and MBI paddr
 * into ap_kentry/ap_kmbi, clears ap_ack, then issues INIT+STARTUP IPI.
 *
 * On SIPI the AP wakes here in 16-bit real mode (segments undefined,
 * paging off). We:
 *   1. Load a flat 32-bit GDT and enter protected mode.
 *   2. Reload data segments, set an AP-local stack.
 *   3. Latch ap_kentry/ap_kmbi into registers.
 *   4. Set ap_ack = 1 — tells the BSP it is now safe to reprogram the
 *      trampoline for the next AP (we no longer read those slots).
 *   5. Jump to K_i's 32-bit Multiboot entry (EAX=magic, EBX=MBI).
 *
 * Steps 3→4→5 use only registers after the latch, so a concurrent BSP
 * rewrite of the slots for AP i+1 cannot corrupt this AP's jump.
 */

#define TRAMPOLINE_PADDR    0x8000
#define AP_STACK_TOP        0x9000
#define MULTIBOOT_MAGIC     0x2BADB002

#define ABS(label)          ((label - ap_trampoline_start) + TRAMPOLINE_PADDR)

    .section .ap_trampoline, "ax"
    .globl ap_trampoline_start
    .globl ap_trampoline_end
    .globl ap_kentry
    .globl ap_kmbi
    .globl ap_ack

    .code16
ap_trampoline_start:
    cli
    cld

    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    lgdtl   ABS(ap_gdt_ptr)

    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0

    ljmpl   $0x08, $ABS(ap_protected_mode)

    .code32
ap_protected_mode:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    movl    $AP_STACK_TOP, %esp

    /* Latch entry + MBI into registers BEFORE acking. */
    movl    ABS(ap_kentry), %edx        /* %edx = K_i entry paddr */
    movl    ABS(ap_kmbi),   %ebx        /* %ebx = &K_i MBI        */

    /* Tell the BSP we have latched — it may now reprogram the slots. */
    movl    $1, ABS(ap_ack)

    /* Multiboot v1 calling convention for K_i's _start. */
    movl    $MULTIBOOT_MAGIC, %eax
    jmp     *%edx

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

    /* -------- Patch slots, rewritten by the BSP per AP -------- */
    .align 4
ap_kentry:
    .long 0xDEADBEEF
ap_kmbi:
    .long 0xCAFEBABE
ap_ack:
    .long 0

    .align 16
ap_trampoline_end:

    /* Non-executable stack marker for the elfloader binary. */
    .section .note.GNU-stack, "", @progbits
