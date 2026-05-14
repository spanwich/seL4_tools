/*
 * Copyright 2026, PhD Research Project — multikernel-AMP MVP-Q (x86_64)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * x86_64 elfloader main entry. Sub-step 1: just prove we got here via the
 * 32→64 mode transition and that COM1 output works. Loading the kernel ELF
 * from the CPIO archive and synthesizing an MBI for it comes in sub-step 2.
 */

#include <stdint.h>

void uart_init(void);
void uart_puts(const char *s);
void uart_puthex(uint32_t v);

/*
 * crt0.S calls main(mb_magic, mb_info_paddr) via cdecl after BSS clear.
 * Multiboot v1 magic delivered to us by the bootloader is 0x2BADB002.
 */
void main(uint32_t mb_magic, uint32_t mb_info_paddr)
{
    uart_init();
    uart_puts("\n[elfloader] x86_64 multikernel-AMP elfloader (sub-step 1)\n");
    uart_puts("[elfloader] arrived at C main: OK\n");
    uart_puts("[elfloader] multiboot magic = ");
    uart_puthex(mb_magic);
    uart_puts("\n[elfloader] multiboot info  = ");
    uart_puthex(mb_info_paddr);
    uart_puts("\n[elfloader] halting (kernel load not yet implemented).\n");

    for (;;) {
        asm volatile("cli; hlt");
    }
}
