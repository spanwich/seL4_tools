/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <autoconf.h>
#include <assembler.h>
#include <armv/assembler.h>

#ifdef CONFIG_ARM_PA_SIZE_BITS_40
#define TCR_PS TCR_PS_1T
#else
#define TCR_PS TCR_PS_16T
#endif

.text

.extern flush_dcache
.extern invalidate_dcache
.extern invalidate_icache
.extern _boot_pgd_down
.extern arm_vector_table
.extern bf2_leave_hyp_after_eret_marker
.extern bf2_leave_hyp_second_el1_marker
.extern bf2_el1_continuation_entry_marker
.extern continue_boot_el1_after_handoff
.extern kernel_info
.extern user_info
.extern dtb
.extern dtb_size
.extern bf2_saved_kernel_info
.extern bf2_saved_user_info
.extern bf2_saved_dtb
.extern bf2_saved_dtb_size

BEGIN_FUNC(disable_caches_hyp)
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    bl      flush_dcache
    disable_id_cache sctlr_el2, x9
    ldp     x29, x30, [sp], #16
    ret
END_FUNC(disable_caches_hyp)

BEGIN_FUNC(leave_hyp)
    /* No interrupt controller is configured in the elfloader. Keep async
     * exceptions masked across the EL2-to-EL1 handoff.
     */
    msr     daifset, #0xf
    isb

    /* We call nested functions, follow the ABI. */
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    bl      flush_dcache

    /* Ensure I-cache, D-cache and mmu are disabled for EL2/Stage1 */
    disable_mmu sctlr_el2, x9

    /*
     * Invalidate the local I-cache so that any instructions fetched
     * speculatively are discarded.
     */
    bl      invalidate_icache

    /* U-Boot-style EL1 handoff state. BF2 UEFI enters the EFI payload at
     * EL2; if we boot a non-hypervisor seL4 kernel, EL1 must have enough
     * architectural state prepared before ERET.
     */
    mrs     x9, cnthctl_el2
    orr     x9, x9, #0x3
    msr     cnthctl_el2, x9
    msr     cntvoff_el2, xzr

    mrs     x9, midr_el1
    mrs     x10, mpidr_el1
    msr     vpidr_el2, x9
    msr     vmpidr_el2, x10

    /* Ensure I-cache, D-cache and mmu are disabled for EL1/Stage2 */
    mov     x9, #(1 << 31)
    msr     hcr_el2, x9

    /* Ensure traps to EL2 are disabled */
    mov     x9, #0x33ff
    msr     cptr_el2, x9
    msr     hstr_el2, xzr
    msr     vttbr_el2, xzr

    mov     x9, #(3 << 20)
    msr     cpacr_el1, x9

    /* Ensure I-cache, D-cache and mmu are disabled for EL1/Stage1 */
    disable_mmu sctlr_el1 , x9

    adrp    x9, arm_vector_table
    msr     vbar_el1, x9

    /* Preserve loaded image metadata across the EL2->EL1 handoff in registers.
     * On BF2 EFI boot, dirty data written shortly before this point can be
     * observed as original image contents after changing EL/cache state.
     */
    /* Direct (non-GOT) addressing: the elfloader is linked -static -fno-pic,
     * so there is no GOT; GOT-relative relocs leave these as undefined refs at
     * link time. adrp+:lo12: matches arm_vector_table above.
     *
     * The bf2_saved_* shadow copies only exist on CONFIG_PLAT_MLXBF2 (they work
     * around a BF2 EFI cache-coherency quirk). On every other platform use the
     * live image-info globals, which are valid at this point. */
#ifdef CONFIG_PLAT_MLXBF2
#define MK_KINFO   bf2_saved_kernel_info
#define MK_UINFO   bf2_saved_user_info
#define MK_DTB     bf2_saved_dtb
#define MK_DTBSZ   bf2_saved_dtb_size
#else
#define MK_KINFO   kernel_info
#define MK_UINFO   user_info
#define MK_DTB     dtb
#define MK_DTBSZ   dtb_size
#endif
    adrp    x11, MK_KINFO
    add     x11, x11, #:lo12:MK_KINFO
    ldp     x0, x1, [x11, #0]
    ldp     x2, x3, [x11, #16]
    ldp     x4, x5, [x11, #32]

    adrp    x12, MK_UINFO
    add     x12, x12, #:lo12:MK_UINFO
    ldp     x6, x7, [x12, #0]
    ldp     x19, x20, [x12, #16]
    ldp     x21, x22, [x12, #32]

    adrp    x13, MK_DTB
    add     x13, x13, #:lo12:MK_DTB
    ldr     x23, [x13]
    adrp    x14, MK_DTBSZ
    add     x14, x14, #:lo12:MK_DTBSZ
    ldr     x24, [x14]

    mov     x9, #(PSR_F_BIT | PSR_I_BIT | PSR_A_BIT | PSR_D_BIT | PSR_MODE_EL1h)
    msr     spsr_el2, x9

    /* Let the caller use our stack, in case it needs to pop something. */
    ldp     x29, x30, [sp], #16
    mov     x10, sp
    msr     sp_el1, x10
    adr     x11, .Lleave_hyp_after_eret
    msr     elr_el2, x11
    eret

.Lleave_hyp_after_eret:
    /* Restore the metadata from registers now that we are executing at EL1,
     * before calling any C helpers that may clobber caller-saved registers.
     */
    adrp    x11, :got:kernel_info
    ldr     x11, [x11, #:got_lo12:kernel_info]
    stp     x0, x1, [x11, #0]
    stp     x2, x3, [x11, #16]
    stp     x4, x5, [x11, #32]

    adrp    x12, :got:user_info
    ldr     x12, [x12, #:got_lo12:user_info]
    stp     x6, x7, [x12, #0]
    stp     x19, x20, [x12, #16]
    stp     x21, x22, [x12, #32]

    adrp    x13, :got:dtb
    ldr     x13, [x13, #:got_lo12:dtb]
    str     x23, [x13]
    adrp    x14, :got:dtb_size
    ldr     x14, [x14, #:got_lo12:dtb_size]
    str     x24, [x14]

    bl      bf2_leave_hyp_after_eret_marker
    bl      bf2_leave_hyp_second_el1_marker
    bl      bf2_el1_continuation_entry_marker

    /* Do not return through the EL2 caller's frame after ERET. Continue the
     * boot path as a fresh EL1-only call; this should not return.
     */
    bl      continue_boot_el1_after_handoff
1:
    wfe
    b       1b
END_FUNC(leave_hyp)

BEGIN_FUNC(arm_enable_hyp_mmu)
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    bl      flush_dcache

    disable_mmu sctlr_el2, x8

    bl      invalidate_icache

    /*
     *   DEVICE_nGnRnE      000     00000000
     *   DEVICE_nGnRE       001     00000100
     *   DEVICE_GRE         010     00001100
     *   NORMAL_NC          011     01000100
     *   NORMAL             100     11111111
     *   NORMAL_WT          101     10101010
     */
    ldr     x5, =MAIR(0x00, MT_DEVICE_nGnRnE) | \
                 MAIR(0x04, MT_DEVICE_nGnRE) | \
                 MAIR(0x0c, MT_DEVICE_GRE) | \
                 MAIR(0x44, MT_NORMAL_NC) | \
                 MAIR(0xff, MT_NORMAL) | \
                 MAIR(0xaa, MT_NORMAL_WT)
    msr     mair_el2, x5
    ldr     x8, =TCR_T0SZ(48) | TCR_IRGN0_WBWC | TCR_ORGN0_WBWC | TCR_SH0_ISH | TCR_TG0_4K | TCR_PS | TCR_EL2_RES1
    msr     tcr_el2, x8
    isb

    adrp    x8, _boot_pgd_down
    msr     ttbr0_el2, x8
    isb

    tlbi    alle2is
    dsb     ish
    isb

    enable_mmu  sctlr_el2, x8
    ic  ialluis
    dsb ish
    isb
    tlbi    alle2is
    dsb     ish
    isb
    ldp     x29, x30, [sp], #16
    ret
END_FUNC(arm_enable_hyp_mmu)

/*
 * Secondary-core variant of arm_enable_hyp_mmu for multikernel bring-up.
 *
 * Identical to arm_enable_hyp_mmu EXCEPT it avoids whole-cache maintenance
 * on a prepared secondary core: no set/way D-cache clean/invalidate and no
 * global I-cache invalidate. On STM32MP25x, non-secure whole-cache maintenance
 * can expose secure TF-A/OP-TEE SYSRAM lines to RISAB (IADDR in the
 * 0x0E00_0000 bank). The handoff contract is that the primary loader already
 * cleaned the K1 image, DTB, rootserver, trampoline, and boot page-table state
 * to PoC before CPU_ON, so this path only orders those writes with barriers.
 */
BEGIN_FUNC(arm_enable_hyp_mmu_secondary)
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    dsb     sy
    isb

    disable_mmu sctlr_el2, x8

    /* Prepared secondary: avoid global ic iallu before MMU enable. */
    dsb     sy
    isb

    ldr     x5, =MAIR(0x00, MT_DEVICE_nGnRnE) | \
                 MAIR(0x04, MT_DEVICE_nGnRE) | \
                 MAIR(0x0c, MT_DEVICE_GRE) | \
                 MAIR(0x44, MT_NORMAL_NC) | \
                 MAIR(0xff, MT_NORMAL) | \
                 MAIR(0xaa, MT_NORMAL_WT)
    msr     mair_el2, x5
    ldr     x8, =TCR_T0SZ(48) | TCR_IRGN0_WBWC | TCR_ORGN0_WBWC | TCR_SH0_ISH | TCR_TG0_4K | TCR_PS | TCR_EL2_RES1
    msr     tcr_el2, x8
    isb

    adrp    x8, _boot_pgd_down
    msr     ttbr0_el2, x8
    isb

    tlbi    alle2is
    dsb     ish
    isb

    enable_mmu  sctlr_el2, x8
    /* Prepared secondary: avoid global ic ialluis after MMU enable. */
    dsb     ish
    isb
    tlbi    alle2is
    dsb     ish
    isb
    ldp     x29, x30, [sp], #16
    ret
END_FUNC(arm_enable_hyp_mmu_secondary)
