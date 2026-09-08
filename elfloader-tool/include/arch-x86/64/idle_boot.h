/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

#define IDLE_BOOT_MAGIC 0x70404944u
#define IDLE_SHARED_PHYS 0x90000000u
#define IDLE_MAX_KERNELS 4

/* Private loader/rootserver ABI. One cache-coherent page in reserved RAM. */
struct idle_boot {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t hpet_phys;
    uint32_t apic_id[IDLE_MAX_KERNELS];
    uint32_t prepared[IDLE_MAX_KERNELS];
    uint32_t timer_ready;
    uint32_t console_lock;
};
