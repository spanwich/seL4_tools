/*
 * Copyright 2026, PhD Research Project — multikernel-AMP MVP-Q (x86_64)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Local APIC SIPI sequencer. Wakes an AP from its INIT state at a chosen
 * SIPI vector. Intel SDM Vol. 3A §10.4 / §10.5.
 *
 * Flow:
 *   1. Send INIT IPI to target APIC ID.
 *   2. Wait ~10 ms.
 *   3. Send STARTUP IPI (vector = trampoline_paddr >> 12).
 *   4. Wait ~200 us.
 *   5. Send STARTUP IPI again (per the SDM-recommended belt-and-braces).
 *
 * LAPIC is memory-mapped at LAPIC_BASE (0xFEE00000 — the standard BIOS
 * default; we don't reprogram IA32_APIC_BASE_MSR).
 */

#include <stdint.h>

void uart_puts(const char *s);
void uart_puthex(uint32_t v);

#define LAPIC_BASE          0xFEE00000u
#define LAPIC_ICR_LO        (LAPIC_BASE + 0x300)
#define LAPIC_ICR_HI        (LAPIC_BASE + 0x310)

/* ICR low-half encoding (Intel SDM Vol. 3A §10.6.1) */
#define ICR_DELIVERY_INIT   (0x5 << 8)
#define ICR_DELIVERY_STARTUP (0x6 << 8)
#define ICR_LEVEL_ASSERT    (1 << 14)
#define ICR_TRIGGER_EDGE    (0 << 15)

static inline void mmio_w32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

static inline uint32_t mmio_r32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

/* Busy-wait roughly N microseconds via in/out to the unused 0x80 port,
 * a classic technique that gives ~1us per outb on real hardware and is
 * functionally a delay under KVM/TCG. */
static void busy_wait_us(uint32_t us)
{
    while (us--) {
        asm volatile("outb %%al, $0x80" :: "a"((uint8_t)0));
    }
}

/* Spin until the ICR's Delivery Status bit clears (bit 12 of ICR low). */
static void icr_wait(void)
{
    while (mmio_r32(LAPIC_ICR_LO) & (1u << 12)) {
        asm volatile("pause");
    }
}

void lapic_send_init_sipi(uint8_t apic_id, uint32_t trampoline_paddr)
{
    uart_puts("[lapic] INIT IPI to APIC ");
    uart_puthex(apic_id);
    uart_puts("\n");

    /* INIT IPI: write high (target) first, then low (kicks off the send). */
    mmio_w32(LAPIC_ICR_HI, ((uint32_t)apic_id) << 24);
    mmio_w32(LAPIC_ICR_LO, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE);
    icr_wait();
    busy_wait_us(10000);  /* ~10 ms */

    uint32_t vector = (trampoline_paddr >> 12) & 0xFF;

    uart_puts("[lapic] STARTUP IPI vec=");
    uart_puthex(vector);
    uart_puts("\n");

    /* STARTUP IPI 1. */
    mmio_w32(LAPIC_ICR_HI, ((uint32_t)apic_id) << 24);
    mmio_w32(LAPIC_ICR_LO, ICR_DELIVERY_STARTUP | ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE | vector);
    icr_wait();
    busy_wait_us(200);    /* ~200 us */

    /* STARTUP IPI 2 (SDM-recommended retry for robustness). */
    mmio_w32(LAPIC_ICR_HI, ((uint32_t)apic_id) << 24);
    mmio_w32(LAPIC_ICR_LO, ICR_DELIVERY_STARTUP | ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE | vector);
    icr_wait();
}
