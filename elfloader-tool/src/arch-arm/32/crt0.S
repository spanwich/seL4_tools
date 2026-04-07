/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <autoconf.h>
#include <elfloader/gen_config.h>
#include <assembler.h>
#include <armv/assembler.h>

.extern main
.extern non_boot_main

.section ".text.start"

BEGIN_FUNC(_start)
#ifdef CONFIG_ELFLOADER_ARMV8_LEAVE_AARCH64
    /* Include an Aarch64 preamble that switches to aarch32 */
    .incbin "crt0_64.bin"
BEGIN_FUNC(_start32)
#endif // CONFIG_ELFLOADER_ARMV8_LEAVE_AARCH64

#ifdef CONFIG_PLAT_BCM2837
    /*
     * BCM2837 boot debugging: single-char UART output at each stage.
     * PL011 UART at 0x3F201000 is already initialized by GPU firmware.
     * Debug sequence: A=entry, B=cpu0, C=dtb_saved, D=cpsid,
     *                 h=hyp_exit, F=stack, G=bss, H=main
     *
     * IMPORTANT: Must poll UARTFR (base+0x18) bit 5 (TXFF) before each
     * write to UARTDR, otherwise PL011 silently drops characters.
     *
     * WARNING: This is BCM2837-specific. The hardcoded UART address
     * (0x3F201000) causes infinite spin on other platforms (e.g. QEMU virt
     * where PL011 is at 0x09000000).
     */

/* Macro: poll TX FIFO then write char to PL011 UART */
.macro uart_putc char
    ldr r3, =0x3F201000
1001:
    ldr r4, [r3, #0x18]        /* Read UARTFR */
    tst r4, #0x20              /* Test TXFF (bit 5) */
    bne 1001b                  /* Spin if TX FIFO full */
    mov r4, #\char
    str r4, [r3]               /* Write to UARTDR */
.endm

    /* --- DEBUG 'A': Entry point reached --- */
    uart_putc 0x41

    /* Park secondary CPUs IMMEDIATELY (before ANY other code).
     * BCM2837 starts all 4 cores at 0x8000 simultaneously. */
    mrc p15, 0, r5, c0, c0, 5  /* Read MPIDR */
    ands r5, r5, #3             /* Extract CPU ID */
    bne _cpu_park               /* If not CPU0, park forever */

    /* --- DEBUG 'B': CPU0 confirmed --- */
    uart_putc 0x42
#endif /* CONFIG_PLAT_BCM2837 */

    /* If we're not booting under EFI, r2 will contain a pointer to the DTB */
    mov r0, r2

#ifdef CONFIG_PLAT_BCM2837
    /* --- DEBUG 'C': DTB pointer saved --- */
    uart_putc 0x43

    /* Check if we're in HYP mode (BCM2837 firmware boots in HYP).
     * Must handle this BEFORE cpsid which may behave differently in HYP. */
    mrs r5, cpsr
    and r5, r5, #0x1F
    cmp r5, #0x1A               /* HYP mode = 0x1A */
    bne _not_hyp

    /* --- DEBUG 'h': In HYP mode, exiting --- */
    uart_putc 0x68

    /* Drop from HYP to SVC mode via eret */
    mov r5, #0xD3               /* SVC mode (0x13) + IRQ/FIQ/Abort disabled */
    msr spsr_hyp, r5
    adr lr, _hyp_exit
    msr elr_hyp, lr
    eret

_hyp_exit:
_not_hyp:
#endif /* CONFIG_PLAT_BCM2837 */

    /* Disable Async aborts that might be pending from bootloader */
    cpsid   ifa

#ifdef CONFIG_PLAT_BCM2837
    /* --- DEBUG 'D': Interrupts disabled --- */
    uart_putc 0x44
#endif /* CONFIG_PLAT_BCM2837 */

    /* r0 contains the FDT address from the bootloader */
#if CONFIG_MAX_NUM_NODES > 1
    /* Enable SMP */
    mrc     ACTLR(r1)
    orr     r1, r1, #(1 << 6)       /* enable SMP bit   */
#ifdef CONFIG_ARM_CORTEX_A9
    orr     r1, r1, #1              /* enable FW bit    */
#endif
    mcr     ACTLR(r1)
#endif /* CONFIG_MAX_NUM_NODES > 1 */

    ldr sp, =core_stack_alloc + 0xff0

#ifdef CONFIG_PLAT_BCM2837
    /* --- DEBUG 'F': Stack set up --- */
    uart_putc 0x46
#endif

#ifndef CONFIG_IMAGE_EFI
    /*
     * clear the BSS section, since it may not be clear yet.
     * Under EFI, this is done earlier in the boot process.
     */
    push    {r0}
    bl      clear_bss
    pop     {r0}
#endif

#ifdef CONFIG_PLAT_BCM2837
    /* --- DEBUG 'G': BSS cleared --- */
    uart_putc 0x47

    /* --- DEBUG 'H': Jumping to main --- */
    uart_putc 0x48
#endif

    b       main

#ifdef CONFIG_PLAT_BCM2837
    /* Park secondary CPUs (BCM2837 boots all 4 cores) */
_cpu_park:
    wfi
    b       _cpu_park
#endif
END_FUNC(_start)

/* Move the elf loader out of the kernel's way */
BEGIN_FUNC(finish_relocation)
#if CONFIG_IMAGE_EFI
    // r0 = offset between current base address and new base address
    // r1 = _DYNAMIC pointer (current base address)
    // r2 = total offset (new_base)
    mov r8, r1 // move to r8

    // figure out relocated address of continue_boot
    adr r1, LC2
    ldmia r1, {r3, r4, r5}

    // r3 = stack pointer (in current program)
    // r4 = continue_boot (in current program)
    // r5 = current base address (_text, the start of the binary)

    sub sp, r3, r0                  // point sp to new stack
    sub r4, r4, r0                  // continue_boot address in relocated elfloader

    // self_reloc expects:
    // r0 = current base address
    // r1 = dynamic section pointer
    // r2 = new base address
    // r3 = 0

    mov r0, r5                      // current base address
    mov r1, r8                      // put _DYNAMIC back
    // r2 is new base address, passed from relocate_below_kernel()
    mov r3, #0                      // NULL r3

    push {r4}                       // save continue_boot address

    bl _relocate

    pop {r4}
    mov r0, #1
    blx r4
    b abort // should never get here!
#else
    /* On non-EFI, the image cannot be relocated.
     * If you hit this, you probably need to modify the ELF loader
     * load address.
     */
    mov pc, lr
#endif
END_FUNC(finish_relocation)
.align 2
.type LC2, #object
LC2:
.word core_stack_alloc + 0xff0  // r3
.word continue_boot             // r4
.word _text                     // r5
.size LC2, . - LC2

#if CONFIG_MAX_NUM_NODES > 1
BEGIN_FUNC(secondary_startup)
    /* Invalidate caches before proceeding... */
    mov     r0, #0
    mcr     IIALL(r0)
    dcache  isw

    /* Disable Async aborts that might be pending from bootloader */
    cpsid   ifa

    /* Enable SMP */
    mrc     ACTLR(r0)
    orr     r0, r0, #(1 << 6)       /* enable SMP bit   */
#ifdef CONFIG_ARM_CORTEX_A9
    orr     r0, r0, #1              /* enable FW bit    */
#endif
    mcr     ACTLR(r0)

    /*
     * secondary_data is a struct like this:
     * 0x0 void *entry
     * 0x4 void *stack
     */
    ldr     r0, =secondary_data
    ldr     r1, [r0, #0x4]         /* load stack */
    mov     sp, r1

    ldr     r2, [r0]               /* load entry point */

    /* core_entry expects sp as its first argument */
    mov     r0, r1
    bx r2
END_FUNC(secondary_startup)
#endif /* CONFIG_MAX_NUM_NODES */

/*
 * Symbols required for libgcc.
 */
.global raise
.global __aeabi_unwind_cpp_pr0
.global __aeabi_unwind_cpp_pr1
.global __aeabi_unwind_cpp_pr2
raise:
__aeabi_unwind_cpp_pr0:
__aeabi_unwind_cpp_pr1:
__aeabi_unwind_cpp_pr2:
    b       raise
