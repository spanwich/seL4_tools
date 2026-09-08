/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2021, HENSOLDT Cyber
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <autoconf.h>
#include <elfloader/gen_config.h>

#include <drivers.h>
#include <drivers/uart.h>
#include <printf.h>
#include <types.h>
#include <abort.h>
#include <strops.h>
#include <cpuid.h>
#ifdef CONFIG_PLAT_MLXBF2
#include <mode/structures.h>
#endif

#include <binaries/efi/efi.h>
#include <elfloader.h>

/* Multikernel-AMP boot trace. The BF2-ELFLOADER[...] lines are very verbose and
 * truncate the actual boot info on a narrow console, so they are gated off here.
 * Set BF2_TRACE to 1 to restore them. The `if (0)` form keeps args referenced
 * (no unused-variable warnings) and is dead-code-eliminated when disabled. */
#define BF2_TRACE 0
#define BF2_PRINTF(...) do { if (BF2_TRACE) printf(__VA_ARGS__); } while (0)

/* 0xd00dfeed in big endian */
#define DTB_MAGIC (0xedfe0dd0)

/* Maximum alignment we need to preserve when relocating (64K) */
#define MAX_ALIGN_BITS (14)

#ifdef CONFIG_IMAGE_EFI
ALIGN(BIT(PAGE_BITS)) VISIBLE
#ifdef CONFIG_PLAT_MLXBF2
/* Keep the early EFI stack outside _bss: clear_bss() runs after _start
 * installs this stack, and the EFI linker folds .bss into .data.
 * Initializing one byte keeps the object before _bss so it is not wiped.
 */
char core_stack_alloc[CONFIG_MAX_NUM_NODES][BIT(PAGE_BITS) * 4] = { { 1 } };
#else
char core_stack_alloc[CONFIG_MAX_NUM_NODES][BIT(PAGE_BITS)];
#endif
#endif

struct image_info kernel_info;
struct image_info user_info;
void const *dtb;
size_t dtb_size;

#ifdef CONFIG_PLAT_MLXBF2
/* These are intentionally initialized so the linker keeps them in .data before
 * the boot page-table/BSS area. The live kernel_info/user_info globals are
 * clobbered during the BF2 EL2->EL1 handoff path before vspace setup.
 */
struct image_info bf2_saved_kernel_info = { .phys_region_start = 1 };
struct image_info bf2_saved_user_info = { .phys_region_start = 1 };
void const *bf2_saved_dtb = (void const *)1;
size_t bf2_saved_dtb_size = 1;

extern uint64_t _boot_pgd_up[BIT(PGD_BITS)];
extern uint64_t _boot_pud_up[BIT(PUD_BITS)];
extern uint64_t _boot_pmd_up[BIT(PMD_BITS)];
extern uint64_t _boot_pgd_down[BIT(PGD_BITS)];
extern uint64_t _boot_pud_down[BIT(PUD_BITS)];
#endif


extern void finish_relocation(int offset, void *_dynamic, unsigned int total_offset);
void continue_boot(int was_relocated);
void continue_boot_el1_after_handoff(void);
#ifdef CONFIG_PLAT_MLXBF2
extern void bf2_install_vector_table(void);
#endif

#ifdef CONFIG_ARCH_AARCH64
static __attribute__((unused)) unsigned long read_currentel(void)
{
    unsigned long val;
    asm volatile("mrs %0, CurrentEL" : "=r"(val));
    return val;
}

static __attribute__((unused)) unsigned long read_vbar_el1(void)
{
    unsigned long val;
    asm volatile("mrs %0, vbar_el1" : "=r"(val));
    return val;
}

static __attribute__((unused)) unsigned long read_sctlr_el1(void)
{
    unsigned long val;
    asm volatile("mrs %0, sctlr_el1" : "=r"(val));
    return val;
}

static __attribute__((unused)) unsigned long read_vbar_el2(void)
{
    unsigned long val;
    asm volatile("mrs %0, vbar_el2" : "=r"(val));
    return val;
}

static __attribute__((unused)) unsigned long read_sctlr_el2(void)
{
    unsigned long val;
    asm volatile("mrs %0, sctlr_el2" : "=r"(val));
    return val;
}

static __attribute__((unused)) unsigned long read_hcr_el2(void)
{
    unsigned long val;
    asm volatile("mrs %0, hcr_el2" : "=r"(val));
    return val;
}

static __attribute__((unused)) unsigned long read_spsr_el2(void)
{
    unsigned long val;
    asm volatile("mrs %0, spsr_el2" : "=r"(val));
    return val;
}

static __attribute__((unused)) unsigned long read_elr_el2(void)
{
    unsigned long val;
    asm volatile("mrs %0, elr_el2" : "=r"(val));
    return val;
}

static void bf2_step_regs(const char *step)
{
    /* Verbose register-dump trace silenced for multikernel bring-up; the
     * BF2_PRINTF macro (top of file) no-ops the remaining BF2-ELFLOADER lines. */
    (void)step;
}

void bf2_leave_hyp_after_eret_marker(void)
{
    bf2_step_regs("after-eret-in-leave-hyp");
}

void bf2_leave_hyp_second_el1_marker(void)
{
    bf2_step_regs("second-el1-call-after-eret");
}

void bf2_el1_continuation_entry_marker(void)
{
    bf2_step_regs("el1-continuation-entry");
}
#else
static void bf2_step_regs(const char *step)
{
    BF2_PRINTF("BF2-ELFLOADER[%s]\n", step);
}

void bf2_leave_hyp_after_eret_marker(void)
{
    bf2_step_regs("after-eret-in-leave-hyp");
}

void bf2_leave_hyp_second_el1_marker(void)
{
    bf2_step_regs("second-el1-call-after-eret");
}

void bf2_el1_continuation_entry_marker(void)
{
    bf2_step_regs("el1-continuation-entry");
}
#endif

static void bf2_print_kernel_mode(void)
{
#ifdef CONFIG_ARM_HYPERVISOR_SUPPORT
    BF2_PRINTF("BF2-ELFLOADER[config]: ARM_HYPERVISOR_SUPPORT=on\n");
#else
    BF2_PRINTF("BF2-ELFLOADER[config]: ARM_HYPERVISOR_SUPPORT=off\n");
#endif
}


#ifdef CONFIG_PLAT_MLXBF2
static void bf2_clean_obj(void const *ptr, size_t size)
{
    uintptr_t start = (uintptr_t)ptr;
    bf2_dcache_clean_range(start, start + size);
}

static void bf2_clean_loaded_metadata(void)
{
    bf2_clean_obj(&kernel_info, sizeof(kernel_info));
    bf2_clean_obj(&user_info, sizeof(user_info));
    bf2_clean_obj(&dtb, sizeof(dtb));
    bf2_clean_obj(&dtb_size, sizeof(dtb_size));
    bf2_clean_obj(&bf2_saved_kernel_info, sizeof(bf2_saved_kernel_info));
    bf2_clean_obj(&bf2_saved_user_info, sizeof(bf2_saved_user_info));
    bf2_clean_obj(&bf2_saved_dtb, sizeof(bf2_saved_dtb));
    bf2_clean_obj(&bf2_saved_dtb_size, sizeof(bf2_saved_dtb_size));
    BF2_PRINTF("BF2-ELFLOADER[metadata-cleaned]: live+saved metadata cleaned to PoC\n");
}

static void bf2_clean_boot_vspace(void)
{
    bf2_clean_obj(_boot_pgd_up, sizeof(_boot_pgd_up));
    bf2_clean_obj(_boot_pud_up, sizeof(_boot_pud_up));
    bf2_clean_obj(_boot_pmd_up, sizeof(_boot_pmd_up));
    bf2_clean_obj(_boot_pgd_down, sizeof(_boot_pgd_down));
    bf2_clean_obj(_boot_pud_down, sizeof(_boot_pud_down));
    BF2_PRINTF("BF2-ELFLOADER[boot-vspace-cleaned]: boot page tables cleaned to PoC\n");
}

static unsigned long bf2_read_ttbr0_el1(void)
{
    unsigned long val;
    asm volatile("mrs %0, ttbr0_el1" : "=r"(val));
    return val;
}

static unsigned long bf2_read_ttbr1_el1(void)
{
    unsigned long val;
    asm volatile("mrs %0, ttbr1_el1" : "=r"(val));
    return val;
}

static unsigned long bf2_read_tcr_el1(void)
{
    unsigned long val;
    asm volatile("mrs %0, tcr_el1" : "=r"(val));
    return val;
}

static unsigned long bf2_read_mair_el1(void)
{
    unsigned long val;
    asm volatile("mrs %0, mair_el1" : "=r"(val));
    return val;
}

static unsigned long bf2_translate_el1_read(uintptr_t va)
{
    unsigned long par;
    asm volatile("at s1e1r, %1\n	"
                 "isb\n	"
                 "mrs %0, par_el1"
                 : "=r"(par)
                 : "r"(va)
                 : "memory");
    return par;
}

static void bf2_print_boot_vspace_state(const char *step, uintptr_t va)
{
    word_t pgd = GET_PGD_INDEX(va);
    word_t pud = GET_PUD_INDEX(va);
    word_t pmd = GET_PMD_INDEX(va);

    BF2_PRINTF("BF2-ELFLOADER[%s-vspace-map]: va=%p pgd=%lu pud=%lu pmd=%lu\n",
           step, va, (unsigned long)pgd, (unsigned long)pud, (unsigned long)pmd);
    BF2_PRINTF("BF2-ELFLOADER[%s-vspace-map]: pgd_up=%p pud_up=%p pmd_up=%p ttbr0=%lx ttbr1=%lx tcr=%lx mair=%lx\n",
           step, _boot_pgd_up, _boot_pud_up, _boot_pmd_up,
           bf2_read_ttbr0_el1(), bf2_read_ttbr1_el1(),
           bf2_read_tcr_el1(), bf2_read_mair_el1());
    BF2_PRINTF("BF2-ELFLOADER[%s-vspace-map]: pgd_entry=%lx pud_entry=%lx pmd_entry=%lx\n",
           step,
           (unsigned long)_boot_pgd_up[pgd],
           (unsigned long)_boot_pud_up[pud],
           (unsigned long)_boot_pmd_up[pmd]);
}

static void bf2_probe_kernel_entry_translation(uintptr_t va)
{
    unsigned long par = bf2_translate_el1_read(va);
    BF2_PRINTF("BF2-ELFLOADER[entry-translation]: va=%p par_el1=%lx", va, par);
    if (par & 1) {
        printf(" fault_fst=%lx", (par >> 1) & 0x3f);
    }
    printf("\n");
}

static void bf2_save_loaded_metadata(void)
{
    bf2_saved_kernel_info = kernel_info;
    bf2_saved_user_info = user_info;
    bf2_saved_dtb = dtb;
    bf2_saved_dtb_size = dtb_size;
    BF2_PRINTF("BF2-ELFLOADER[saved-metadata]: &saved_kernel=%p &saved_user=%p k_entry=%p u_entry=%p dtb=%p dtb_size=%lu\n",
           &bf2_saved_kernel_info,
           &bf2_saved_user_info,
           bf2_saved_kernel_info.virt_entry,
           bf2_saved_user_info.virt_entry,
           bf2_saved_dtb,
           (unsigned long)bf2_saved_dtb_size);
}
#endif

static void bf2_print_image_state(const char *step)
{
    BF2_PRINTF("BF2-ELFLOADER[%s]: &kernel_info=%p &user_info=%p &dtb=%p\n",
           step, &kernel_info, &user_info, &dtb);
    BF2_PRINTF("BF2-ELFLOADER[%s]: k_entry=%p k_phys=[%p..%p] k_virt=[%p..%p] k_off=%p\n",
           step,
           kernel_info.virt_entry,
           kernel_info.phys_region_start,
           kernel_info.phys_region_end,
           kernel_info.virt_region_start,
           kernel_info.virt_region_end,
           kernel_info.phys_virt_offset);
    BF2_PRINTF("BF2-ELFLOADER[%s]: u_entry=%p u_phys=[%p..%p] u_virt=[%p..%p] u_off=%p dtb=%p dtb_size=%lu\n",
           step,
           user_info.virt_entry,
           user_info.phys_region_start,
           user_info.phys_region_end,
           user_info.virt_region_start,
           user_info.virt_region_end,
           user_info.phys_virt_offset,
           dtb,
           (unsigned long)dtb_size);
}


/*
 * Make sure the ELF loader is below the kernel's first virtual address
 * so that when we enable the MMU we can keep executing.
 */
extern char _DYNAMIC[];
void relocate_below_kernel(void)
{
    /*
     * These are the ELF loader's physical addresses,
     * since we are either running with MMU off or
     * identity-mapped.
     */
    uintptr_t UNUSED start = (uintptr_t)_text;
    uintptr_t end = (uintptr_t)_end;

    if (end <= kernel_info.virt_region_start) {
        /*
         * If the ELF loader is already below the kernel,
         * skip relocation.
         */
        continue_boot(0);
        return;
    }

#ifdef CONFIG_IMAGE_EFI
    /*
     * Note: we make the (potentially incorrect) assumption
     * that there is enough physical RAM below the kernel's first vaddr
     * to fit the ELF loader.
     * FIXME: do we need to make sure we don't accidentally wipe out the DTB too?
     */
    uintptr_t size = end - start;

    /*
     * we ROUND_UP size in this calculation so that all aligned things
     * (interrupt vectors, stack, etc.) end up in similarly aligned locations.
     * The strictes alignment requirement we have is the 64K-aligned AArch32
     * page tables, so we use that to calculate the new base of the elfloader.
     */
    uintptr_t new_base = kernel_info.virt_region_start - (ROUND_UP(size, MAX_ALIGN_BITS));
    uint32_t offset = start - new_base;
    printf("relocating from %p-%p to %p-%p... size=0x%x (padded size = 0x%x)\n", start, end, new_base, new_base + size,
           size, ROUND_UP(size, MAX_ALIGN_BITS));

    memmove((void *)new_base, (void *)start, size);

    /* call into assembly to do the finishing touches */
    finish_relocation(offset, _DYNAMIC, new_base);
#else
    printf("ERROR: The ELF loader does not support relocating itself. You"
           " probably need to move the kernel window higher, or the load"
           " address lower.\n");
    abort();
#endif
}

/*
 * Entry point.
 *
 * Unpack images, setup the MMU, jump to the kernel.
 */
void main(UNUSED void *arg)
{
    void *bootloader_dtb = NULL;

    /* initialize platform to a state where we can print to a UART */
    if (initialise_devices()) {
        printf("ERROR: Did not successfully return from initialise_devices()\n");
        abort();
    }
    bf2_step_regs("after-initialise-devices");

    platform_init();
    bf2_step_regs("after-platform-init");

    /* Print welcome message. */
    printf("\nELF-loader started on ");
    print_cpuid();
    printf("  paddr=[%p..%p]\n", _text, (uintptr_t)_end - 1);
    bf2_print_kernel_mode();

#if defined(CONFIG_IMAGE_UIMAGE)

    /* U-Boot passes a DTB. Ancient bootloaders may pass atags. When booting via
     * bootelf argc is NULL.
     */
    if (arg && (DTB_MAGIC == *(uint32_t *)arg)) {
        bootloader_dtb = arg;
    }

#elif defined(CONFIG_IMAGE_EFI)

#ifdef CONFIG_PLAT_MLXBF2
    if (__efi_system_table) {
        BF2_PRINTF("BF2-ELFLOADER[efi-real-entry]: ImageHandle=%p SystemTable=%p\n",
               __application_handle, __efi_system_table);
        bf2_step_regs("before-efi-get-fdt");
        bootloader_dtb = efi_get_fdt();
        BF2_PRINTF("BF2-ELFLOADER[efi-get-fdt]: fdt=%p\n", bootloader_dtb);
        bf2_step_regs("before-exit-boot-services");
        unsigned long efi_exit_status = efi_exit_boot_services();
        if (efi_exit_status != EFI_SUCCESS) {
            printf("ERROR: Unable to exit UEFI boot services! status=%lx\n", efi_exit_status);
            abort();
        }
#ifdef CONFIG_PLAT_MLXBF2
        bf2_install_vector_table();
#endif
        bf2_step_regs("after-exit-boot-services");
        /* UEFI ConOut is no longer valid after this point. */
    } else {
        /* BF2 RShim Linux-Image compatibility path: UEFI has already exited
         * boot services, and x0 carries the DTB if one was supplied.
         */
        BF2_PRINTF("BF2-ELFLOADER[linux-image-entry]: arg/dtb=%p\n", arg);
        bootloader_dtb = arg;
    }
#else
    if (efi_exit_boot_services() != EFI_SUCCESS) {
        printf("ERROR: Unable to exit UEFI boot services!\n");
        abort();
    }

    bootloader_dtb = efi_get_fdt();
#endif

#endif

    if (bootloader_dtb) {
        printf("  dtb=%p\n", bootloader_dtb);
    } else {
        printf("No DTB passed in from boot loader.\n");
    }

    /* Unpack ELF images into memory. */
    bf2_step_regs("before-load-images");
    unsigned int num_apps = 0;
    int ret = load_images(&kernel_info, &user_info, 1, &num_apps,
                          bootloader_dtb, &dtb, &dtb_size);
    if (0 != ret) {
        printf("ERROR: image loading failed\n");
        abort();
    }

    BF2_PRINTF("BF2-ELFLOADER[after-load-images]: num_apps=%u dtb=%p dtb_size=%lu\n",
           num_apps, dtb, (unsigned long)dtb_size);
    bf2_print_image_state("after-load-images");
#ifdef CONFIG_PLAT_MLXBF2
    bf2_save_loaded_metadata();
    bf2_clean_loaded_metadata();
#endif
    bf2_step_regs("after-load-images");
    if (num_apps != 1) {
        printf("ERROR: expected to load just 1 app, actually loaded %u apps\n",
               num_apps);
        abort();
    }
    /*
     * We don't really know where we've been loaded.
     * It's possible that EFI loaded us in a place
     * that will become part of the 'kernel window'
     * once we switch to the boot page tables.
     * Make sure this is not the case.
     */
    BF2_PRINTF("BF2-ELFLOADER[before-relocate-below-kernel]\n");
    relocate_below_kernel();
    printf("ERROR: Relocation failed, aborting!\n");
    abort();
}

static void continue_boot_after_el_handoff(void)
{
    struct image_info kernel_info_local = kernel_info;
    struct image_info user_info_local = user_info;
    void const *dtb_local = dtb;
    size_t dtb_size_local = dtb_size;

    BF2_PRINTF("BF2-ELFLOADER[before-vspace-hyp-mode-check]\n");
    bf2_step_regs("before-vspace-hyp-mode-check");
    BF2_PRINTF("BF2-ELFLOADER[metadata-local-copy]: k_entry=%p u_entry=%p dtb=%p dtb_size=%lu\n",
           kernel_info_local.virt_entry,
           user_info_local.virt_entry,
           dtb_local,
           (unsigned long)dtb_size_local);
    /* Setup MMU. */
    if (is_hyp_mode()) {
#ifdef CONFIG_ARCH_AARCH64
        extern void disable_caches_hyp();
        BF2_PRINTF("BF2-ELFLOADER[before-disable-caches-hyp]\n");
        bf2_step_regs("before-disable-caches-hyp");
        disable_caches_hyp();
        BF2_PRINTF("BF2-ELFLOADER[after-disable-caches-hyp]\n");
        bf2_step_regs("after-disable-caches-hyp");
#endif
        BF2_PRINTF("BF2-ELFLOADER[before-init-hyp-boot-vspace]\n");
        init_hyp_boot_vspace(&kernel_info_local);
        BF2_PRINTF("BF2-ELFLOADER[after-init-hyp-boot-vspace]\n");
    } else {
        /* If we are not in HYP mode, we enable the SV MMU and paging
         * just in case the kernel does not support hyp mode. */
        BF2_PRINTF("BF2-ELFLOADER[before-init-boot-vspace]\n");
        init_boot_vspace(&kernel_info_local);
        BF2_PRINTF("BF2-ELFLOADER[after-init-boot-vspace]\n");
    }
#ifdef CONFIG_PLAT_MLXBF2
    bf2_print_boot_vspace_state("before-clean", (uintptr_t)kernel_info_local.virt_entry);
    bf2_clean_boot_vspace();
    bf2_print_boot_vspace_state("after-clean", (uintptr_t)kernel_info_local.virt_entry);
#endif
    bf2_print_image_state("after-vspace-init-global");
    BF2_PRINTF("BF2-ELFLOADER[after-vspace-init-local]: k_entry=%p u_entry=%p dtb=%p dtb_size=%lu\n",
           kernel_info_local.virt_entry,
           user_info_local.virt_entry,
           dtb_local,
           (unsigned long)dtb_size_local);
    bf2_step_regs("after-vspace-init");

#if CONFIG_MAX_NUM_NODES > 1
    smp_boot();
#endif /* CONFIG_MAX_NUM_NODES */

#ifdef CONFIG_MULTIKERNEL
    /* Multikernel-AMP: load secondary kernel/rootserver/DTB payloads at their
     * declared physical addresses and populate multikernel_entries[].
     *
     * Dispatch is intentionally build-time gated. The first BF2 proof point is
     * that K0 remains stable with the secondary payloads loaded and mapped. The
     * previous immediate-dispatch experiment proved K1 can reach user space, but
     * it let K1 run while K0 was still in this handoff path and K0 aborted. */
    {
        extern int multikernel_load_all(void);
        if (multikernel_load_all() != 0) {
            printf("ERROR: multikernel_load_all() failed\n");
            abort();
        }
#ifndef MULTIKERNEL_DISPATCH_SECONDARIES
        printf("multikernel: secondary dispatch disabled for K0 stability test\n");
#elif !defined(MULTIKERNEL_DISPATCH_AFTER_MMU)
        extern int multikernel_dispatch_secondaries(void);
        printf("multikernel: dispatching secondaries before K0 MMU enable\n");
        if (multikernel_dispatch_secondaries() != 0) {
            printf("ERROR: multikernel_dispatch_secondaries() failed\n");
            abort();
        }
#endif
    }
#endif

    if (is_hyp_mode()) {
        printf("Enabling hypervisor MMU and paging\n");
        bf2_step_regs("before-arm-enable-hyp-mmu");
        arm_enable_hyp_mmu();
        bf2_step_regs("after-arm-enable-hyp-mmu");
    } else {
        printf("Enabling MMU and paging\n");
        bf2_step_regs("before-arm-enable-mmu");
        arm_enable_mmu();
        bf2_step_regs("after-arm-enable-mmu");
#ifdef CONFIG_PLAT_MLXBF2
        bf2_print_boot_vspace_state("after-mmu", (uintptr_t)kernel_info_local.virt_entry);
        bf2_probe_kernel_entry_translation((uintptr_t)kernel_info_local.virt_entry);
#endif
        bf2_print_image_state("after-arm-enable-mmu");
    }

#if defined(CONFIG_MULTIKERNEL) && defined(MULTIKERNEL_DISPATCH_SECONDARIES) && defined(MULTIKERNEL_DISPATCH_AFTER_MMU)
    {
        extern int multikernel_dispatch_secondaries(void);
        printf("multikernel: dispatching secondaries after K0 MMU enable\n");
        if (multikernel_dispatch_secondaries() != 0) {
            printf("ERROR: multikernel_dispatch_secondaries() failed\n");
            abort();
        }
    }
#endif

    /* Enter kernel. The UART may no longer be accessible here. */
    if ((uintptr_t)uart_get_mmio() < kernel_info_local.virt_region_start) {
        printf("Jumping to kernel-image entry point...\n\n");
    }

    BF2_PRINTF("BF2-ELFLOADER[kernel-entry]: entry=%p user=[%p..%p] offset=%p user_entry=%p dtb=%p dtb_size=%lu\n",
           kernel_info_local.virt_entry,
           user_info_local.phys_region_start,
           user_info_local.phys_region_end,
           user_info_local.phys_virt_offset,
           user_info_local.virt_entry,
           dtb_local,
           (unsigned long)dtb_size_local);
    ((init_arm_kernel_t)kernel_info_local.virt_entry)(user_info_local.phys_region_start,
                                                      user_info_local.phys_region_end,
                                                      user_info_local.phys_virt_offset,
                                                      user_info_local.virt_entry,
                                                      (word_t)dtb_local,
                                                      dtb_size_local);

    /* We should never get here. */
    printf("ERROR: Kernel returned back to the ELF Loader\n");
    abort();
}

void continue_boot_el1_after_handoff(void)
{
    BF2_PRINTF("BF2-ELFLOADER[continue-boot-el1-after-handoff]\n");
    bf2_step_regs("continue-boot-el1-after-handoff");
    continue_boot_after_el_handoff();
}

void continue_boot(int was_relocated)
{
    BF2_PRINTF("BF2-ELFLOADER[continue-boot]: was_relocated=%d\n", was_relocated);
    bf2_step_regs("continue-boot-entry");
    if (was_relocated) {
        printf("ELF loader relocated, continuing boot...\n");
    }

    /*
     * If we were relocated, we need to re-initialise the
     * driver model so all its pointers are set up properly.
     */
    if (was_relocated) {
        if (initialise_devices()) {
            printf("ERROR: Did not successfully return from initialise_devices()\n");
            abort();
        }
    }

    BF2_PRINTF("BF2-ELFLOADER[before-el1-leave-hyp-check]\n");
    bf2_step_regs("before-el1-leave-hyp-check");
#if (defined(CONFIG_ARCH_ARM_V7A) || defined(CONFIG_ARCH_ARM_V8A)) && !defined(CONFIG_ARM_HYPERVISOR_SUPPORT)
    if (is_hyp_mode()) {
        extern void leave_hyp(void);
        BF2_PRINTF("BF2-ELFLOADER[before-leave-hyp]\n");
        bf2_step_regs("before-leave-hyp");
        leave_hyp();
        printf("ERROR: leave_hyp returned to EL2 caller\n");
        abort();
    } else {
        BF2_PRINTF("BF2-ELFLOADER[already-not-hyp-before-vspace]\n");
    }
#endif


    continue_boot_after_el_handoff();
}

