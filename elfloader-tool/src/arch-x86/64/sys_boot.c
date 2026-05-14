/*
 * Copyright 2026, PhD Research Project — multikernel-AMP MVP-Q (x86_64)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * x86_64 elfloader main. Multikernel-AMP boot orchestrator.
 *
 * Invoke via QEMU:
 *   qemu-system-x86_64 -kernel elfloader \
 *       -initrd "k0_kernel,k0_rootserver[,k1_kernel,k1_rootserver]" \
 *       -enable-kvm -cpu host -smp 2
 *
 * Modes (driven by module count):
 *   2 modules → single-kernel boot (Step 1). Load K0, forward rs0 to it.
 *   4 modules → two-kernel boot (Step 2). Load K0 and K1, set up the
 *     real-mode AP trampoline at TRAMPOLINE_PADDR, issue SIPI to APIC 1
 *     so it runs K1 on core 1, then BSP continues to K0 on core 0.
 */

#include <stdint.h>
#include "multiboot.h"
#include "elf32.h"

#define TRAMPOLINE_PADDR    0x8000u
#define K1_APIC_ID          1

void uart_init(void);
void uart_puts(const char *s);
void uart_puthex(uint32_t v);
void uart_putdec(uint32_t v);
void uart_putc(char c);
void lapic_send_init_sipi(uint8_t apic_id, uint32_t trampoline_paddr);

extern char ap_trampoline_start[];
extern char ap_trampoline_end[];
extern uint32_t ap_k1_entry;
extern uint32_t ap_k1_mbi;

/* Scratch space for synthesized MBIs (one per kernel). Lives in our .bss,
 * well below the kernel load regions at 0x10000000 / 0x20000000. */
static struct mb_info k0_mbi __attribute__((aligned(8)));
static struct mb_info k1_mbi __attribute__((aligned(8)));
static struct mb_module k0_mod __attribute__((aligned(8)));
static struct mb_module k1_mod __attribute__((aligned(8)));
static char k0_cmdline[32] = "rootserver_0";
static char k1_cmdline[32] = "rootserver_1";

/* Per-kernel memory map so K0 and K1 see disjoint usable RAM regions and
 * don't fight over untyped allocations. The kernel parses these to build
 * its physical memory pool. */
static struct mb_mmap_entry k0_mmap[1] __attribute__((aligned(8)));
static struct mb_mmap_entry k1_mmap[1] __attribute__((aligned(8)));

#define K0_RAM_BASE   0x10000000u
#define K0_RAM_SIZE   0x10000000u   /* 256 MiB */
#define K1_RAM_BASE   0x20000000u
#define K1_RAM_SIZE   0x10000000u   /* 256 MiB */
#define SHARED_BASE   0x30000000u
#define SHARED_SIZE   0x00100000u   /* 1 MiB (Steps 3 & 4) */

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

/* Build a per-kernel MBI with the rootserver as its only module and a
 * disjoint usable-RAM region (so K0 and K1 don't fight over untypeds). */
static void build_kernel_mbi(struct mb_info *out,
                             const struct mb_info *src,
                             struct mb_module *mod_storage,
                             uint32_t rootserver_paddr,
                             uint32_t rootserver_size,
                             char *cmdline,
                             struct mb_mmap_entry *mmap,
                             uint32_t ram_base,
                             uint32_t ram_size)
{
    memset_local(out, 0, sizeof(*out));
    out->cmdline = src->cmdline;

    /* Synthesize a 1-entry mmap covering this kernel's exclusive RAM. */
    mmap[0].size      = sizeof(struct mb_mmap_entry) - 4;
    mmap[0].base_addr = (uint64_t)ram_base;
    mmap[0].length    = (uint64_t)ram_size;
    mmap[0].type      = 1; /* usable RAM */

    out->flags       = MULTIBOOT_INFO_MEM_MAP | MULTIBOOT_INFO_MEMORY;
    out->mem_lower   = 640;            /* legacy <640 KiB (unused by seL4) */
    out->mem_upper   = ram_size / 1024; /* size in KiB above 1 MiB */
    out->mmap_length = sizeof(*mmap);
    out->mmap_addr   = (uint32_t)(uintptr_t)mmap;

    /* Rootserver module. */
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

/* Stage the SIPI trampoline at TRAMPOLINE_PADDR and patch its data slots
 * (ap_k1_entry, ap_k1_mbi) with K1's entry paddr and MBI paddr. */
static void install_ap_trampoline(uint32_t k1_entry, uint32_t k1_mbi_paddr)
{
    uint32_t tramp_size = (uint32_t)(uintptr_t)(ap_trampoline_end - ap_trampoline_start);
    uart_puts("[elfloader] installing AP trampoline ("); uart_putdec(tramp_size);
    uart_puts(" bytes) @ "); uart_puthex(TRAMPOLINE_PADDR); uart_puts("\n");

    memcpy_local((void *)(uintptr_t)TRAMPOLINE_PADDR,
                 (const void *)ap_trampoline_start,
                 tramp_size);

    /* Compute offsets of the patch slots within the trampoline blob. */
    uint32_t entry_off = (uint32_t)((char *)&ap_k1_entry - ap_trampoline_start);
    uint32_t mbi_off   = (uint32_t)((char *)&ap_k1_mbi   - ap_trampoline_start);
    *(volatile uint32_t *)(uintptr_t)(TRAMPOLINE_PADDR + entry_off) = k1_entry;
    *(volatile uint32_t *)(uintptr_t)(TRAMPOLINE_PADDR + mbi_off)   = k1_mbi_paddr;

    uart_puts("[elfloader]   patched ap_k1_entry @ +"); uart_puthex(entry_off);
    uart_puts(" = ");                                   uart_puthex(k1_entry);
    uart_puts("\n[elfloader]   patched ap_k1_mbi   @ +"); uart_puthex(mbi_off);
    uart_puts(" = ");                                   uart_puthex(k1_mbi_paddr);
    uart_puts("\n");
}

void main(uint32_t mb_magic, uint32_t mb_info_paddr)
{
    uart_init();
    uart_puts("\n[elfloader] x86_64 multikernel-AMP elfloader\n");
    uart_puts("[elfloader] magic="); uart_puthex(mb_magic);
    uart_puts(" mbi=");               uart_puthex(mb_info_paddr);
    uart_puts("\n");

    if (mb_magic != MULTIBOOT_BOOTLOADER_MAGIC) halt("not Multiboot v1");

    const struct mb_info *mbi = (const struct mb_info *)mb_info_paddr;
    if (!(mbi->flags & MULTIBOOT_INFO_MODS)) halt("no modules");
    if (mbi->mods_count != 2 && mbi->mods_count != 4) {
        uart_puts("[elfloader] need 2 or 4 modules, got ");
        uart_putdec(mbi->mods_count);
        halt("module count");
    }

    const struct mb_module *mods = (const struct mb_module *)mbi->mods_addr;

    /* -------- Load K0 -------- */
    uart_puts("[elfloader] loading K0...\n");
    uint32_t k0_end = 0;
    uint32_t k0_entry = load_elf32(mods[0].mod_start, &k0_end);
    if (!k0_entry) halt("K0 load failed");

    /* Relocate rs0 to align(k0_end+gap). */
    uint32_t rs0_size  = mods[1].mod_end - mods[1].mod_start;
    uint32_t rs0_paddr = round_up_with_gap(k0_end);
    memcpy_local((void *)(uintptr_t)rs0_paddr,
                 (const void *)(uintptr_t)mods[1].mod_start, rs0_size);
    uart_puts("[elfloader] K0 entry="); uart_puthex(k0_entry);
    uart_puts(" rs0->");                uart_puthex(rs0_paddr); uart_puts("\n");

    build_kernel_mbi(&k0_mbi, mbi, &k0_mod, rs0_paddr, rs0_size, k0_cmdline,
                     k0_mmap, K0_RAM_BASE, K0_RAM_SIZE);

    if (mbi->mods_count == 2) {
        /* Step-1 single-kernel path. */
        uart_puts("[elfloader] single-kernel mode — handing off to K0\n");
        jump_to_kernel(k0_entry, (uint32_t)(uintptr_t)&k0_mbi);
    }

    /* -------- Load K1 -------- */
    uart_puts("[elfloader] loading K1...\n");
    uint32_t k1_end = 0;
    uint32_t k1_entry = load_elf32(mods[2].mod_start, &k1_end);
    if (!k1_entry) halt("K1 load failed");

    /* Relocate rs1 above K1. */
    uint32_t rs1_size  = mods[3].mod_end - mods[3].mod_start;
    uint32_t rs1_paddr = round_up_with_gap(k1_end);
    memcpy_local((void *)(uintptr_t)rs1_paddr,
                 (const void *)(uintptr_t)mods[3].mod_start, rs1_size);
    uart_puts("[elfloader] K1 entry="); uart_puthex(k1_entry);
    uart_puts(" rs1->");                uart_puthex(rs1_paddr); uart_puts("\n");

    build_kernel_mbi(&k1_mbi, mbi, &k1_mod, rs1_paddr, rs1_size, k1_cmdline,
                     k1_mmap, K1_RAM_BASE, K1_RAM_SIZE);

    /* -------- Wake AP into K1 -------- */
    install_ap_trampoline(k1_entry, (uint32_t)(uintptr_t)&k1_mbi);
    lapic_send_init_sipi(K1_APIC_ID, TRAMPOLINE_PADDR);

    uart_puts("[elfloader] BSP handing off to K0\n");
    jump_to_kernel(k0_entry, (uint32_t)(uintptr_t)&k0_mbi);
}
