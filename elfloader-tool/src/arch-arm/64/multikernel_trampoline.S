/*
 * Copyright 2026, PhD Research Project -- multikernel-AMP
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <autoconf.h>
#include <elfloader/gen_config.h>

#ifdef CONFIG_MULTIKERNEL

.section .text

#define MK_ENTRY_SIZE_BITS  6
#define MK_OFF_KENTRY       8
#define MK_OFF_X0          16
#define MK_OFF_X1          24
#define MK_OFF_X2          32
#define MK_OFF_X3          40
#define MK_OFF_X4          48
#define MK_OFF_X5          56
#define MK_STACK_BITS      12
#define BF2_SHARED_READY   4

#define SCTLR_M            0x1
#define SCTLR_C            0x4
#define SCTLR_I            0x1000
#define HCR_RW             0x80000000
#define CPTR_EL2_RES1      0x33ff
#define CPACR_FPEN         0x300000
#define SPSR_EL1H_MASKED   0x3c5

.extern arm_enable_mmu
.extern arm_vector_table
.extern mk_secondary_stack
.extern multikernel_entries

.global multikernel_secondary_startup
.type multikernel_secondary_startup, %function
multikernel_secondary_startup:
    /* x0 is the PSCI context id and equals the multikernel slot index. */
    mov     x19, x0

    msr     daifset, #0xf
    isb

    /* Set a scratch stack before doing any calls or EL handoff. */
    adrp    x10, mk_secondary_stack
    add     x10, x10, :lo12:mk_secondary_stack
    add     x10, x10, x19, lsl #MK_STACK_BITS
    add     x10, x10, #(1 << MK_STACK_BITS)
    mov     sp, x10
    msr     sp_el1, x10

#ifdef CONFIG_PLAT_MLXBF2
    /* BF2 enters secondary kernels through an EFI/EL2 path and needs a
     * TMFIFO console ownership gate. The shared page is BF2-specific and is
     * invalid on STM32MP25x, where TF-A PSCI performs the CPU1 handoff. */
    cbz     x19, 2f
    movz    x11, #0x7fff, lsl #16
    movk    x11, #0x8, lsl #32
1:
    ldr     w12, [x11, #BF2_SHARED_READY]
    cmp     w12, #1
    b.eq    2f
    wfe
    b       1b
2:
#endif

    mrs     x9, CurrentEL
    cmp     x9, #(2 << 2)
    b.eq    .Lfrom_el2

.Lfrom_el1:
    mov     x9, #CPACR_FPEN
    msr     cpacr_el1, x9

    mrs     x9, sctlr_el1
    bic     x9, x9, #SCTLR_M
    bic     x9, x9, #SCTLR_C
    bic     x9, x9, #SCTLR_I
    msr     sctlr_el1, x9
    isb

    adrp    x9, arm_vector_table
    add     x9, x9, :lo12:arm_vector_table
    msr     vbar_el1, x9
    b       .Lsecondary_el1

.Lfrom_el2:
    /* BF2-style EL2 handoff path for firmware that enters the trampoline at EL2. */
    mrs     x9, cnthctl_el2
    orr     x9, x9, #0x3
    msr     cnthctl_el2, x9
    msr     cntvoff_el2, xzr

    mrs     x9, midr_el1
    mrs     x10, mpidr_el1
    msr     vpidr_el2, x9
    msr     vmpidr_el2, x10

    mov     x9, #HCR_RW
    msr     hcr_el2, x9
    mov     x9, #CPTR_EL2_RES1
    msr     cptr_el2, x9
    msr     hstr_el2, xzr
    msr     vttbr_el2, xzr

    mov     x9, #CPACR_FPEN
    msr     cpacr_el1, x9

    mrs     x9, sctlr_el1
    bic     x9, x9, #SCTLR_M
    bic     x9, x9, #SCTLR_C
    bic     x9, x9, #SCTLR_I
    msr     sctlr_el1, x9
    isb

    adrp    x9, arm_vector_table
    add     x9, x9, :lo12:arm_vector_table
    msr     vbar_el1, x9

    mov     x9, #SPSR_EL1H_MASKED
    msr     spsr_el2, x9
    adr     x9, .Lsecondary_el1
    msr     elr_el2, x9
    eret

.Lsecondary_el1:
    msr     daifset, #0xf
    isb

    /* Enable EL1 MMU using the shared boot tables prepared by K0 elfloader. */
    bl      arm_enable_mmu

    adrp    x9, multikernel_entries
    add     x9, x9, :lo12:multikernel_entries
    add     x9, x9, x19, lsl #MK_ENTRY_SIZE_BITS

    ldr     x12, [x9, #MK_OFF_KENTRY]
    ldr     x0,  [x9, #MK_OFF_X0]
    ldr     x1,  [x9, #MK_OFF_X1]
    ldr     x2,  [x9, #MK_OFF_X2]
    ldr     x3,  [x9, #MK_OFF_X3]
    ldr     x4,  [x9, #MK_OFF_X4]
    ldr     x5,  [x9, #MK_OFF_X5]
    br      x12

1:
    wfe
    b       1b

.size multikernel_secondary_startup, . - multikernel_secondary_startup

#endif /* CONFIG_MULTIKERNEL */
