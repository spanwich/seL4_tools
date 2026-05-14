/*
 * Copyright 2026, PhD Research Project — multikernel-AMP MVP-Q (x86_64)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * x86_64 elfloader entry — built as a 32-bit elf32-i386 binary because QEMU's
 * Multiboot v1 loader only accepts 32-bit ELFs (and so does GRUB legacy).
 * The seL4 x86_64 kernel itself has a 32-bit Multiboot entry (head.S:269)
 * and transitions to long mode internally, so we don't need to go 64-bit in
 * the elfloader at all. Mode-transition complexity stays in the kernel.
 *
 * Bootloader enters at `_start` in 32-bit protected mode, paging off, IRQs
 * disabled. %eax = 0x2BADB002 (Multiboot magic), %ebx = paddr of MBI struct.
 * We save those for main(), set up our own stack, clear BSS, call main().
 */

#define MULTIBOOT_MAGIC      0x1BADB002
/* flag 0: align modules on page boundary; flag 1: provide mmap */
#define MULTIBOOT_FLAGS      0x00000003
#define MULTIBOOT_CHECKSUM   -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

/* ============================================================== */
/* Multiboot v1 header — must be near the start of the binary,    */
/* 4-byte aligned, within the first 8 KiB of the file.            */
/* ============================================================== */
.section .multiboot, "a"
.align 4
.globl multiboot_header
multiboot_header:
    .long MULTIBOOT_MAGIC
    .long MULTIBOOT_FLAGS
    .long MULTIBOOT_CHECKSUM

/* ============================================================== */
/* 32-bit entry. Bootloader hands us %eax/%ebx in protected mode. */
/* ============================================================== */
.section .text, "ax"
.code32
.globl _start
_start:
    cli
    cld

    /* Stash Multiboot args in callee-saved regs so the BSS clear can't
     * touch them. (%esi and %edi are clobbered by `rep stosb`; %ebp/%ebx
     * survive.) */
    movl    %eax, %ebp          /* %ebp = Multiboot magic */
    /* %ebx already holds MBI pointer */

    /* Clear .bss BEFORE setting up the stack — the stack lives inside .bss
     * (boot_stack_bottom..boot_stack_top), so anything pushed before this
     * clear would be wiped. */
    leal    _bss_start, %edi
    leal    _bss_end, %ecx
    subl    %edi, %ecx
    xorl    %eax, %eax
    rep     stosb

    /* Now safe to set up the stack. */
    leal    boot_stack_top, %esp

    /* Push args (cdecl, right-to-left) and call main(magic, mbi_paddr). */
    pushl   %ebx                /* arg 2: mbi_paddr */
    pushl   %ebp                /* arg 1: magic */
    call    main

    /* main() shouldn't return. Halt if it does. */
.Lhang:
    cli
    hlt
    jmp     .Lhang

/* ============================================================== */
/* BSS — stack only for now (page tables come later when we add   */
/* the long-mode transition for the K1 AP trampoline).            */
/* ============================================================== */
.section .bss, "aw", @nobits

.align 16
boot_stack_bottom:
    .skip 8192
boot_stack_top:

/* Non-executable stack marker — silences linker deprecation warning. */
.section .note.GNU-stack, "", @progbits
