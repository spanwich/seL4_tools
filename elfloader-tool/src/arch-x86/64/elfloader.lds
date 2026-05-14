/*
 * Copyright 2026, PhD Research Project — multikernel-AMP MVP-Q (x86_64)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * x86_64 elfloader linker script. Output is elf64-x86-64 but the entry path
 * runs in 32-bit protected mode (.boot.text uses .code32). Multiboot v1
 * header is the first thing in the file so the bootloader can find the magic
 * within the 8 KiB window.
 *
 * The elfloader image is placed at IMAGE_START_ADDR (passed via CMake — we
 * use 0x100000 / 1 MiB, the conventional Multiboot kernel landing pad).
 */

#include "image_start_addr.h"

OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
    . = IMAGE_START_ADDR;
    _text = .;

    /* Multiboot header first — within 8 KiB, 4-byte aligned. */
    .multiboot : ALIGN(4)
    {
        KEEP(*(.multiboot))
    }

    /* All code (32-bit protected mode). The .ap_trampoline section holds
     * the SIPI trampoline that the BSP copies to phys 0x8000 at runtime.
     * It must be readable by the BSP, so it lands in the load segment. */
    .text : ALIGN(16)
    {
        *(.text)
        *(.text.*)
        *(.ap_trampoline)
    }

    .rodata : ALIGN(16)
    {
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(16);
        _archive_start = .;
        *(._archive_cpio)
        _archive_end = .;
    }

    .data : ALIGN(16)
    {
        *(.data)
        *(.data.*)
    }

    .bss (NOLOAD) : ALIGN(16)
    {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(16);
        _bss_end = .;
    }

    _end = .;

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.note.*)
        *(.comment)
    }
}
