/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

struct idle_mmap_entry {
    uint32_t size;
    uint64_t base;
    uint64_t length;
    uint32_t type;
} __attribute__((packed));

/* Multiboot map validation: 1 = wholly usable, 0 = unavailable, -1 = malformed. */
static inline int idle_range_usable(const void *map, uint32_t length, uint32_t base, uint32_t size)
{
    uint32_t off = 0;
    int covered = 0, reserved = 0;
    if (!size || (uint64_t)base + size > 0x100000000ULL) return 0;
    while (off < length) {
        if (length - off < sizeof(struct idle_mmap_entry)) return -1;
        const struct idle_mmap_entry *m = (const void *)((const uint8_t *)map + off);
        if (m->size < 20 || m->size > length - off - 4 || m->base + m->length < m->base) return -1;
        if (m->type != 1 && base < m->base + m->length && m->base < (uint64_t)base + size) reserved = 1;
        if (m->type == 1 && m->base <= base && m->base + m->length >= (uint64_t)base + size) covered = 1;
        off += m->size + 4;
    }
    return covered && !reserved;
}
