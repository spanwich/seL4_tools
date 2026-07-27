/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <autoconf.h>
#include <elfloader/gen_config.h>
#if defined(CONFIG_IMAGE_BINARY) || defined(CONFIG_IMAGE_BOOTI)
#include <image_start_addr.h>
#endif

#include <assembler.h>
#include <mode/assembler.h>

.extern main

.section ".text.start"

#ifdef CONFIG_IMAGE_BOOTI
/*
 * ARM64 Image header for U-Boot 'booti' command.
 * This 64-byte header must be at the start of the binary.
 * See Linux Documentation/arm64/booting.rst.
 *
 * The linker places .text.start at IMAGE_START_ADDR, so the header
 * occupies IMAGE_START_ADDR..+63 and _start begins at IMAGE_START_ADDR+64.
 */
    b       _start              /* code0: branch past header to entry   */
    .long   0                   /* code1: reserved                      */
    .quad   0                   /* text_offset: 0 (position independent)*/
    .quad   _image_size         /* image_size: full footprint incl. BSS */
    .quad   0xa                 /* flags: LE, 4K pages, any load addr   */
    .quad   0                   /* res2                                 */
    .quad   0                   /* res3                                 */
    .quad   0                   /* res4                                 */
    .ascii  "ARM\x64"           /* magic: 0x644d5241                    */
    .long   0                   /* res5: PE/COFF offset (not EFI)       */
#endif

BEGIN_FUNC(_start)
    /* Make sure interrupts are disabled */
    msr daifset, #DAIFSET_MASK

    adrp    x19, core_stack_alloc
    add     x19, x19, #0xff0
    mov     sp, x19
#if defined(CONFIG_IMAGE_BINARY) || defined(CONFIG_IMAGE_BOOTI)
    /* Store our original arguments before calling subroutines */
    stp     x0, x1, [sp, #-16]!
    /*
     * Binary/booti images may not be loaded in the correct location.
     * Try and move ourselves so we're in the right place.
     */
#ifdef CONFIG_IMAGE_BOOTI
    /* For booti, the 64-byte ARM64 Image header precedes _start in
     * .text.start. The linker places .text.start at IMAGE_START_ADDR,
     * so _start is linked at IMAGE_START_ADDR + 64. */
    ldr     x0, =(IMAGE_START_ADDR + 64)
#else
    ldr     x0, =IMAGE_START_ADDR
#endif
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
#ifndef CONFIG_IMAGE_EFI
    /* Clear .bss section before calling main.
     * BSS is not included in the binary payload (objcopy -O binary strips
     * NOBITS sections), so it must be zeroed explicitly. This matches the
     * AArch32 crt0.S behaviour which clears BSS for all non-EFI image types.
     */
    stp     x0, x1, [sp, #-16]!
    bl      clear_bss
    ldp     x0, x1, [sp], #16
#endif
    b       main
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
    /* Make sure interrupts are disabled */
    msr daifset, #DAIFSET_MASK

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
