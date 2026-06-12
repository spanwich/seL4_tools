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

#define MK_TRACE_BUFFER_VADDR 0x41f000ull
#define MK_TRACE_BUFFER_SIZE  0x1000ull
#define MK_TRACE_RECORD_SIZE  0x80ull
#define MK_TRACE_MAX_RECORDS  32ull
#define MK_TRACE_K1_TRAMP_ENTRY      10ull
#define MK_TRACE_K1_ENTRY_DELAY_PRE  11ull
#define MK_TRACE_K1_ENTRY_DELAY_POST 12ull
#define MK_TRACE_K1_BEFORE_BRANCH    30ull

#ifndef MK_SECONDARY_ENTRY_DELAY_TICKS
#define MK_SECONDARY_ENTRY_DELAY_TICKS 100000ull
#endif

#ifndef MK_SECONDARY_STEP_DELAY_TICKS
#define MK_SECONDARY_STEP_DELAY_TICKS 0ull
#endif

ALIGN(16) char mk_secondary_stack[MULTIKERNEL_MAX_KERNELS][4096];

unsigned int multikernel_count = MULTIKERNEL_COUNT;
struct multikernel_entry multikernel_entries[MULTIKERNEL_MAX_KERNELS];
uint64_t mk_trace_buffer_paddr = 0;
uint64_t mk_secondary_entry_delay_ticks = MK_SECONDARY_ENTRY_DELAY_TICKS;
uint64_t mk_secondary_step_delay_ticks = MK_SECONDARY_STEP_DELAY_TICKS;

#ifndef CONFIG_PLAT_MLXBF2
extern void multikernel_secondary_park(void);

extern void mk_trace_record_phys(void);
extern void mk_trace_delay_ticks(void);
extern void mk_trace_step_delay(void);

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
 * Normal-path diagnostics are memory trace records only; UART is kept solely
 * for the EL2 exception handler below.
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
    "    adrp x16, mk_core1_vbar\n"
    "    add  x16, x16, :lo12:mk_core1_vbar\n"
    "    msr  vbar_el2, x16\n"
    "    isb\n"
    "    adrp x10, mk_secondary_stack\n"
    "    add  x10, x10, :lo12:mk_secondary_stack\n"
    "    add  x10, x10, #0x2000\n"
    "    mov  sp, x10\n"
    "    mov  x0, #10\n"
    "    bl   mk_trace_record_phys\n"
    "    mov  x0, #11\n"
    "    bl   mk_trace_record_phys\n"
    "    adrp x9, mk_secondary_entry_delay_ticks\n"
    "    add  x9, x9, :lo12:mk_secondary_entry_delay_ticks\n"
    "    ldr  x0, [x9]\n"
    "    bl   mk_trace_delay_ticks\n"
    "    mov  x0, #12\n"
    "    bl   mk_trace_record_phys\n"
    "    bl   arm_enable_hyp_mmu_secondary\n"
    "    mov  x0, #30\n"
    "    bl   mk_trace_record_phys\n"
    "    adrp x9, multikernel_entries\n"
    "    add  x9, x9, :lo12:multikernel_entries\n"
    "    add  x9, x9, #0x40\n"
    "    ldr  x12, [x9, #8]\n"
    "    ldr  x0,  [x9, #16]\n"
    "    ldr  x1,  [x9, #24]\n"
    "    ldr  x2,  [x9, #32]\n"
    "    ldr  x3,  [x9, #40]\n"
    "    ldr  x4,  [x9, #48]\n"
    "    ldr  x5,  [x9, #56]\n"
    "    br   x12\n"
    "mk_tramp_putc:\n"
    "    ldr  w13, [x15, #0x1c]\n"
    "    tbz  w13, #7, mk_tramp_putc\n"
    "    str  w14, [x15, #0x28]\n"
    "    ret\n"
    "mk_tramp_puthex:\n"
    "    mov  x11, #60\n"
    "1:  lsr  x10, x12, x11\n    and x10, x10, #0xf\n    cmp x10, #10\n"
    "    b.lt 2f\n    add w14, w10, #0x57\n    b 3f\n"
    "2:  add  w14, w10, #0x30\n"
    "3:  ldr  w13, [x15, #0x1c]\n    tbz w13, #7, 3b\n    str w14, [x15, #0x28]\n"
    "    subs x11, x11, #4\n    b.ge 1b\n    ret\n"
    "mk_core1_exc:\n"
    "    movz x15, #0x400e, lsl #16\n"
    "    mov  w14, #0x58\n bl mk_tramp_putc\n"
    "    mov  w14, #0x20\n bl mk_tramp_putc\n    mov w14, #0x45\n bl mk_tramp_putc\n mov w14, #0x3d\n bl mk_tramp_putc\n"
    "    mrs  x12, esr_el2\n bl mk_tramp_puthex\n"
    "    mov  w14, #0x20\n bl mk_tramp_putc\n    mov w14, #0x4c\n bl mk_tramp_putc\n mov w14, #0x3d\n bl mk_tramp_putc\n"
    "    mrs  x12, elr_el2\n bl mk_tramp_puthex\n"
    "    mov  w14, #0x20\n bl mk_tramp_putc\n    mov w14, #0x46\n bl mk_tramp_putc\n mov w14, #0x3d\n bl mk_tramp_putc\n"
    "    mrs  x12, far_el2\n bl mk_tramp_puthex\n"
    "    mov  w14, #0x0a\n bl mk_tramp_putc\n"
    "4:  wfe\n    b 4b\n"
    "    .balign 0x800\n"
    "    .global mk_core1_vbar\n"
    "mk_core1_vbar:\n"
    "    .rept 16\n    b mk_core1_exc\n    .balign 0x80\n    .endr\n"
    ".size mk_core1_trampoline, . - mk_core1_trampoline\n"
);
#endif

__asm__(
    ".section .text\n"
    ".align 4\n"
    ".global mk_trace_record_phys\n"
    ".type mk_trace_record_phys, %function\n"
    "mk_trace_record_phys:\n"
    "    adrp x9, mk_trace_buffer_paddr\n"
    "    add  x9, x9, :lo12:mk_trace_buffer_paddr\n"
    "    ldr  x10, [x9]\n"
    "    cbz  x10, 9f\n"
    "    cmp  x0, #32\n"
    "    b.hs 9f\n"
    "    lsl  x11, x0, #7\n"
    "    add  x10, x10, x11\n"
    "    str  x0, [x10, #0]\n"
    "    str  x0, [x10, #8]\n"
    "    mrs  x12, cntvct_el0\n"
    "    str  x12, [x10, #16]\n"
    "    mrs  x12, cntfrq_el0\n"
    "    str  x12, [x10, #24]\n"
    "    mrs  x12, mpidr_el1\n"
    "    str  x12, [x10, #32]\n"
    "    mrs  x12, CurrentEL\n"
    "    str  x12, [x10, #40]\n"
    "    mrs  x12, sctlr_el2\n"
    "    str  x12, [x10, #48]\n"
    "    mrs  x12, hcr_el2\n"
    "    str  x12, [x10, #56]\n"
    "    mrs  x12, ttbr0_el2\n"
    "    str  x12, [x10, #64]\n"
    "    mrs  x12, tcr_el2\n"
    "    str  x12, [x10, #72]\n"
    "    mrs  x12, mair_el2\n"
    "    str  x12, [x10, #80]\n"
    "    mrs  x12, esr_el2\n"
    "    str  x12, [x10, #88]\n"
    "    mrs  x12, far_el2\n"
    "    str  x12, [x10, #96]\n"
    "    mrs  x12, elr_el2\n"
    "    str  x12, [x10, #104]\n"
    "    mov  x12, sp\n"
    "    str  x12, [x10, #112]\n"
    "    str  xzr, [x10, #120]\n"
    "    dsb  sy\n"
    "9:  ret\n"
    ".global mk_trace_delay_ticks\n"
    ".type mk_trace_delay_ticks, %function\n"
    "mk_trace_delay_ticks:\n"
    "    cbz  x0, 2f\n"
    "    mrs  x9, cntvct_el0\n"
    "1:  mrs  x10, cntvct_el0\n"
    "    sub  x11, x10, x9\n"
    "    cmp  x11, x0\n"
    "    b.lo 1b\n"
    "2:  ret\n"
    ".global mk_trace_step_delay\n"
    ".type mk_trace_step_delay, %function\n"
    "mk_trace_step_delay:\n"
    "    adrp x9, mk_secondary_step_delay_ticks\n"
    "    add  x9, x9, :lo12:mk_secondary_step_delay_ticks\n"
    "    ldr  x0, [x9]\n"
    "    b    mk_trace_delay_ticks\n"
);


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


static void mk_init_trace_buffer(void)
{
#ifndef CONFIG_PLAT_MLXBF2
    if (MK_TRACE_BUFFER_VADDR < user_info.virt_region_start ||
        (MK_TRACE_BUFFER_VADDR + MK_TRACE_BUFFER_SIZE) > user_info.virt_region_end) {
        printf("multikernel: trace vaddr %p outside K0 rootserver [%p..%p)\n",
               (void *)(uintptr_t)MK_TRACE_BUFFER_VADDR,
               user_info.virt_region_start,
               user_info.virt_region_end);
        mk_trace_buffer_paddr = 0;
    } else {
        mk_trace_buffer_paddr = user_info.phys_region_start +
                                (MK_TRACE_BUFFER_VADDR - user_info.virt_region_start);
        printf("multikernel: trace buffer vaddr=%p paddr=%p entry_delay=%llu step_delay=%llu\n",
               (void *)(uintptr_t)MK_TRACE_BUFFER_VADDR,
               (void *)(uintptr_t)mk_trace_buffer_paddr,
               (unsigned long long)mk_secondary_entry_delay_ticks,
               (unsigned long long)mk_secondary_step_delay_ticks);
    }
    mk_clean_range((uintptr_t)&mk_trace_buffer_paddr,
                   (uintptr_t)(&mk_trace_buffer_paddr + 1));
    mk_clean_range((uintptr_t)&mk_secondary_entry_delay_ticks,
                   (uintptr_t)(&mk_secondary_entry_delay_ticks + 1));
    mk_clean_range((uintptr_t)&mk_secondary_step_delay_ticks,
                   (uintptr_t)(&mk_secondary_step_delay_ticks + 1));
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
    mk_init_trace_buffer();
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
