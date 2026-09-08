/* SPDX-License-Identifier: BSD-2-Clause */
/* Restricted BIOS/Multiboot1 loader for the Optiplex idle experiment. */
#include <stdint.h>
#include "multiboot.h"
#include "elf32.h"
#include "mvpq_memmap.h"
#include "idle_boot.h"
#include "idle_ranges.h"

void uart_init(void);
void uart_puts(const char *s);
void uart_puthex(uint32_t v);
void uart_putdec(uint32_t v);
void lapic_send_init_sipi(uint8_t id, uint32_t trampoline);
extern char ap_trampoline_start[], ap_trampoline_end[], _text[], _end[];
extern uint32_t ap_kentry, ap_kmbi, ap_ack;

static struct mb_info info[4];
static struct mb_module root[4], modules[8];
static struct mb_mmap_entry maps[4][4];
static const struct mb_info *original;
static struct idle_boot *const shared = (void *)IDLE_SHARED_PHYS;
static uint32_t entries[4], root_at[4];

static __attribute__((noreturn)) void fail(const char *s)
{
    uart_puts("[loader] FAIL: "); uart_puts(s); uart_puts("\n");
    for (;;) __asm__ volatile("cli; hlt");
}

static void copy(void *dest, const void *src, uint32_t n)
{
    uint8_t *d = dest;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
}

static void zero(void *dest, uint32_t n)
{
    uint8_t *d = dest;
    while (n--) *d++ = 0;
}

static int equal(const void *a, const char *b, unsigned n)
{
    const char *p = a;
    while (n--) if (*p++ != *b++) return 0;
    return 1;
}

static int checksum(const void *a, unsigned n)
{
    const uint8_t *p = a;
    uint8_t sum = 0;
    while (n--) sum += *p++;
    return sum == 0;
}

static int overlaps(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    return a < b + bn && b < a + an;
}

/* Strictly require a complete usable interval, rejecting reserved overlays. */
static int usable(uint32_t base, uint32_t size)
{
    int result = idle_range_usable((void *)original->mmap_addr, original->mmap_length, base, size);
    if (result < 0) fail("malformed firmware memory map");
    return result;
}

static void require_ram(uint32_t base, uint32_t size)
{
    if (!size || (uint64_t)base + size > 0x100000000ULL || !usable(base, size)) {
        uart_puthex(base); uart_puts(" size="); uart_puthex(size); uart_puts("\n");
        fail("required interval is not firmware RAM");
    }
}

struct table {
    char signature[4];
    uint32_t length;
    uint8_t rest[28];
} __attribute__((packed));

static struct table *table_at(uint32_t addr)
{
    if (addr < 0x400 || addr > 0xffff0000u) fail("ACPI address");
    struct table *t = (void *)addr;
    if (t->length < sizeof(*t) || t->length > 65536 ||
        (uint64_t)addr + t->length > 0x100000000ULL || !checksum(t, t->length)) fail("ACPI checksum/length");
    return t;
}

static uint32_t scan_rsdp(uint32_t begin, uint32_t end)
{
    for (uint32_t p = begin; p + 20 <= end; p += 16)
        if (equal((void *)p, "RSD PTR ", 8) && checksum((void *)p, 20)) return p;
    return 0;
}

static void discover(unsigned count)
{
    uint16_t ebda_seg;
    /* BIOS data area is supplied by firmware, not a C object. */
    __asm__ volatile("movw 0x40e, %0" : "=r"(ebda_seg));
    uint32_t ebda = (uint32_t)ebda_seg << 4;
    uint32_t rsdp = ebda >= 0x80000 && ebda < 0xa0000 ? scan_rsdp(ebda, ebda + 1024) : 0;
    if (!rsdp) rsdp = scan_rsdp(0xe0000, 0x100000);
    if (!rsdp) fail("no BIOS RSDP");
    uint32_t rsdt_addr;
    copy(&rsdt_addr, (void *)(rsdp + 16), 4);
    struct table *rsdt = table_at(rsdt_addr);
    if (!equal(rsdt, "RSDT", 4) || (rsdt->length - 36) % 4) fail("RSDT required");
    struct table *madt = 0, *hpet = 0;
    for (uint32_t p = 36; p < rsdt->length; p += 4) {
        uint32_t addr;
        copy(&addr, (uint8_t *)rsdt + p, 4);
        struct table *t = table_at(addr);
        if (equal(t, "APIC", 4)) madt = t;
        if (equal(t, "HPET", 4)) hpet = t;
    }
    if (!madt || madt->length < 44 || !hpet || hpet->length < 56) fail("MADT/HPET required");
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    uint32_t bsp = b >> 24;
    uint32_t msr_lo, msr_hi;
    __asm__ volatile("rdmsr" : "=a"(msr_lo), "=d"(msr_hi) : "c"(0x1b));
    if (msr_hi || (msr_lo & 0xfffff000u) != 0xfee00000u || (msr_lo & (1u << 10)))
        fail("requires standard xAPIC mode");
    if (*(uint32_t *)((uint8_t *)madt + 36) != 0xfee00000u) fail("LAPIC base override");
    unsigned total = 1, saw_bsp = 0;
    shared->apic_id[0] = bsp;
    uint8_t seen[256];
    zero(seen, sizeof(seen));
    for (uint32_t off = 44; off < madt->length;) {
        uint8_t *p = (uint8_t *)madt + off;
        if (madt->length - off < 2 || p[1] < 2 || p[1] > madt->length - off) fail("MADT entry");
        if (p[0] == 5 || p[0] == 9) fail("unsupported LAPIC override/x2APIC table");
        if (p[0] == 0) {
            if (p[1] < 8) fail("short LAPIC entry");
            uint32_t flags;
            copy(&flags, p + 4, 4);
            if (flags & 1) {
                if (seen[p[3]]++) fail("duplicate APIC ID");
                if (p[3] == bsp) saw_bsp = 1;
                else if (total < 4) shared->apic_id[total++] = p[3];
            }
        }
        off += p[1];
    }
    if (!saw_bsp || total < count) fail("not enough enabled CPUs");
    uint8_t *hp = (uint8_t *)hpet;
    uint64_t address;
    copy(&address, hp + 44, 8);
    if (hp[40] != 0 || address < 0x100000 || address > 0xfffff000ULL || (address & 4095))
        fail("HPET must be page-aligned MMIO below 4GiB");
    shared->hpet_phys = (uint32_t)address;
    uart_puts("[loader] HPET="); uart_puthex(shared->hpet_phys); uart_puts("\n");
    for (unsigned i = 0; i < count; i++) {
        uart_puts("[loader] K"); uart_putdec(i); uart_puts(" APIC=");
        uart_putdec(shared->apic_id[i]); uart_puts("\n");
    }
}

static void mmap_entry(struct mb_mmap_entry *m, uint32_t base, uint32_t size)
{
    m->size = 20; m->base_addr = base; m->length = size; m->type = 1;
}

static void load(unsigned i)
{
    const struct mb_module *m = &modules[2 * i];
    uint32_t size = m->mod_end - m->mod_start;
    const struct elf32_ehdr *e = (void *)m->mod_start;
    if (size < sizeof(*e) || !equal(e, "\177ELF", 4) || e->e_ident[4] != 1 ||
        e->e_phentsize != sizeof(struct elf32_phdr) ||
        (uint64_t)e->e_phoff + (uint64_t)e->e_phnum * sizeof(struct elf32_phdr) > size)
        fail("kernel ELF bounds");
    const struct elf32_phdr *ph = (void *)(m->mod_start + e->e_phoff);
    uint32_t end = MVPQ_KLO_BASE(i);
    int entry_ok = 0;
    for (unsigned p = 0; p < e->e_phnum; p++) {
        if (ph[p].p_type != PT_LOAD) continue;
        uint64_t stop = (uint64_t)ph[p].p_paddr + ph[p].p_memsz;
        if (ph[p].p_filesz > ph[p].p_memsz ||
            (uint64_t)ph[p].p_offset + ph[p].p_filesz > size ||
            ph[p].p_paddr < MVPQ_KLO_BASE(i) || stop > MVPQ_KLO_BASE(i) + MVPQ_KLO_SIZE)
            fail("kernel segment outside its partition");
        for (unsigned q = 0; q < p; q++)
            if (ph[q].p_type == PT_LOAD && overlaps(ph[p].p_paddr, ph[p].p_memsz,
                                                    ph[q].p_paddr, ph[q].p_memsz)) fail("overlapping ELF segments");
        if (e->e_entry >= ph[p].p_paddr && e->e_entry < stop) entry_ok = 1;
        if (stop > end) end = (uint32_t)stop;
    }
    if (!entry_ok) fail("kernel entry outside load segments");
    root_at[i] = (end + 8191u) & ~4095u;
    uint32_t rs_size = modules[2 * i + 1].mod_end - modules[2 * i + 1].mod_start;
    /* Leave 16MiB for rootserver ELF expansion and initial kernel allocations. */
    if ((uint64_t)root_at[i] + rs_size + 0x1000000 > MVPQ_KLO_BASE(i) + MVPQ_KLO_SIZE)
        fail("rootserver too large");
    for (unsigned p = 0; p < e->e_phnum; p++) {
        if (ph[p].p_type != PT_LOAD) continue;
        copy((void *)ph[p].p_paddr, (void *)(m->mod_start + ph[p].p_offset), ph[p].p_filesz);
        zero((void *)(ph[p].p_paddr + ph[p].p_filesz), ph[p].p_memsz - ph[p].p_filesz);
    }
    copy((void *)root_at[i], (void *)modules[2 * i + 1].mod_start, rs_size);
    entries[i] = e->e_entry;
    root[i].mod_start = root_at[i]; root[i].mod_end = root_at[i] + rs_size;
    mmap_entry(&maps[i][0], MVPQ_KLO_BASE(i), MVPQ_KLO_SIZE);
    mmap_entry(&maps[i][1], MVPQ_KHI_BASE(i), MVPQ_KHI_SIZE);
    mmap_entry(&maps[i][2], MVPQ_GUARD_LO, MVPQ_GUARD_SIZE);
    mmap_entry(&maps[i][3], MVPQ_GUARD_HI, MVPQ_GUARD_SIZE);
    info[i].flags = MULTIBOOT_INFO_MODS | MULTIBOOT_INFO_MEM_MAP | MULTIBOOT_INFO_MEMORY;
    info[i].mem_lower = 640;
    info[i].mem_upper = (MVPQ_KLO_SIZE + MVPQ_KHI_SIZE) / 1024;
    info[i].mods_count = 1; info[i].mods_addr = (uint32_t)&root[i];
    info[i].mmap_length = sizeof(maps[i]); info[i].mmap_addr = (uint32_t)maps[i];
}

static volatile uint32_t *slot(uint32_t *s)
{
    return (void *)(MVPQ_TRAMPOLINE_PADDR + (uint32_t)((char *)s - ap_trampoline_start));
}

void main(uint32_t magic, uint32_t mbi_addr)
{
    uart_init(); uart_puts("\n[loader] Optiplex 7040 idle v1\n");
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) fail("not Multiboot1");
    original = (void *)mbi_addr;
    if ((original->flags & (MULTIBOOT_INFO_MODS | MULTIBOOT_INFO_MEM_MAP)) !=
        (MULTIBOOT_INFO_MODS | MULTIBOOT_INFO_MEM_MAP) ||
        original->mods_count < 2 || original->mods_count > 8 || original->mods_count % 2)
        fail("requires memory map and kernel/rootserver pairs");
    unsigned count = original->mods_count / 2;
    require_ram(MVPQ_TRAMPOLINE_PADDR, 4096);
    require_ram(MVPQ_GUARD_LO, MVPQ_SHARED_SIZE + 2 * MVPQ_GUARD_SIZE);
    for (unsigned i = 0; i < count; i++) {
        require_ram(MVPQ_KLO_BASE(i), MVPQ_KLO_SIZE);
        require_ram(MVPQ_KHI_BASE(i), MVPQ_KHI_SIZE);
    }
    /* Stage modules outside every destination before any ELF relocation. */
    copy(modules, (void *)original->mods_addr, original->mods_count * sizeof(modules[0]));
    uint32_t total = 0;
    for (unsigned i = 0; i < count * 2; i++) {
        if (modules[i].mod_end <= modules[i].mod_start) fail("empty module");
        uint32_t size = modules[i].mod_end - modules[i].mod_start;
        if (size > 0x1000000 || total > 0x08000000 - size - 4096) fail("module staging capacity");
        if (overlaps(modules[i].mod_start, size, (uint32_t)_text, (uint32_t)(_end - _text)) ||
            overlaps(modules[i].mod_start, size, IDLE_SHARED_PHYS - 8192, MVPQ_SHARED_SIZE + 16384) ||
            overlaps(modules[i].mod_start, size, MVPQ_TRAMPOLINE_PADDR, 4096)) fail("module overlaps loader/control");
        total += (size + 4095) & ~4095u;
    }
    require_ram(0x80000000, total);
    for (unsigned i = 0; i < count * 2; i++)
        if (overlaps(modules[i].mod_start, modules[i].mod_end - modules[i].mod_start,
                     0x80000000, total)) fail("staging overlaps source modules");
    zero(shared, sizeof(*shared));
    shared->version = 1; shared->count = count;
    discover(count);
    uint32_t dest = 0x80000000;
    for (unsigned i = 0; i < count * 2; i++) {
        uint32_t size = modules[i].mod_end - modules[i].mod_start;
        copy((void *)dest, (void *)modules[i].mod_start, size);
        modules[i].mod_start = dest; modules[i].mod_end = dest + size;
        dest += (size + 4095) & ~4095u;
    }
    for (unsigned i = 0; i < count; i++) load(i);
    shared->magic = IDLE_BOOT_MAGIC;
    if ((uint32_t)(ap_trampoline_end - ap_trampoline_start) > 2048) fail("trampoline size");
    copy((void *)MVPQ_TRAMPOLINE_PADDR, ap_trampoline_start,
         (uint32_t)(ap_trampoline_end - ap_trampoline_start));
    /* Wait for userspace, not just trampoline ack: PIT calibration is shared. */
    for (unsigned i = 1; i < count; i++) {
        *slot(&ap_kentry) = entries[i]; *slot(&ap_kmbi) = (uint32_t)&info[i]; *slot(&ap_ack) = 0;
        lapic_send_init_sipi(shared->apic_id[i], MVPQ_TRAMPOLINE_PADDR);
        unsigned spins = 0;
        while (!__atomic_load_n(&shared->prepared[i], __ATOMIC_ACQUIRE)) {
            if (++spins == 2000000000u) fail("AP userspace startup timeout");
            __asm__ volatile("pause");
        }
    }
    uart_puts("[loader] BSP entering K0\n");
    __asm__ volatile("movl %0, %%eax; movl %1, %%ebx; jmp *%2"
        :: "i"(MULTIBOOT_BOOTLOADER_MAGIC), "r"(&info[0]), "r"(entries[0]) : "eax", "ebx");
    __builtin_unreachable();
}
