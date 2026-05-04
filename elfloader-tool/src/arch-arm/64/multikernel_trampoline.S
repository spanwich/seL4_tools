/*
 * Copyright 2026, PhD Research Project — multikernel-AMP
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Per-kernel MMU trampoline for multikernel-AMP boot of secondary cores.
 *
 * Entry contract (from PSCI CPU_ON on STM32MP25x and qemu-arm-virt):
 *   - MMU OFF, TTBRs undefined
 *   - I-cache/D-cache state: undefined (we re-enable in seL4 _start)
 *   - Exception level: NSEC EL2 (hypervisor build)
 *   - x0 = context_id (the user-supplied PSCI CPU_ON ctx) — we use this as
 *         the multikernel_entries[] index (= MPIDR.AFF0 of this core)
 *   - x1–x7: 0
 *   - Stack: not provided (we don't need one before branching to _start)
 *
 * What this trampoline does:
 *   1. Read multikernel_entries[x0] to find this kernel's TTBR0_EL2, its
 *      _start virtual address, and its init_kernel() args.
 *   2. Load TTBR0_EL2 with the kernel's identity-mapped boot pgd.
 *      (EL2 without VHE has no TTBR1_EL2 — TCR_T0SZ=48 covers full 48-bit VA.)
 *   3. TLBI alle2is + DSB ish + ISB to flush the previous translation regime.
 *   4. Set x0–x5 per the seL4 ABI (head.S:127-134).
 *   5. Branch to kernel_entry_vaddr. seL4's _start at line 94 of head.S then
 *      writes SCTLR_EL2 to enable the MMU with our preloaded TTBR0_EL2.
 *
 * Status: MVP-Q stub — assembly framework only. Slot loading and full ABI
 * setup land alongside the C-side multikernel_load_all() implementation.
 */

#include <autoconf.h>
#include <elfloader/gen_config.h>

#ifdef CONFIG_MULTIKERNEL

.section .text

/* struct multikernel_entry layout — must match include/multikernel.h:
 *   0: ttbr0_phys           (8 bytes)
 *   8: kernel_entry_vaddr   (8 bytes)
 *  16: user_phys_start      (8 bytes)
 *  24: user_phys_end        (8 bytes)
 *  32: phys_virt_offset     (8 bytes)
 *  40: user_virt_entry      (8 bytes)
 *  48: dtb_paddr            (8 bytes)
 *  56: dtb_size             (8 bytes)
 * Total per slot: 64 bytes — exactly 1<<6. */
#define MK_ENTRY_SIZE_BITS  6  /* 64 bytes */
#define MK_OFF_TTBR0        0
#define MK_OFF_KENTRY       8
#define MK_OFF_X0          16  /* user_phys_start */
#define MK_OFF_X1          24  /* user_phys_end */
#define MK_OFF_X2          32  /* phys_virt_offset */
#define MK_OFF_X3          40  /* user_virt_entry */
#define MK_OFF_X4          48  /* dtb_paddr */
#define MK_OFF_X5          56  /* dtb_size */

.global multikernel_secondary_startup
.type multikernel_secondary_startup, %function
multikernel_secondary_startup:
    /* x0 holds our multikernel_entries[] index from PSCI ctx_id.
     * Compute &multikernel_entries[x0] in x9. */
    adrp    x9, multikernel_entries
    add     x9, x9, :lo12:multikernel_entries
    add     x9, x9, x0, lsl #MK_ENTRY_SIZE_BITS

    /* Load TTBR0_EL2 first while we still have unfettered register access.
     * (EL2 without VHE has no TTBR1_EL2.) */
    ldr     x10, [x9, #MK_OFF_TTBR0]
    msr     ttbr0_el2, x10

    /* Flush stale TLB entries from whatever EL2 state TF-A/elfloader left
     * behind on the boot core. We're targeting EL2 since the kernel uses
     * sctlr_el2 under hypervisor support (head.S:73-76). */
    tlbi    alle2is
    dsb     ish
    isb

    /* Save kernel _start vaddr in a callee-safe register before clobbering x0–x5. */
    ldr     x12, [x9, #MK_OFF_KENTRY]

    /* Load init_kernel() args per seL4 ABI (head.S:127-134):
     *   x0: user image phys start
     *   x1: user image phys end
     *   x2: phys/virt offset
     *   x3: user image virt entry
     *   x4: DTB phys addr
     *   x5: DTB size */
    ldr     x0, [x9, #MK_OFF_X0]
    ldr     x1, [x9, #MK_OFF_X1]
    ldr     x2, [x9, #MK_OFF_X2]
    ldr     x3, [x9, #MK_OFF_X3]
    ldr     x4, [x9, #MK_OFF_X4]
    ldr     x5, [x9, #MK_OFF_X5]

    /* Branch to kernel _start. MMU is still OFF; seL4's _start enables it
     * via msr SCTLR_EL2 at head.S:94 with our preloaded TTBRs. The branch
     * target lives in identity-mapped phys memory until SCTLR.M flips, so
     * the next instruction fetch after the MMU-enable msr remains valid. */
    br      x12

.size multikernel_secondary_startup, . - multikernel_secondary_startup

#endif /* CONFIG_MULTIKERNEL */
