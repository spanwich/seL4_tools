/*
 * Copyright 2026, PhD Research Project — multikernel-AMP MVP-Q (x86_64)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Minimal ELF32 structures for parsing the seL4 x86_64 kernel image. The
 * kernel ELF is objcopy'd from elf64-x86-64 to elf32-i386 by the seL4
 * build (see DeclareRootserver in tools/seL4/cmake-tool/helpers/rootserver.cmake:61),
 * so all the addresses fit in 32 bits and we can use ELF32 structs directly.
 */

#ifndef _ELF32_H_
#define _ELF32_H_

#include <stdint.h>

#define ELF_MAGIC0  0x7F
#define ELF_MAGIC1  'E'
#define ELF_MAGIC2  'L'
#define ELF_MAGIC3  'F'

#define PT_NULL     0
#define PT_LOAD     1

struct elf32_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed));

#endif /* _ELF32_H_ */
