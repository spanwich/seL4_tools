/*
 * Copyright 2026, PhD Research Project — multikernel-AMP
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Multikernel-AMP support: load and dispatch N kernel+rootserver pairs
 * across the ARM cluster's cores, one kernel per core, each kernel a
 * verified unicore seL4 instance built at a distinct KernelArmRamBase.
 *
 * MVP-Q scope: N=2 (kernel_0 on core 0, kernel_1 on core 1, both AArch64).
 */

#pragma once

#ifdef CONFIG_MULTIKERNEL

#include <elfloader.h>
#include <types.h>

/* Per-kernel descriptor: everything the trampoline needs to bring a kernel up
 * from PSCI CPU_ON entry to the kernel's _start.
 *
 * NB: under standard EL2 (no VHE), only TTBR0_EL2 exists. With TCR_T0SZ=48 the
 * page table covers the full 48-bit VA space — kernel high-half mappings
 * (0x80_xxxx_xxxx) and user low-half (0x0..) both live in the same TTBR0
 * tree. Same convention as elfloader's own arm_enable_hyp_mmu().
 */
struct multikernel_entry {
    /* Identity-mapped boot page-table root for this kernel.
     * Set up by elfloader before issuing PSCI CPU_ON. */
    paddr_t ttbr0_phys;
    /* Kernel _start virtual address (high-half, e.g. 0x8060000000) */
    vaddr_t kernel_entry_vaddr;
    /* init_kernel() args per the seL4 ABI in head.S:127-134:
     *   x0: user image phys start, x1: user image phys end,
     *   x2: phys/virt offset, x3: user virt entry, x4: dtb paddr, x5: dtb size */
    paddr_t user_phys_start;
    paddr_t user_phys_end;
    word_t  phys_virt_offset;
    vaddr_t user_virt_entry;
    paddr_t dtb_paddr;
    size_t  dtb_size;
};

/* For MVP-Q we statically allocate two slots — one per A35 core. */
#define MULTIKERNEL_MAX_KERNELS 2

extern struct multikernel_entry multikernel_entries[MULTIKERNEL_MAX_KERNELS];

/* Implemented in arch-arm/multikernel.c — load both kernel+rootserver pairs
 * from the bundled CPIO and populate multikernel_entries[]. */
int multikernel_load_all(void);

/* Implemented in arch-arm/multikernel.c — for each non-boot kernel, set up
 * its identity-mapped page tables and PSCI CPU_ON the matching core with
 * the trampoline as entry. Boot kernel is dispatched via the existing path. */
int multikernel_dispatch_secondaries(void);

/* Implemented in arch-arm/64/multikernel_trampoline.S — runs at NSEC EL2 with
 * MMU OFF immediately after PSCI CPU_ON. Loads TTBR0/1 from the per-kernel
 * struct, TLBI + DSB + ISB, sets x0–x5, then branches to kernel_entry_vaddr.
 * The kernel's own _start enables MMU via SCTLR write at line 94 of head.S.
 *
 * Entry point parameter: x0 = MPIDR.AFF0 (logical core id); used to index
 * multikernel_entries[]. */
extern void multikernel_secondary_startup(void);

#endif /* CONFIG_MULTIKERNEL */
