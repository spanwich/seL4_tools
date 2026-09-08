/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <abort.h>
#include <types.h>
#include <printf.h>

static uint64_t read_currentel(void)
{
    uint64_t val;
    asm volatile("mrs %0, CurrentEL" : "=r"(val));
    return val;
}

#define read_sysreg(_r) ({                 \
        uint64_t val;                      \
        asm volatile("mrs %0," #_r : "=r"(val)); \
        val;                               \
    })

static const char *vector_name(unsigned long vector)
{
    switch (vector) {
    case 0: return "sync EL1t";
    case 1: return "IRQ EL1t";
    case 2: return "FIQ EL1t";
    case 3: return "SError EL1t";
    case 4: return "sync EL1h";
    case 5: return "IRQ EL1h";
    case 6: return "FIQ EL1h";
    case 7: return "SError EL1h";
    case 8: return "sync EL0 64";
    case 9: return "IRQ EL0 64";
    case 10: return "FIQ EL0 64";
    case 11: return "SError EL0 64";
    case 12: return "sync EL0 32";
    case 13: return "IRQ EL0 32";
    case 14: return "FIQ EL0 32";
    case 15: return "SError EL0 32";
    default: return "unknown";
    }
}

void invalid_vector_entry(unsigned long vector)
{
    uint64_t currentel = read_currentel();

    printf("ELF-LOADER: Invalid exception received: vector=%lu (%s) CurrentEL=%lx\n",
           vector, vector_name(vector), currentel);
    if (currentel == (2u << 2)) {
        printf("esr_el2: %lx\n", read_sysreg(esr_el2));
        printf("elr_el2: %lx\n", read_sysreg(elr_el2));
        printf("spsr_el2: %lx\n", read_sysreg(spsr_el2));
        printf("far_el2: %lx\n", read_sysreg(far_el2));
        printf("vbar_el2: %lx\n", read_sysreg(vbar_el2));
    } else {
        printf("esr_el1: %lx\n", read_sysreg(esr_el1));
        printf("elr_el1: %lx\n", read_sysreg(elr_el1));
        printf("spsr_el1: %lx\n", read_sysreg(spsr_el1));
        printf("far_el1: %lx\n", read_sysreg(far_el1));
        printf("vbar_el1: %lx\n", read_sysreg(vbar_el1));
    }
    abort();
}

void el1_sync(void)
{
    uint64_t currentel = read_currentel();

    printf("ELF-LOADER: Synchronous exception received: CurrentEL=%lx\n", currentel);
    if (currentel == (2u << 2)) {
        printf("esr_el2: %lx\n", read_sysreg(esr_el2));
        printf("elr_el2: %lx\n", read_sysreg(elr_el2));
        printf("spsr_el2: %lx\n", read_sysreg(spsr_el2));
        printf("far_el2: %lx\n", read_sysreg(far_el2));
        printf("vbar_el2: %lx\n", read_sysreg(vbar_el2));
    } else {
        printf("esr_el1: %lx\n", read_sysreg(esr_el1));
        printf("elr_el1: %lx\n", read_sysreg(elr_el1));
        printf("spsr_el1: %lx\n", read_sysreg(spsr_el1));
        printf("far_el1: %lx\n", read_sysreg(far_el1));
        printf("vbar_el1: %lx\n", read_sysreg(vbar_el1));
    }
    abort();
}
