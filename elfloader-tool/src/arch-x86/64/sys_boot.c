/*
 * Copyright 2026, PhD Research Project — multikernel-AMP MVP-Q (x86_64)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * x86_64 elfloader main. Multikernel-AMP boot orchestrator — generalised
 * to N kernels (Task C).
 *
 * Invoke via QEMU:
 *   qemu-system-x86_64 -kernel elfloader -smp N -m 4G \
 *       -initrd "k0,rs0[,k1,rs1[,k2,rs2[,k3,rs3...]]]"
 *
 * Module list is (kernel ELF, rootserver) pairs. N = mods_count / 2:
 *   N == 1  → single-kernel boot (Step 1). Load K0, jump to it.
 *   N >= 2  → N-kernel boot. Load K0..K(N-1), then for each AP i in
 *             1..N-1 patch the shared real-mode trampoline with K_i's
 *             entry+MBI, SIPI APIC id i, and wait for the AP to ack that
 *             it has latched those values before reprogramming for the
 *             next AP. Finally the BSP runs K0 on core 0.
 *
 * Memory map: see include/arch-x86/64/mvpq_memmap.h. Each kernel gets a
 * disjoint <1 GiB low window (ELF + rootserver + untyped) and a disjoint
 * >1 GiB high RAM pool. The shared inter-kernel region is excluded from
 * every kernel's usable mmap (→ device untyped) and bracketed by guard
 * pages so it becomes exactly one device untyped at MVPQ_SHARED_BASE.
 */

#include <stdint.h>
#include "multiboot.h"
#include "elf32.h"
#include "mvpq_memmap.h"

void uart_init(void);
void uart_puts(const char *s);
void uart_puthex(uint32_t v);
void uart_putdec(uint32_t v);
void uart_putc(char c);
void lapic_send_init_sipi(uint8_t apic_id, uint32_t trampoline_paddr);

extern char ap_trampoline_start[];
extern char ap_trampoline_end[];
extern uint32_t ap_kentry;   /* patched: K_i entry paddr           */
extern uint32_t ap_kmbi;     /* patched: &K_i MBI                   */
extern uint32_t ap_ack;      /* AP sets 1 once entry/mbi are latched */

/* Scratch for synthesized per-kernel MBIs. Lives in our .bss at the
 * elfloader's load address (~1 MiB), well below every kernel window. */
static struct mb_info     kmbi[MVPQ_MAX_KERNELS]      __attribute__((aligned(8)));
static struct mb_module   kmod[MVPQ_MAX_KERNELS]      __attribute__((aligned(8)));
static char               kcmdline[MVPQ_MAX_KERNELS][16];
/* 4 mmap entries per kernel: lo window, hi window, shared guard-lo,
 * shared guard-hi. (The shared region itself is deliberately absent.) */
static struct mb_mmap_entry kmmap[MVPQ_MAX_KERNELS][4] __attribute__((aligned(8)));

static void memcpy_local(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
}

static void memset_local(void *dst, int v, uint32_t n)
{
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)v;
}

static __attribute__((noreturn)) void halt(const char *reason)
{
    uart_puts("[elfloader] HALT: ");
    uart_puts(reason);
    uart_puts("\n");
    for (;;) asm volatile("cli; hlt");
}

/* Walk an ELF32 image, copying LOAD segments to declared phys addrs.
 * Returns entry vaddr on success, 0 on failure. Updates *end_paddr_out
 * with the highest phys addr the kernel touches. */
static uint32_t load_elf32(uint32_t elf_base, uint32_t *end_paddr_out)
{
    const struct elf32_ehdr *eh = (const struct elf32_ehdr *)elf_base;
    if (eh->e_ident[0] != ELF_MAGIC0 || eh->e_ident[1] != ELF_MAGIC1 ||
        eh->e_ident[2] != ELF_MAGIC2 || eh->e_ident[3] != ELF_MAGIC3) {
        uart_puts("[elfloader] bad ELF magic\n");
        return 0;
    }
    if (eh->e_ident[4] != 1) {
        uart_puts("[elfloader] not ELF32 (kernel must be objcopy'd)\n");
        return 0;
    }

    const struct elf32_phdr *ph = (const struct elf32_phdr *)(elf_base + eh->e_phoff);
    uint32_t end_paddr = 0;
    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uart_puts("[elfloader]   LOAD paddr=");
        uart_puthex(ph[i].p_paddr);
        uart_puts(" filesz=");
        uart_puthex(ph[i].p_filesz);
        uart_puts(" memsz=");
        uart_puthex(ph[i].p_memsz);
        uart_puts("\n");
        if (ph[i].p_filesz > 0) {
            memcpy_local((void *)ph[i].p_paddr,
                         (const void *)(elf_base + ph[i].p_offset),
                         ph[i].p_filesz);
        }
        if (ph[i].p_memsz > ph[i].p_filesz) {
            memset_local((void *)(ph[i].p_paddr + ph[i].p_filesz), 0,
                         ph[i].p_memsz - ph[i].p_filesz);
        }
        uint32_t seg_end = ph[i].p_paddr + ph[i].p_memsz;
        if (seg_end > end_paddr) end_paddr = seg_end;
    }
    if (end_paddr_out) *end_paddr_out = end_paddr;
    return eh->e_entry;
}

static void set_mmap(struct mb_mmap_entry *e, uint64_t base, uint64_t len, uint32_t type)
{
    e->size      = sizeof(struct mb_mmap_entry) - 4;
    e->base_addr = base;
    e->length    = len;
    e->type      = type;   /* 1 = usable RAM */
}

/* Build a per-kernel MBI: the rootserver as the only module, plus a
 * 4-entry mmap (low window, high pool, two shared-region guard pages).
 * The shared region [MVPQ_SHARED_BASE, MVPQ_SHARED_END) is intentionally
 * NOT in the usable set so seL4 exposes it as a device untyped; the two
 * guard pages bracket it so it lands as exactly one device UT whose base
 * == MVPQ_SHARED_BASE. */
static void build_kernel_mbi(struct mb_info *out,
                             const struct mb_info *src,
                             struct mb_module *mod_storage,
                             uint32_t rootserver_paddr,
                             uint32_t rootserver_size,
                             char *cmdline,
                             struct mb_mmap_entry *mmap,
                             uint32_t i)
{
    memset_local(out, 0, sizeof(*out));
    out->cmdline = src->cmdline;

    set_mmap(&mmap[0], MVPQ_KLO_BASE(i), MVPQ_KLO_SIZE, 1);
    set_mmap(&mmap[1], MVPQ_KHI_BASE(i), MVPQ_KHI_SIZE, 1);
    set_mmap(&mmap[2], MVPQ_GUARD_LO,    MVPQ_GUARD_SIZE, 1);
    set_mmap(&mmap[3], MVPQ_GUARD_HI,    MVPQ_GUARD_SIZE, 1);

    out->flags       = MULTIBOOT_INFO_MEM_MAP | MULTIBOOT_INFO_MEMORY;
    out->mem_lower   = 640;
    out->mem_upper   = (MVPQ_KLO_SIZE + MVPQ_KHI_SIZE) / 1024;
    out->mmap_length = 4 * sizeof(struct mb_mmap_entry);
    out->mmap_addr   = (uint32_t)(uintptr_t)mmap;

    mod_storage->mod_start = rootserver_paddr;
    mod_storage->mod_end   = rootserver_paddr + rootserver_size;
    mod_storage->cmdline   = (uint32_t)(uintptr_t)cmdline;
    mod_storage->pad       = 0;
    out->flags     |= MULTIBOOT_INFO_MODS;
    out->mods_count = 1;
    out->mods_addr  = (uint32_t)(uintptr_t)mod_storage;
}

/* Round up to the next 4 KiB boundary, plus a 4 KiB safety gap. */
static uint32_t round_up_with_gap(uint32_t v)
{
    return (v + 0x1000 + 0xFFF) & ~0xFFFu;
}

static __attribute__((noreturn)) void jump_to_kernel(uint32_t entry, uint32_t mbi_paddr)
{
    asm volatile(
        "movl %0, %%eax\n\t"
        "movl %1, %%ebx\n\t"
        "jmp  *%2\n\t"
        :: "i"(MULTIBOOT_BOOTLOADER_MAGIC), "r"(mbi_paddr), "r"(entry)
        : "eax", "ebx"
    );
    __builtin_unreachable();
}

/* Copy the trampoline blob to TRAMPOLINE_PADDR once. */
static void stage_ap_trampoline(void)
{
    uint32_t sz = (uint32_t)(uintptr_t)(ap_trampoline_end - ap_trampoline_start);
    uart_puts("[elfloader] staging AP trampoline ("); uart_putdec(sz);
    uart_puts(" bytes) @ "); uart_puthex(MVPQ_TRAMPOLINE_PADDR); uart_puts("\n");
    memcpy_local((void *)(uintptr_t)MVPQ_TRAMPOLINE_PADDR,
                 (const void *)ap_trampoline_start, sz);
}

static volatile uint32_t *tramp_slot(uint32_t *sym)
{
    uint32_t off = (uint32_t)((char *)sym - ap_trampoline_start);
    return (volatile uint32_t *)(uintptr_t)(MVPQ_TRAMPOLINE_PADDR + off);
}

/* Bring up AP `apic_id` running kernel `k_entry`/`k_mbi_paddr`. Serialised:
 * we patch the trampoline's generic slots, clear the ack, SIPI, then spin
 * until the AP has latched the values into registers (ack==1) so it is
 * safe to reprogram the trampoline for the next AP. */
static void bringup_ap(uint8_t apic_id, uint32_t k_entry, uint32_t k_mbi_paddr)
{
    volatile uint32_t *p_entry = tramp_slot(&ap_kentry);
    volatile uint32_t *p_mbi   = tramp_slot(&ap_kmbi);
    volatile uint32_t *p_ack   = tramp_slot(&ap_ack);

    *p_entry = k_entry;
    *p_mbi   = k_mbi_paddr;
    *p_ack   = 0;

    uart_puts("[elfloader]   AP "); uart_putdec(apic_id);
    uart_puts(" -> entry "); uart_puthex(k_entry);
    uart_puts(" mbi ");      uart_puthex(k_mbi_paddr); uart_puts("\n");

    lapic_send_init_sipi(apic_id, MVPQ_TRAMPOLINE_PADDR);

    /* Wait (bounded) for the AP to latch entry/mbi before we repatch. */
    uint32_t spins = 0;
    while (*p_ack == 0) {
        asm volatile("pause");
        if (++spins > 200000000u) {
            uart_puts("[elfloader]   WARN: AP "); uart_putdec(apic_id);
            uart_puts(" no ack — continuing\n");
            break;
        }
    }
    if (*p_ack) {
        uart_puts("[elfloader]   AP "); uart_putdec(apic_id);
        uart_puts(" latched\n");
    }
}

void main(uint32_t mb_magic, uint32_t mb_info_paddr)
{
    uart_init();
    uart_puts("\n[elfloader] x86_64 multikernel-AMP elfloader (N-kernel)\n");
    uart_puts("[elfloader] magic="); uart_puthex(mb_magic);
    uart_puts(" mbi=");               uart_puthex(mb_info_paddr);
    uart_puts("\n");

    if (mb_magic != MULTIBOOT_BOOTLOADER_MAGIC) halt("not Multiboot v1");

    const struct mb_info *mbi = (const struct mb_info *)mb_info_paddr;
    if (!(mbi->flags & MULTIBOOT_INFO_MODS)) halt("no modules");
    if (mbi->mods_count < 2 || (mbi->mods_count & 1)) {
        uart_puts("[elfloader] need an even module count >=2, got ");
        uart_putdec(mbi->mods_count);
        halt("module count");
    }

    uint32_t n = mbi->mods_count / 2;
    if (n > MVPQ_MAX_KERNELS) {
        uart_puts("[elfloader] too many kernels: "); uart_putdec(n);
        halt("kernel count");
    }
    uart_puts("[elfloader] booting "); uart_putdec(n);
    uart_puts(" kernel(s)\n");

    const struct mb_module *mods = (const struct mb_module *)mbi->mods_addr;

    uint32_t entry[MVPQ_MAX_KERNELS];

    /* -------- Load every kernel + relocate its rootserver -------- */
    for (uint32_t i = 0; i < n; i++) {
        uart_puts("[elfloader] loading K"); uart_putdec(i); uart_puts("...\n");
        uint32_t k_end = 0;
        entry[i] = load_elf32(mods[2 * i].mod_start, &k_end);
        if (!entry[i]) halt("kernel load failed");

        uint32_t rs_size  = mods[2 * i + 1].mod_end - mods[2 * i + 1].mod_start;
        uint32_t rs_paddr = round_up_with_gap(k_end);
        memcpy_local((void *)(uintptr_t)rs_paddr,
                     (const void *)(uintptr_t)mods[2 * i + 1].mod_start, rs_size);
        uart_puts("[elfloader] K"); uart_putdec(i);
        uart_puts(" entry="); uart_puthex(entry[i]);
        uart_puts(" rs->");   uart_puthex(rs_paddr); uart_puts("\n");

        /* cmdline: "rootserver_<i>" */
        kcmdline[i][0] = 'r'; kcmdline[i][1] = 's'; kcmdline[i][2] = '_';
        kcmdline[i][3] = '0' + (char)i; kcmdline[i][4] = 0;

        build_kernel_mbi(&kmbi[i], mbi, &kmod[i], rs_paddr, rs_size,
                         kcmdline[i], kmmap[i], i);
    }

    /* -------- Wake APs 1..N-1 (serialised) -------- */
    if (n >= 2) {
        stage_ap_trampoline();
        for (uint32_t i = 1; i < n; i++) {
            bringup_ap((uint8_t)i, entry[i], (uint32_t)(uintptr_t)&kmbi[i]);
        }
    }

    uart_puts("[elfloader] BSP handing off to K0\n");
    jump_to_kernel(entry[0], (uint32_t)(uintptr_t)&kmbi[0]);
}
