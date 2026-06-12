/*
 * Copyright 2026, PhD Research Project -- multikernel-AMP
 *
 * SPDX-License-Identifier: GPL-2.0-only
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
#ifdef CONFIG_PLAT_MLXBF2
#include <mode/structures.h>
#endif

#ifndef MULTIKERNEL_COUNT
#define MULTIKERNEL_COUNT 2
#endif

#define BF2_MK_SHARED_POOL_PADDR 0x87fff0000ull
#define BF2_MK_SHARED_POOL_SIZE  0x1000ull
#define BF2_MK_RING_MAGIC        0x4d4b4c47u /* "MKLG" */

ALIGN(16) char mk_secondary_stack[MULTIKERNEL_MAX_KERNELS][4096];

unsigned int multikernel_count = MULTIKERNEL_COUNT;
struct multikernel_entry multikernel_entries[MULTIKERNEL_MAX_KERNELS];

#ifndef CONFIG_PLAT_MLXBF2
extern void multikernel_secondary_park(void);
__asm__(
    ".section .text\n"
    ".align 12\n"
    ".global multikernel_secondary_park\n"
    ".type multikernel_secondary_park, %function\n"
    "multikernel_secondary_park:\n"
    "    msr daifset, #0xf\n"
    "1:  wfe\n"
    "    b 1b\n"
);

/*
 * Position-independent "core 1 alive" stub for STM32MP25 multikernel bring-up.
 * The elfloader copies [mk_core1_stub_start, mk_core1_stub_end) into K1's
 * SURVIVING region (k_phys_start + 0x400000); the booted-K0 rootserver then
 * PSCI-CPU_ON's core 1 to that physical address. It runs NS, MMU-off, and:
 *   - prints a banner on USART2 (0x400e0000) via the stm32h7-uart TX path,
 *   - loops petting IWDG1 through the OP-TEE SMC watchdog (0xbc000000/PET=3)
 *     and emitting a heartbeat '.', so the board no longer resets at 32s and
 *     core 1's liveness is visible.
 * Proves core 1 executes surviving NS code from K1's region without tripping
 * the RISAB firewall — the foundation before the full K1 kernel trampoline.
 */
extern char mk_core1_stub_start[];
extern char mk_core1_stub_end[];
__asm__(
    ".section .text\n"
    ".align 4\n"
    ".global mk_core1_stub_start\n"
    "mk_core1_stub_start:\n"
    "    movz x9, #0x400e, lsl #16\n"      /* x9 = USART2 base 0x400E0000 */
    "    adr  x10, 7f\n"                   /* x10 = banner (PC-relative) */
    "1:  ldrb w11, [x10], #1\n"
    "    cbz  w11, 3f\n"
    "2:  ldr  w12, [x9, #0x1c]\n"          /* USART_ISR */
    "    tbz  w12, #7, 2b\n"               /* wait TXE (bit 7) */
    "    str  w11, [x9, #0x28]\n"          /* USART_TDR */
    "    b    1b\n"
    "3:  movz x0, #0xbc00, lsl #16\n"      /* SMCWD_FUNC_ID 0xBC000000 */
    "    mov  x1, #3\n"                    /* SMCWD_PET */
    "    smc  #0\n"                        /* pet IWDG1 via OP-TEE */
    "    movz x9, #0x400e, lsl #16\n"      /* re-load (smc may clobber) */
    "    mov  x13, #0x8000000\n"
    "4:  subs x13, x13, #1\n"
    "    b.ne 4b\n"
    "    mov  w11, #0x2e\n"                /* '.' heartbeat */
    "5:  ldr  w12, [x9, #0x1c]\n"
    "    tbz  w12, #7, 5b\n"
    "    str  w11, [x9, #0x28]\n"
    "    b    3b\n"
    ".align 3\n"
    "7:  .asciz \"[K1] core1 ALIVE via trampoline stub (NS, MMU off)\\n\"\n"
    ".global mk_core1_stub_end\n"
    "mk_core1_stub_end:\n"
);

/*
 * Stage 2b: real K1 bring-up trampoline (STM32MP25, hypervisor/EL2 build).
 * Core 1 is PSCI-CPU_ON'd here by the booted-K0 rootserver. We reuse the
 * elfloader's PROVEN hyp MMU setup: arm_enable_hyp_mmu loads _boot_pgd_down
 * into TTBR0_EL2, whose high-half (built for K0) already maps K1's virtual
 * entry 0x8098000000 -> 0x98000000 (K0/K1 share pv_offset 0x8000000000 and
 * K1's phys lies inside K0's mapped 1 GiB). Then we load K1's init_kernel ABI
 * args from multikernel_entries[1] and branch to its virtual entry.
 *
 * Requires the elfloader region (this code, _boot_pgd_down, mk_secondary_stack,
 * multikernel_entries) to survive K0's boot — the minimal K0 rootserver does
 * not reclaim it; if a future K0 does, reserve [elfloader_start..end] from K0.
 *
 * NB struct multikernel_entry layout (8B fields): ttbr0(0) kentry(8)
 * x0/user_phys_start(16) x1/user_phys_end(24) x2/pv_offset(32)
 * x3/user_virt_entry(40) x4/dtb_paddr(48) x5/dtb_size(56) — the seL4 init_kernel
 * ABI (head.S:127-134).
 */
extern void arm_enable_hyp_mmu_secondary(void);
__asm__(
    ".section .text\n"
    ".align 4\n"
    ".global mk_core1_trampoline\n"
    ".type mk_core1_trampoline, %function\n"
    "mk_core1_trampoline:\n"
    "    msr  daifset, #0xf\n"
    "    isb\n"
    /* DEBUG (MMU off, raw USART2): print "K1:EL<n> A\n" so we see core 1's EL
     * and that it reached here, BEFORE touching any EL2 register. x15=UART base,
     * w14=char, x13=tmp; mk_tramp_putc is a leaf (no stack) so this is safe with
     * sp not yet set. */
    "    movz x15, #0x400e, lsl #16\n"
    "    mov  w14, #0x4b\n    bl mk_tramp_putc\n"   /* 'K' */
    "    mov  w14, #0x31\n    bl mk_tramp_putc\n"   /* '1' */
    "    mov  w14, #0x3a\n    bl mk_tramp_putc\n"   /* ':' */
    "    mrs  x14, CurrentEL\n    lsr x14, x14, #2\n    add w14, w14, #0x30\n"
    "    bl mk_tramp_putc\n"                        /* EL digit (2=EL2,1=EL1) */
    "    mov  w14, #0x41\n    bl mk_tramp_putc\n"   /* 'A' = before MMU enable */
    "    mov  w14, #0x0a\n    bl mk_tramp_putc\n"
    /* Point VBAR_EL2 at our DDR handler so an exception in the MMU enable is
     * reported (ESR/ELR/FAR) instead of vectoring into BL31's secure table. */
    "    adrp x16, mk_core1_vbar\n"
    "    add  x16, x16, :lo12:mk_core1_vbar\n"
    "    msr  vbar_el2, x16\n"
    "    isb\n"
    /* scratch stack = top of mk_secondary_stack[1] (the MMU helper needs sp) */
    "    adrp x10, mk_secondary_stack\n"
    "    add  x10, x10, :lo12:mk_secondary_stack\n"
    "    add  x10, x10, #0x2000\n"            /* slot 1 base (0x1000) + top (0x1000) */
    "    mov  sp, x10\n"
    "    bl   arm_enable_hyp_mmu_secondary\n" /* TTBR0_EL2=_boot_pgd_down; MMU on; no global cache ops */
    /* DEBUG: print 'B\n' = MMU enable returned (UART is device, works post-MMU) */
    "    movz x15, #0x400e, lsl #16\n"
    "    mov  w14, #0x42\n    bl mk_tramp_putc\n"   /* 'B' */
    "    mov  w14, #0x0a\n    bl mk_tramp_putc\n"
    "    adrp x9, multikernel_entries\n"
    "    add  x9, x9, :lo12:multikernel_entries\n"
    "    add  x9, x9, #0x40\n"                /* multikernel_entries[1] (sizeof 64) */
    "    ldr  x12, [x9, #8]\n"                /* kernel_entry_vaddr (0x8098000000) */
    "    ldr  x0,  [x9, #16]\n"               /* user image phys start */
    "    ldr  x1,  [x9, #24]\n"               /* user image phys end */
    "    ldr  x2,  [x9, #32]\n"               /* phys/virt offset */
    "    ldr  x3,  [x9, #40]\n"               /* user virt entry */
    "    ldr  x4,  [x9, #48]\n"               /* dtb phys */
    "    ldr  x5,  [x9, #56]\n"               /* dtb size */
    "    br   x12\n"
    "mk_tramp_putc:\n"                         /* w14=char, x15=USART2 base; leaf */
    "    ldr  w13, [x15, #0x1c]\n"             /* USART_ISR */
    "    tbz  w13, #7, mk_tramp_putc\n"        /* wait TXE */
    "    str  w14, [x15, #0x28]\n"             /* USART_TDR */
    "    ret\n"
    /* print x12 as 16 hex digits; x15=UART; clobbers x10,x11,x13,x14; leaf */
    "mk_tramp_puthex:\n"
    "    mov  x11, #60\n"
    "1:  lsr  x10, x12, x11\n    and x10, x10, #0xf\n    cmp x10, #10\n"
    "    b.lt 2f\n    add w14, w10, #0x57\n    b 3f\n"
    "2:  add  w14, w10, #0x30\n"
    "3:  ldr  w13, [x15, #0x1c]\n    tbz w13, #7, 3b\n    str w14, [x15, #0x28]\n"
    "    subs x11, x11, #4\n    b.ge 1b\n    ret\n"
    /* EL2 exception handler: print 'X E=<esr> L=<elr> F=<far>' then park. */
    "mk_core1_exc:\n"
    "    movz x15, #0x400e, lsl #16\n"
    "    mov  w14, #0x58\n bl mk_tramp_putc\n"   /* 'X' */
    "    mov  w14, #0x20\n bl mk_tramp_putc\n    mov w14, #0x45\n bl mk_tramp_putc\n mov w14, #0x3d\n bl mk_tramp_putc\n" /* ' E=' */
    "    mrs  x12, esr_el2\n bl mk_tramp_puthex\n"
    "    mov  w14, #0x20\n bl mk_tramp_putc\n    mov w14, #0x4c\n bl mk_tramp_putc\n mov w14, #0x3d\n bl mk_tramp_putc\n" /* ' L=' */
    "    mrs  x12, elr_el2\n bl mk_tramp_puthex\n"
    "    mov  w14, #0x20\n bl mk_tramp_putc\n    mov w14, #0x46\n bl mk_tramp_putc\n mov w14, #0x3d\n bl mk_tramp_putc\n" /* ' F=' */
    "    mrs  x12, far_el2\n bl mk_tramp_puthex\n"
    "    mov  w14, #0x0a\n bl mk_tramp_putc\n"
    "4:  wfe\n    b 4b\n"
    /* EL2 vector table (16 x 0x80), all entries branch to the handler. */
    "    .balign 0x800\n"
    "    .global mk_core1_vbar\n"
    "mk_core1_vbar:\n"
    "    .rept 16\n    b mk_core1_exc\n    .balign 0x80\n    .endr\n"
    ".size mk_core1_trampoline, . - mk_core1_trampoline\n"
);
#endif

static const unsigned long bf2_mpidr[MULTIKERNEL_MAX_KERNELS] = {
    0x0, 0x1, 0x100, 0x101, 0x200, 0x201, 0x300, 0x301
};

static const char *kernel_names[MULTIKERNEL_MAX_KERNELS] = {
    "kernel", "kernel_1", "kernel_2", "kernel_3",
    "kernel_4", "kernel_5", "kernel_6", "kernel_7"
};

static const char *kernel_elf_names[MULTIKERNEL_MAX_KERNELS] = {
    "kernel.elf", "kernel_1.elf", "kernel_2.elf", "kernel_3.elf",
    "kernel_4.elf", "kernel_5.elf", "kernel_6.elf", "kernel_7.elf"
};

static const char *kernel_dtb_names[MULTIKERNEL_MAX_KERNELS] = {
    "kernel.dtb", "kernel_1.dtb", "kernel_2.dtb", "kernel_3.dtb",
    "kernel_4.dtb", "kernel_5.dtb", "kernel_6.dtb", "kernel_7.dtb"
};

static const char *rootserver_names[MULTIKERNEL_MAX_KERNELS] = {
    "rootserver", "rootserver_1", "rootserver_2", "rootserver_3",
    "rootserver_4", "rootserver_5", "rootserver_6", "rootserver_7"
};

static const char *app_bin_names[MULTIKERNEL_MAX_KERNELS] = {
    "app.bin", "app_1.bin", "app_2.bin", "app_3.bin",
    "app_4.bin", "app_5.bin", "app_6.bin", "app_7.bin"
};

static void mk_clean_range(uintptr_t start, uintptr_t end)
{
    extern void elfloader_dcache_clean_range(uintptr_t start, uintptr_t end);
    elfloader_dcache_clean_range(start, end);
}

#ifdef CONFIG_PLAT_MLXBF2
static void mk_map_kernel_window(unsigned int idx, struct image_info *kernel_info)
{
    extern uint64_t _boot_pud_up[BIT(PUD_BITS)];
    word_t pud = GET_PUD_INDEX(kernel_info->virt_entry);
    paddr_t paddr = kernel_info->phys_region_start & ~MASK(ARM_1GB_BLOCK_BITS);

    _boot_pud_up[pud] = paddr
                        | BIT(10)       /* access flag */
                        | (4 << 2)      /* MT_NORMAL */
                        | BIT(0);       /* 1G block */
    mk_clean_range((uintptr_t)&_boot_pud_up[pud],
                   (uintptr_t)&_boot_pud_up[pud + 1]);
    printf("multikernel: K%u boot PUD[%lu]=%lx for entry=%p\n",
           idx, (unsigned long)pud, (unsigned long)_boot_pud_up[pud],
           kernel_info->virt_entry);
}
#else
static void mk_map_kernel_window(unsigned int idx, struct image_info *kernel_info)
{
    UNUSED_VARIABLE(idx);
    UNUSED_VARIABLE(kernel_info);
}
#endif

static void mk_init_shared_pool(void)
{
#ifdef CONFIG_PLAT_MLXBF2
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)BF2_MK_SHARED_POOL_PADDR;
    for (unsigned int i = 0; i < BF2_MK_SHARED_POOL_SIZE / sizeof(*p); i++) {
        p[i] = 0;
    }
    *(volatile uint32_t *)(uintptr_t)BF2_MK_SHARED_POOL_PADDR = BF2_MK_RING_MAGIC;
    mk_clean_range((uintptr_t)BF2_MK_SHARED_POOL_PADDR,
                   (uintptr_t)(BF2_MK_SHARED_POOL_PADDR + BF2_MK_SHARED_POOL_SIZE));
    printf("multikernel: shared dataport initialized at %p\n",
           (void *)(uintptr_t)BF2_MK_SHARED_POOL_PADDR);
#endif
}

static int load_secondary(unsigned int idx, void const *cpio, size_t cpio_len)
{
    int ret;
    unsigned long blob_size_ul = 0;
    paddr_t next_paddr;
    uint64_t k_phys_start, k_phys_end;

    void const *kernel_elf = cpio_get_file(cpio, cpio_len,
                                           kernel_elf_names[idx],
                                           &blob_size_ul);
    if (kernel_elf == NULL) {
        printf("multikernel: %s missing from CPIO\n", kernel_elf_names[idx]);
        return -1;
    }
    size_t kernel_elf_size = (size_t)blob_size_ul;
    if (elf_checkFile(kernel_elf) != 0) {
        printf("multikernel: %s is not a valid ELF\n", kernel_elf_names[idx]);
        return -1;
    }
    ret = elf_getMemoryBounds(kernel_elf, 1, &k_phys_start, &k_phys_end);
    if (1 != ret) {
        printf("multikernel: cannot get %s memory bounds\n", kernel_elf_names[idx]);
        return -1;
    }
    printf("multikernel: K%u wants phys [%p..%p)\n", idx,
           (void *)(uintptr_t)k_phys_start, (void *)(uintptr_t)k_phys_end);

    struct image_info kernel_info = {0};
    next_paddr = ROUND_UP((paddr_t)k_phys_end, PAGE_BITS);
    ret = load_elf(cpio, cpio_len, kernel_names[idx], kernel_elf,
                   kernel_elf_size, "kernel_secondary.bin",
                   (paddr_t)k_phys_start, 0, &kernel_info, &next_paddr);
    if (ret != 0) {
        printf("multikernel: failed to load K%u kernel\n", idx);
        return -1;
    }
    mk_clean_range((uintptr_t)kernel_info.phys_region_start,
                   (uintptr_t)kernel_info.phys_region_end);
    mk_map_kernel_window(idx, &kernel_info);

    void const *kernel_dtb = cpio_get_file(cpio, cpio_len,
                                           kernel_dtb_names[idx],
                                           &blob_size_ul);
    paddr_t dtb_paddr = 0;
    size_t dtb_size = 0;
    if (kernel_dtb != NULL) {
        size_t sz = fdt_size(kernel_dtb);
        if (sz > 0) {
            paddr_t dst = ROUND_UP(next_paddr, PAGE_BITS);
            memmove((void *)dst, kernel_dtb, sz);
            dtb_paddr = dst;
            dtb_size = sz;
            next_paddr = ROUND_UP(dst + sz, PAGE_BITS);
            mk_clean_range((uintptr_t)dst, (uintptr_t)(dst + sz));
            printf("multikernel: K%u dtb at %p (%lu bytes)\n",
                   idx, (void *)dst, (unsigned long)sz);
        }
    }

    void const *rootserver_elf = cpio_get_file(cpio, cpio_len,
                                               rootserver_names[idx],
                                               &blob_size_ul);
    if (rootserver_elf == NULL) {
        printf("multikernel: %s missing from CPIO\n", rootserver_names[idx]);
        return -1;
    }
    size_t rootserver_size = (size_t)blob_size_ul;
    if (elf_checkFile(rootserver_elf) != 0) {
        printf("multikernel: %s is not a valid ELF\n", rootserver_names[idx]);
        return -1;
    }
    struct image_info rootserver_info = {0};
    ret = load_elf(cpio, cpio_len, rootserver_names[idx], rootserver_elf,
                   rootserver_size, app_bin_names[idx], next_paddr, 1,
                   &rootserver_info, &next_paddr);
    if (ret != 0) {
        printf("multikernel: failed to load K%u rootserver\n", idx);
        return -1;
    }
    mk_clean_range((uintptr_t)rootserver_info.phys_region_start,
                   (uintptr_t)rootserver_info.phys_region_end);

#ifndef CONFIG_PLAT_MLXBF2
    /* Stage the "core1 alive" stub 4 MiB into this kernel's region (well above
     * its kernel/dtb/rootserver), in memory K0 never owns, so it survives K0's
     * boot. The booted-K0 rootserver PSCI-CPU_ON's core 1 to this address. */
    {
        paddr_t stub_paddr = (paddr_t)k_phys_start + 0x400000;
        size_t stub_len = (size_t)(mk_core1_stub_end - mk_core1_stub_start);
        memmove((void *)stub_paddr, mk_core1_stub_start, stub_len);
        mk_clean_range((uintptr_t)stub_paddr, (uintptr_t)(stub_paddr + stub_len));
        printf("multikernel: K%u core1 stub staged at %p (%lu bytes)\n",
               idx, (void *)stub_paddr, (unsigned long)stub_len);
    }
#endif

    multikernel_entries[idx].ttbr0_phys = 0;
    multikernel_entries[idx].kernel_entry_vaddr = kernel_info.virt_entry;
    multikernel_entries[idx].user_phys_start = rootserver_info.phys_region_start;
    multikernel_entries[idx].user_phys_end = rootserver_info.phys_region_end;
    multikernel_entries[idx].phys_virt_offset = rootserver_info.phys_virt_offset;
    multikernel_entries[idx].user_virt_entry = rootserver_info.virt_entry;
    multikernel_entries[idx].dtb_paddr = dtb_paddr;
    multikernel_entries[idx].dtb_size = dtb_size;
    mk_clean_range((uintptr_t)&multikernel_entries[idx],
                   (uintptr_t)(&multikernel_entries[idx] + 1));

    printf("multikernel: K%u loaded entry=%p user=[%p..%p] uentry=%p dtb=%p/%lu\n",
           idx, kernel_info.virt_entry,
           rootserver_info.phys_region_start, rootserver_info.phys_region_end,
           rootserver_info.virt_entry, (void *)dtb_paddr,
           (unsigned long)dtb_size);
    return 0;
}

int multikernel_load_all(void)
{
    void const *cpio = _archive_start;
    size_t cpio_len = _archive_start_end - _archive_start;

    if (multikernel_count < 2 || multikernel_count > MULTIKERNEL_MAX_KERNELS) {
        printf("multikernel: invalid count %u\n", multikernel_count);
        return -1;
    }

    printf("multikernel: loading %u-kernel BF2 bundle\n", multikernel_count);
    mk_init_shared_pool();
    for (unsigned int idx = 1; idx < multikernel_count; idx++) {
        if (load_secondary(idx, cpio, cpio_len) != 0) {
            return -1;
        }
    }
    mk_clean_range((uintptr_t)multikernel_entries,
                   (uintptr_t)(multikernel_entries + multikernel_count));
    return 0;
}

int multikernel_dispatch_secondaries(void)
{
    dsb();

    #ifdef CONFIG_PLAT_MLXBF2
    unsigned long entry = (unsigned long)(uintptr_t)multikernel_secondary_startup;
#else
    unsigned long entry = (unsigned long)(uintptr_t)multikernel_secondary_park;
#endif
    for (unsigned int idx = 1; idx < multikernel_count; idx++) {
        unsigned long target_cpu = bf2_mpidr[idx];
        unsigned long ctx_id = idx;
        printf("multikernel: PSCI CPU_ON K%u mpidr=%lx -> %p ctx=%lu\n",
               idx, target_cpu, (void *)entry, ctx_id);
        int ret = psci_cpu_on(target_cpu, entry, ctx_id);
        if (ret != PSCI_SUCCESS) {
            printf("multikernel: PSCI CPU_ON K%u failed: %d\n", idx, ret);
            return -1;
        }
        printf("multikernel: K%u dispatched\n", idx);
    }
    return 0;
}

#endif /* CONFIG_MULTIKERNEL */
