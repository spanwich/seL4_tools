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
 *   - kernel.elf, rootserver, kernel.dtb           (for core 0 — kernel_0)
 *   - kernel_1.elf, rootserver_1, kernel_1.dtb    (for core 1)
 *
 * Boot order:
 *   1. Boot CPU (core 0) runs the elfloader (this code).
 *   2. multikernel_load_all() places kernel_1 + rootserver_1 + DTB in memory
 *      at their declared physical addresses, populates multikernel_entries[1]
 *      with init_kernel ABI args.
 *   3. multikernel_dispatch_secondaries() PSCI CPU_ONs core 1 with
 *      multikernel_secondary_startup as entry, ctx_id=1.
 *   4. Core 0 falls through to the existing single-kernel boot path,
 *      which boots kernel_0 normally.
 *   5. Core 1's PSCI handoff lands in multikernel_secondary_startup, which
 *      reads multikernel_entries[1] and branches to kernel_1's _start.
 */

#include <autoconf.h>
#include <elfloader/gen_config.h>

#ifdef CONFIG_MULTIKERNEL

#include <multikernel.h>
#include <elfloader.h>
#include <elfloader_common.h>
#include <binaries/elf/elf.h>
#include <cpio/cpio.h>
#include <printf.h>
#include <abort.h>
#include <strops.h>
#include <psci.h>
#include <fdt.h>
#include <armv/machine.h>

/* Per-core scratch stack the trampoline uses before MMU is enabled.
 * 4 KiB per core × MULTIKERNEL_MAX_KERNELS. Aligned to 16 bytes for AArch64
 * AAPCS. Lives in BSS so it's zero-init by clear_bss(). */
ALIGN(16) char mk_secondary_stack[MULTIKERNEL_MAX_KERNELS][4096];

struct multikernel_entry multikernel_entries[MULTIKERNEL_MAX_KERNELS];

/* The kernel ELF for kernel_1 declares its desired phys addr in its program
 * headers. We use that to locate the kernel image; the rootserver follows
 * after the kernel and DTB, in the same convention as common.c:load_images. */
int multikernel_load_all(void)
{
    int ret;
    void const *cpio = _archive_start;
    size_t cpio_len = _archive_start_end - _archive_start;
    unsigned long blob_size_ul = 0;
    paddr_t next_paddr;
    uint64_t k1_phys_start, k1_phys_end;

    /* ----- Find kernel_1.elf in CPIO. ----- */
    void const *k1_elf = cpio_get_file(cpio, cpio_len, "kernel_1.elf",
                                       &blob_size_ul);
    if (k1_elf == NULL) {
        printf("multikernel: kernel_1.elf missing from CPIO\n");
        return -1;
    }
    size_t k1_elf_size = (size_t)blob_size_ul;
    if (elf_checkFile(k1_elf) != 0) {
        printf("multikernel: kernel_1.elf is not a valid ELF\n");
        return -1;
    }
    ret = elf_getMemoryBounds(k1_elf, 1, &k1_phys_start, &k1_phys_end);
    if (1 != ret) {
        printf("multikernel: cannot get kernel_1 memory bounds\n");
        return -1;
    }
    printf("multikernel: kernel_1 wants phys [%p..%p)\n",
           (void *)(uintptr_t)k1_phys_start, (void *)(uintptr_t)k1_phys_end);

    /* ----- Load kernel_1 ELF at its declared phys addr. ----- */
    struct image_info k1_info = {0};
    next_paddr = ROUND_UP((paddr_t)k1_phys_end, PAGE_BITS);
    ret = load_elf(cpio, cpio_len, "kernel_1", k1_elf, k1_elf_size,
                   "kernel_1.bin", (paddr_t)k1_phys_start, /*keep_headers=*/0,
                   &k1_info, &next_paddr);
    if (ret != 0) {
        printf("multikernel: failed to load kernel_1\n");
        return -1;
    }

    /* ----- Stage kernel_1.dtb after the kernel. ----- */
    void const *k1_dtb = cpio_get_file(cpio, cpio_len, "kernel_1.dtb",
                                       &blob_size_ul);
    paddr_t k1_dtb_paddr = 0;
    size_t k1_dtb_size = 0;
    if (k1_dtb != NULL) {
        size_t sz = fdt_size(k1_dtb);
        if (sz > 0) {
            paddr_t dst = ROUND_UP(next_paddr, PAGE_BITS);
            memmove((void *)dst, k1_dtb, sz);
            k1_dtb_paddr = dst;
            k1_dtb_size = sz;
            next_paddr = ROUND_UP(dst + sz, PAGE_BITS);
            printf("multikernel: kernel_1.dtb at %p (%lu bytes)\n",
                   (void *)dst, (unsigned long)sz);
        }
    }

    /* ----- Load rootserver_1 ELF. ----- */
    void const *rs1_elf = cpio_get_file(cpio, cpio_len, "rootserver_1",
                                        &blob_size_ul);
    if (rs1_elf == NULL) {
        printf("multikernel: rootserver_1 missing from CPIO\n");
        return -1;
    }
    size_t rs1_size = (size_t)blob_size_ul;
    if (elf_checkFile(rs1_elf) != 0) {
        printf("multikernel: rootserver_1 not a valid ELF\n");
        return -1;
    }
    struct image_info rs1_info = {0};
    /* keep_headers=1 to mirror common.c:load_images for the rootserver. */
    ret = load_elf(cpio, cpio_len, "rootserver_1", rs1_elf, rs1_size,
                   "app_1.bin", next_paddr, /*keep_headers=*/1,
                   &rs1_info, &next_paddr);
    if (ret != 0) {
        printf("multikernel: failed to load rootserver_1\n");
        return -1;
    }

    /* ----- Populate multikernel_entries[1] with the seL4 init_kernel ABI.
     *
     * Per kernel/src/arch/arm/64/head.S:127-134:
     *   x0: user image phys start
     *   x1: user image phys end
     *   x2: phys/virt offset    (= phys_addr - virt_addr)
     *   x3: user image virt entry
     *   x4: DTB phys addr (0 if none)
     *   x5: DTB size      (0 if none)
     */
    multikernel_entries[1].kernel_entry_vaddr = k1_info.virt_entry;
    multikernel_entries[1].user_phys_start    = rs1_info.phys_region_start;
    multikernel_entries[1].user_phys_end      = rs1_info.phys_region_end;
    multikernel_entries[1].phys_virt_offset   = rs1_info.phys_virt_offset;
    multikernel_entries[1].user_virt_entry    = rs1_info.virt_entry;
    multikernel_entries[1].dtb_paddr          = k1_dtb_paddr;
    multikernel_entries[1].dtb_size           = k1_dtb_size;
    /* TTBR0 — for now reuse the boot core's _boot_pgd_down. The trampoline
     * calls arm_enable_hyp_mmu which uses _boot_pgd_down directly. We extend
     * the page table in init_hyp_boot_vspace_extend_for_k1() (called from
     * sys_boot.c) so it covers both kernels' high-half mappings. */
    extern char _boot_pgd_down[];
    multikernel_entries[1].ttbr0_phys = (paddr_t)(uintptr_t)_boot_pgd_down;

    printf("multikernel: kernel_1 loaded; virt_entry=%p\n",
           (void *)multikernel_entries[1].kernel_entry_vaddr);
    return 0;
}

int multikernel_dispatch_secondaries(void)
{
    /* Make sure stores to multikernel_entries[] and the loaded kernel image
     * are visible to the secondary core before it executes. */
    dsb();

    /* Secondary core's MPIDR.AFF0 is target. We pass ctx_id=1 so the
     * trampoline reads multikernel_entries[1]. */
    unsigned long target_cpu = 1;
    unsigned long entry = (unsigned long)(uintptr_t)multikernel_secondary_startup;
    unsigned long ctx_id = 1;

    printf("multikernel: PSCI CPU_ON core %lu -> %p (ctx=%lu)\n",
           target_cpu, (void *)entry, ctx_id);
    int ret = psci_cpu_on(target_cpu, entry, ctx_id);
    if (ret != PSCI_SUCCESS) {
        printf("multikernel: PSCI CPU_ON failed: %d\n", ret);
        return -1;
    }
    printf("multikernel: core 1 dispatched\n");
    return 0;
}

#endif /* CONFIG_MULTIKERNEL */
