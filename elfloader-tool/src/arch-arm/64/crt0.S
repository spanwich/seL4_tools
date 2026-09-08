/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <autoconf.h>
#include <elfloader/gen_config.h>
#ifdef CONFIG_IMAGE_BINARY
#include <image_start_addr.h>
#endif

#include <assembler.h>

#ifdef CONFIG_PLAT_MLXBF2
.macro BF2_EFI_MARKER label
#if defined(CONFIG_IMAGE_EFI)
    stp     x0, x1, [sp, #-16]!
    stp     x2, x3, [sp, #-16]!
    adrp    x0, \label
    add     x0, x0, #:lo12:\label
    bl      efi_conout_puts
    ldp     x2, x3, [sp], #16
    ldp     x0, x1, [sp], #16
#endif
.endm

.macro BF2_EFI_HEX label, value
#if defined(CONFIG_IMAGE_EFI)
    stp     x0, x1, [sp, #-16]!
    stp     x2, x3, [sp, #-16]!
    stp     x4, x5, [sp, #-16]!
    adrp    x0, \label
    add     x0, x0, #:lo12:\label
    mov     x1, \value
    bl      efi_conout_put_hex64
    ldp     x4, x5, [sp], #16
    ldp     x2, x3, [sp], #16
    ldp     x0, x1, [sp], #16
#endif
.endm
#else
.macro BF2_EFI_MARKER label
.endm
#endif

.extern main
.extern arm_vector_table
.extern efi_conout_puts
.extern efi_conout_put_hex64

#ifdef CONFIG_PLAT_MLXBF2
BEGIN_FUNC(bf2_install_vector_table)
    adrp    x8, arm_vector_table
    mrs     x9, CurrentEL
    cmp     x9, #(2 << 2)
    b.ne    1f
    msr     vbar_el2, x8
    msr     vbar_el1, x8
    b       2f
1:
    msr     vbar_el1, x8
2:
    isb
    ret
END_FUNC(bf2_install_vector_table)
#endif

.section ".text.start"
BEGIN_FUNC(_start)
    adrp    x19, core_stack_alloc
#ifdef CONFIG_PLAT_MLXBF2
    add     x19, x19, #0x3000
    add     x19, x19, #0xff0
#else
    add     x19, x19, #0xff0
#endif
    mov     sp, x19
#ifdef CONFIG_PLAT_MLXBF2
    /* The BF2 EFI/RShim path can deliver asynchronous exceptions while the
     * elfloader is polling TMFIFO. We do not have interrupt handlers here.
     */
    msr     daifset, #0xf
    isb
#endif
#ifdef CONFIG_PLAT_MLXBF2
#if defined(CONFIG_IMAGE_EFI)
    /* If we were entered as a real EFI application, UEFI boot services are
     * still active. Leave firmware vectors installed until ExitBootServices;
     * otherwise firmware/device IRQs land in the elfloader's fatal trap table.
     * The BF2 Linux-Image compatibility path has no EFI system table here, so
     * install our vectors early as before.
     */
    adrp    x8, __efi_system_table
    add     x8, x8, #:lo12:__efi_system_table
    ldr     x8, [x8]
    cbnz    x8, 4f
#endif
    bl      bf2_install_vector_table
4:
#endif
#ifdef CONFIG_IMAGE_BINARY
    /* Store our original arguments before calling subroutines */
    stp     x0, x1, [sp, #-16]!
    /*
     * Binary images may not be loaded in the correct location.
     * Try and move ourselves so we're in the right place.
     */
    ldr     x0, =IMAGE_START_ADDR
    adrp    x1, _start
    add     x1, x1, #:lo12:_start
    adrp    x2, _end
    add     x2, x2, #:lo12:_end
    mov     x3, #0 /* Arg 4 of fixup_image_base is dtb=NULL */
    bl      fixup_image_base
    mov     x2, x0
    /* fixup_image_base returns 0 if no need to move */
    cmp     x2, #0
    beq     1f
    /* otherwise, jump to the start of the new elfloader */
    /* restore original arguments for restarting at new addr */
    ldp     x0, x1, [sp], #16

    br      x2
1:
    /* restore original arguments for next step */
    ldp     x0, x1, [sp], #16
#endif
    /* EFI normally enters through _gnuefi_start, which relocates this PIE
     * image before efi_main() clears BSS. BF2 RShim BFB live boot enters here
     * as a Linux arm64 Image after UEFI has exited boot services, so do the
     * relocation and BSS clear in the Linux entry path too.
     */
    stp     x0, x1, [sp, #-16]!
#if defined(CONFIG_IMAGE_EFI) && defined(CONFIG_PLAT_MLXBF2)
    adrp    x2, __efi_system_table
    add     x2, x2, #:lo12:__efi_system_table
    ldr     x2, [x2]
    cbnz    x2, .Lbf2_skip_relocate

    mov     x2, x0
    mov     x3, x1
    adr     x0, ImageBase
    adrp    x1, _DYNAMIC
    add     x1, x1, #:lo12:_DYNAMIC
    bl      _relocate
    cbnz    x0, .Lbf2_reloc_failed
.Lbf2_skip_relocate:
    adrp    x2, __efi_system_table
    add     x2, x2, #:lo12:__efi_system_table
    ldr     x2, [x2]
    cbnz    x2, 1f
    bl      clear_bss
1:
#elif !defined(CONFIG_IMAGE_EFI)
    bl      clear_bss
#endif
    ldp     x0, x1, [sp], #16
    b       main

#if defined(CONFIG_IMAGE_EFI) && defined(CONFIG_PLAT_MLXBF2)
.Lbf2_reloc_failed:
    b       abort
#endif
END_FUNC(_start)

/* Move the elf loader out of the kernel's way */
BEGIN_FUNC(finish_relocation)
    /*
     * On aarch64 the kernel is loaded at a very high address:
     * at least above 0x0000ff8080000000. We assume that
     * the ELF loader will never get loaded in a way that overlaps
     * with the kernel, so reaching this function is an error.
     */
    b abort // should never get here!
END_FUNC(finish_relocation)

/* secondary cpu startup */
BEGIN_FUNC(secondary_startup)
    /*
     * secondary_data is a struct that looks like this:
     * 0x0 void *entry
     * 0x8 void *stack
     */
    adrp    x19, secondary_data
    add     x19, x19, #:lo12:secondary_data

    ldr     x0, [x19, #0x8]     // load stack
    mov     sp, x0
    ldr     x1, [x19, #0x0]     // load entry point

    br x1
END_FUNC(secondary_startup)

