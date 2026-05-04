/*
 * Copyright 2026, PhD Research Project — multikernel-AMP
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Per-kernel MMU trampoline for multikernel-AMP boot of secondary cores.
 *
 * Entry contract (from PSCI CPU_ON on STM32MP25x and qemu-arm-virt):
 *   - MMU OFF, TTBRs undefined
 *   - I-cache/D-cache state: undefined (re-enabled in seL4 _start)
 *   - Exception level: NSEC EL2 (hypervisor build)
 *   - x0 = ctx_id (the user-supplied PSCI CPU_ON ctx) — multikernel_entries
 *         index for this core (= 1 for MVP-Q on core 1)
 *   - x1–x7: 0
 *   - Stack: not provided — we set one up from mk_secondary_stack[].
 *
 * What this trampoline does:
 *   1. Set up a 4 KiB scratch stack (mk_secondary_stack[ctx_id]) for use by
 *      arm_enable_hyp_mmu, which calls flush_dcache / invalidate_icache.
 *   2. Call arm_enable_hyp_mmu — sets MAIR_EL2, TCR_EL2, TTBR0_EL2 = _boot_pgd_down,
 *      then writes SCTLR_EL2 to enable the MMU. _boot_pgd_down already covers
 *      both kernels' phys regions (set up by init_hyp_boot_vspace on the boot
 *      core for kernel_0; the contiguous 2 MiB PMD fill happens to map
 *      kernel_1's high-half too, since both kernels share the same PUD slot).
 *   3. Read multikernel_entries[ctx_id] for kernel_N's _start vaddr and
 *      init_kernel() ABI args (x0–x5).
 *   4. Branch to kernel _start. seL4's _start at line 94 of head.S re-writes
 *      SCTLR_EL2 with finer cache-bit control, keeping the MMU on.
 */

#include <autoconf.h>
#include <elfloader/gen_config.h>

#ifdef CONFIG_MULTIKERNEL

.section .text

/* struct multikernel_entry layout — must match include/multikernel.h. */
#define MK_ENTRY_SIZE_BITS  6  /* 64 bytes per slot, exactly 1<<6 */
#define MK_OFF_TTBR0        0
#define MK_OFF_KENTRY       8
#define MK_OFF_X0          16  /* user_phys_start */
#define MK_OFF_X1          24  /* user_phys_end */
#define MK_OFF_X2          32  /* phys_virt_offset */
#define MK_OFF_X3          40  /* user_virt_entry */
#define MK_OFF_X4          48  /* dtb_paddr */
#define MK_OFF_X5          56  /* dtb_size */

/* mk_secondary_stack: 4 KiB per core, declared in arch-arm/multikernel.c */
#define MK_STACK_BITS  12  /* 4096 bytes per core */

.global multikernel_secondary_startup
.type multikernel_secondary_startup, %function
multikernel_secondary_startup:
    /* Save ctx_id (our slot index) in a callee-saved-ish register before
     * we clobber x0 by setting up the stack. x19 survives function calls. */
    mov     x19, x0

    /* Set up scratch stack: sp = &mk_secondary_stack[ctx_id+1] (top of slot). */
    adrp    x10, mk_secondary_stack
    add     x10, x10, :lo12:mk_secondary_stack
    add     x10, x10, x19, lsl #MK_STACK_BITS
    add     x10, x10, #(1 << MK_STACK_BITS)   /* sp = top of slot */
    mov     sp, x10

    /* Bring up MMU using the shared boot page tables (_boot_pgd_down).
     * arm_enable_hyp_mmu sets MAIR/TCR/TTBR0_EL2 + SCTLR_EL2.M=1. */
    bl      arm_enable_hyp_mmu

    /* MMU is now on. Compute &multikernel_entries[ctx_id] in x9. */
    adrp    x9, multikernel_entries
    add     x9, x9, :lo12:multikernel_entries
    add     x9, x9, x19, lsl #MK_ENTRY_SIZE_BITS

    /* Load init_kernel() args per seL4 ABI (head.S:127-134):
     *   x0: user image phys start
     *   x1: user image phys end
     *   x2: phys/virt offset
     *   x3: user image virt entry
     *   x4: DTB phys addr
     *   x5: DTB size */
    ldr     x12, [x9, #MK_OFF_KENTRY]   /* save kernel _start vaddr */
    ldr     x0,  [x9, #MK_OFF_X0]
    ldr     x1,  [x9, #MK_OFF_X1]
    ldr     x2,  [x9, #MK_OFF_X2]
    ldr     x3,  [x9, #MK_OFF_X3]
    ldr     x4,  [x9, #MK_OFF_X4]
    ldr     x5,  [x9, #MK_OFF_X5]

    /* Branch to kernel _start (now reachable as a virtual address). */
    br      x12

.size multikernel_secondary_startup, . - multikernel_secondary_startup

#endif /* CONFIG_MULTIKERNEL */
