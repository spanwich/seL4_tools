/*
 * Copyright 2026, PhD Research Project — multikernel-AMP MVP-Q (x86_64)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * x86_64 elfloader main. Expects to be invoked by QEMU's Multiboot v1
 * loader (via `-kernel elfloader`) with two modules passed via
 * `-initrd "kernel.elf,rootserver"`. Comma separates Multiboot modules
 * in QEMU's -initrd syntax.
 *
 * Boot sequence:
 *   1. Verify Multiboot magic.
 *   2. Parse the bootloader's MBI; expect modules_count >= 2.
 *   3. Module 0 = kernel ELF (elf32-i386 post-objcopy from elf64-x86-64).
 *      Walk its program headers and copy LOAD segments to their declared
 *      physical addresses (typically 0x10000000 for K0).
 *   4. Module 1 = rootserver. Leave it where the bootloader placed it.
 *   5. Build a new MBI in scratch RAM with rootserver as its only module
 *      (the seL4 kernel expects mod 0 to be the rootserver per
 *      kernel/src/arch/x86/kernel/boot_sys.c:514).
 *   6. Jump to the kernel entry with eax=0x2BADB002, ebx=&new_mbi.
 */

#include <stdint.h>
#include "multiboot.h"
#include "elf32.h"

void uart_init(void);
void uart_puts(const char *s);
void uart_puthex(uint32_t v);
void uart_putdec(uint32_t v);
void uart_putc(char c);

/* Scratch space for the synthesized MBI passed to the kernel. Lives in .bss,
 * so it's well below the kernel's load region (0x10000000) and won't be
 * overwritten when the kernel claims memory. */
static struct mb_info new_mbi __attribute__((aligned(8)));
static struct mb_module new_mod0 __attribute__((aligned(8)));
static char new_cmdline[64];

static int memcmp_local(const void *a, const void *b, uint32_t n)
{
    const uint8_t *pa = a, *pb = b;
    for (uint32_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

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

static void halt(const char *reason)
{
    uart_puts("[elfloader] HALT: ");
    uart_puts(reason);
    uart_puts("\n");
    for (;;) asm volatile("cli; hlt");
}

/*
 * Walk an ELF32 image, copying LOAD segments to declared phys addrs.
 * Also tracks the highest physical address touched, written to *out_end
 * if non-NULL (used to find a safe relocation target for the rootserver).
 * Returns the entry point on success, 0 on failure.
 */
static uint32_t kernel_end_paddr;

static uint32_t load_elf32(uint32_t elf_base)
{
    const struct elf32_ehdr *eh = (const struct elf32_ehdr *)elf_base;

    if (eh->e_ident[0] != ELF_MAGIC0 || eh->e_ident[1] != ELF_MAGIC1 ||
        eh->e_ident[2] != ELF_MAGIC2 || eh->e_ident[3] != ELF_MAGIC3) {
        uart_puts("[elfloader] bad ELF magic\n");
        return 0;
    }
    if (eh->e_ident[4] != 1) {  /* ELFCLASS32 */
        uart_puts("[elfloader] not ELF32 — kernel must be objcopy'd to elf32-i386\n");
        return 0;
    }

    const struct elf32_phdr *ph = (const struct elf32_phdr *)(elf_base + eh->e_phoff);
    uart_puts("[elfloader]   phnum=");
    uart_putdec(eh->e_phnum);
    uart_puts(" entry=");
    uart_puthex(eh->e_entry);
    uart_puts("\n");

    uint32_t end_paddr = 0;
    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;

        uart_puts("[elfloader]   LOAD vaddr=");
        uart_puthex(ph[i].p_vaddr);
        uart_puts(" paddr=");
        uart_puthex(ph[i].p_paddr);
        uart_puts(" filesz=");
        uart_puthex(ph[i].p_filesz);
        uart_puts(" memsz=");
        uart_puthex(ph[i].p_memsz);
        uart_puts("\n");

        /* Copy file image to paddr. */
        if (ph[i].p_filesz > 0) {
            memcpy_local((void *)ph[i].p_paddr,
                         (const void *)(elf_base + ph[i].p_offset),
                         ph[i].p_filesz);
        }
        /* Zero-fill bss tail (memsz > filesz). */
        if (ph[i].p_memsz > ph[i].p_filesz) {
            memset_local((void *)(ph[i].p_paddr + ph[i].p_filesz), 0,
                         ph[i].p_memsz - ph[i].p_filesz);
        }
        uint32_t seg_end = ph[i].p_paddr + ph[i].p_memsz;
        if (seg_end > end_paddr) end_paddr = seg_end;
    }

    kernel_end_paddr = end_paddr;
    return eh->e_entry;
}

/*
 * Jump to the kernel's _start. The kernel's entry is 32-bit Multiboot v1:
 * eax = 0x2BADB002, ebx = &mbi. No return.
 */
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

void main(uint32_t mb_magic, uint32_t mb_info_paddr)
{
    uart_init();
    uart_puts("\n[elfloader] x86_64 multikernel-AMP elfloader\n");
    uart_puts("[elfloader] magic="); uart_puthex(mb_magic);
    uart_puts(" mbi=");  uart_puthex(mb_info_paddr);
    uart_puts("\n");

    if (mb_magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        halt("not Multiboot v1");
    }

    const struct mb_info *mbi = (const struct mb_info *)mb_info_paddr;
    if (!(mbi->flags & MULTIBOOT_INFO_MODS)) {
        halt("bootloader provided no Multiboot modules");
    }
    if (mbi->mods_count < 2) {
        uart_puts("[elfloader] need 2 modules (kernel + rootserver), got ");
        uart_putdec(mbi->mods_count);
        halt("module count");
    }

    const struct mb_module *mods = (const struct mb_module *)mbi->mods_addr;
    uart_puts("[elfloader] module 0 (kernel): ");
    uart_puthex(mods[0].mod_start); uart_puts(" - ");
    uart_puthex(mods[0].mod_end);   uart_puts("\n");
    uart_puts("[elfloader] module 1 (rootserver): ");
    uart_puthex(mods[1].mod_start); uart_puts(" - ");
    uart_puthex(mods[1].mod_end);   uart_puts("\n");

    /* Load the kernel ELF into its declared phys addrs. */
    uart_puts("[elfloader] loading kernel ELF...\n");
    uint32_t entry = load_elf32(mods[0].mod_start);
    if (!entry) halt("kernel load failed");
    uart_puts("[elfloader] kernel entry @ "); uart_puthex(entry);
    uart_puts(" end @ "); uart_puthex(kernel_end_paddr); uart_puts("\n");

    /* Relocate the rootserver to a paddr just after the kernel image.
     * The seL4 x86 boot path asserts mods_end_paddr > ki_p_reg.end
     * (boot_sys.c:452), so the module MUST sit above the kernel. QEMU
     * placed it in low memory; we copy it into RAM beyond the kernel. */
    uint32_t rootsrv_size = mods[1].mod_end - mods[1].mod_start;
    /* Align to 4 KiB and leave a 4 KiB gap for paranoia. */
    uint32_t rootsrv_paddr = (kernel_end_paddr + 0x1000 + 0xFFF) & ~0xFFFu;
    memcpy_local((void *)rootsrv_paddr,
                 (const void *)mods[1].mod_start, rootsrv_size);
    uart_puts("[elfloader] relocated rootserver: ");
    uart_puthex(mods[1].mod_start); uart_puts(" -> ");
    uart_puthex(rootsrv_paddr);     uart_puts(" size=");
    uart_puthex(rootsrv_size);      uart_puts("\n");

    /* Synthesize a new MBI that the kernel will receive. The seL4 kernel
     * reads mods[0] as its rootserver; we set that to the rootserver and
     * forward the memory map. We do NOT include the kernel ELF in the new
     * module list (it's already loaded). */
    memset_local(&new_mbi, 0, sizeof(new_mbi));
    /* Carry over basic mem info + the memory map for ACPI parsing. */
    new_mbi.flags = mbi->flags;
    new_mbi.mem_lower = mbi->mem_lower;
    new_mbi.mem_upper = mbi->mem_upper;
    new_mbi.mmap_length = mbi->mmap_length;
    new_mbi.mmap_addr   = mbi->mmap_addr;

    /* Carry forward the cmdline so kernel debug_port etc. survive. */
    new_mbi.cmdline = mbi->cmdline;

    /* Set up new module list with just rootserver. */
    const char *cmd = "rootserver";
    for (int i = 0; i < (int)sizeof(new_cmdline) - 1 && cmd[i]; i++) {
        new_cmdline[i] = cmd[i];
    }
    new_mod0.mod_start = rootsrv_paddr;
    new_mod0.mod_end   = rootsrv_paddr + rootsrv_size;
    new_mod0.cmdline   = (uint32_t)(uintptr_t)new_cmdline;
    new_mod0.pad       = 0;

    new_mbi.flags     |= MULTIBOOT_INFO_MODS;
    new_mbi.mods_count = 1;
    new_mbi.mods_addr  = (uint32_t)(uintptr_t)&new_mod0;

    uart_puts("[elfloader] handing off to kernel...\n");
    jump_to_kernel(entry, (uint32_t)(uintptr_t)&new_mbi);
}
