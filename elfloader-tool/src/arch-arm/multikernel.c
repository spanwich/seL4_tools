/*
 * Copyright 2026, PhD Research Project — multikernel-AMP
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Multikernel-AMP boot dispatch: orchestrates loading and starting two
 * independent seL4 kernels on the ARM cluster's two cores.
 *
 * Each kernel is a verified unicore seL4 instance built at its own
 * KernelArmRamBase. The CPIO archive bundled in this elfloader contains:
 *   - kernel_0.elf, rootserver_0, kernel_0.dtb  (for core 0)
 *   - kernel_1.elf, rootserver_1, kernel_1.dtb  (for core 1)
 *
 * Boot order:
 *   1. Boot CPU (core 0) runs the elfloader (this code).
 *   2. multikernel_load_all() places both kernel+rootserver+dtb sets at
 *      their declared physical addresses.
 *   3. multikernel_dispatch_secondaries() sets up kernel_1's identity-mapped
 *      page tables, populates multikernel_entries[1], and PSCI CPU_ONs core 1
 *      with multikernel_secondary_startup as the entry point.
 *   4. Core 0 then jumps to kernel_0's _start via the existing single-kernel
 *      path (with multikernel_entries[0] data).
 *   5. Core 1's PSCI handoff lands in multikernel_secondary_startup, which
 *      loads kernel_1's TTBR0/1 and branches to kernel_1's _start.
 *
 * Status: MVP-Q stub — not yet wired into sys_boot.c. Provides the shape of
 * the per-kernel descriptor and the dispatch ABI; CPIO bundling and the
 * actual load path land in a follow-up.
 */

#include <autoconf.h>
#include <elfloader/gen_config.h>

#ifdef CONFIG_MULTIKERNEL

#include <multikernel.h>
#include <printf.h>
#include <abort.h>

struct multikernel_entry multikernel_entries[MULTIKERNEL_MAX_KERNELS];

int multikernel_load_all(void)
{
    /* TODO: read kernel_0/rootserver_0/kernel_0.dtb and kernel_1/rootserver_1/
     * kernel_1.dtb from the CPIO archive (replacing common.c:load_images for
     * this build). Populate multikernel_entries[0..1] with phys addrs, virt
     * entries, and dtb paddr/size. Set up identity-mapped page tables for
     * each kernel (one TTBR0/TTBR1 pair per kernel). */
    printf("multikernel: load_all() — not implemented yet\n");
    abort();
    return -1;
}

int multikernel_dispatch_secondaries(void)
{
    /* TODO: for each non-boot kernel slot, issue PSCI CPU_ON targeting the
     * appropriate core with multikernel_secondary_startup as the entry.
     * For MVP-Q (N=2): only kernel_1 → core 1.
     *
     * The trampoline reads multikernel_entries[core_id] to find its TTBRs
     * and kernel entry, so we must ensure the data is committed to memory
     * (DSB + cache clean) before issuing the CPU_ON SMC. */
    printf("multikernel: dispatch_secondaries() — not implemented yet\n");
    return -1;
}

#endif /* CONFIG_MULTIKERNEL */
