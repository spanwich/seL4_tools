/*
 * Copyright 2026, PhD Research Project — multikernel-AMP MVP-Q (x86_64)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Minimal Multiboot v1 info structures. Just enough to parse what QEMU
 * passes us and synthesize what the seL4 kernel expects.
 */

#ifndef _MULTIBOOT_H_
#define _MULTIBOOT_H_

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC   0x2BADB002

#define MULTIBOOT_INFO_MEMORY        0x00000001
#define MULTIBOOT_INFO_BOOTDEV       0x00000002
#define MULTIBOOT_INFO_CMDLINE       0x00000004
#define MULTIBOOT_INFO_MODS          0x00000008
#define MULTIBOOT_INFO_MEM_MAP       0x00000040

struct mb_mmap_entry {
    uint32_t size;        /* size of this entry MINUS the size field (= 20) */
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;        /* 1 = usable RAM, 2 = reserved, 3 = ACPI, 4 = NVS */
} __attribute__((packed));

struct mb_module {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t cmdline;
    uint32_t pad;
} __attribute__((packed));

struct mb_info {
    uint32_t flags;
    /* INFO_MEMORY */
    uint32_t mem_lower;
    uint32_t mem_upper;
    /* INFO_BOOTDEV */
    uint32_t boot_device;
    /* INFO_CMDLINE */
    uint32_t cmdline;
    /* INFO_MODS */
    uint32_t mods_count;
    uint32_t mods_addr;
    /* symbol info (unused) */
    uint32_t syms[4];
    /* INFO_MEM_MAP */
    uint32_t mmap_length;
    uint32_t mmap_addr;
    /* rest unused */
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6];
} __attribute__((packed));

#endif /* _MULTIBOOT_H_ */
