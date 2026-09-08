/*
 * Copyright 2026, PhD Research Project -- multikernel-AMP
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Minimal TX-only console for the STM32 USART (st,stm32h7-uart), as used for
 * USART2 @ 0x400e0000 on the Digi CCMP25-DVK (STM32MP25x).
 *
 * The elfloader was previously silent on this platform: devices_gen.h lists the
 * serial node (compat "st,stm32h7-uart", base 0x400e0000) but no driver matched
 * it, so uart_set_out() was never called and every elfloader printf() was a
 * no-op. That hid the multikernel PSCI dispatch trace. This driver wires it up.
 *
 * U-Boot has already configured baud rate and enabled the UART before handing
 * off (we see U-Boot + kernel output on this same port), so we only poll the
 * TX-empty flag and write the data register -- no re-init.
 */

#include <devices_gen.h>
#include <drivers/common.h>
#include <drivers/uart.h>

#include <elfloader_common.h>

/* STM32 F7/H7/MP USART register offsets (the "new" peripheral layout). */
#define USART_ISR       0x1C    /* interrupt and status register */
#define USART_TDR       0x28    /* transmit data register */

/* USART_ISR bits */
#define USART_ISR_TXE   (1U << 7)   /* TX data register empty (a.k.a. TXFNF) */

#define UART_REG(mmio, x) ((volatile uint32_t *)((uintptr_t)(mmio) + (x)))

static int stm32_uart_putchar(struct elfloader_device *dev, unsigned int c)
{
    volatile void *mmio = dev->region_bases[0];

    /* Wait until the transmit data register can accept a byte. */
    while (!(*UART_REG(mmio, USART_ISR) & USART_ISR_TXE)) {
        /* spin */
    }

    *UART_REG(mmio, USART_TDR) = (uint32_t)(c & 0xff);

    return 0;
}

static int stm32_uart_init(struct elfloader_device *dev, UNUSED void *match_data)
{
    uart_set_out(dev);
    return 0;
}

static const struct dtb_match_table stm32_uart_matches[] = {
    { .compatible = "st,stm32h7-uart" },
    { .compatible = "st,stm32f7-uart" },
    { .compatible = NULL /* sentinel */ },
};

static const struct elfloader_uart_ops stm32_uart_ops = {
    .putc = &stm32_uart_putchar,
};

static const struct elfloader_driver stm32_uart = {
    .match_table = stm32_uart_matches,
    .type = DRIVER_UART,
    .init = &stm32_uart_init,
    .ops = &stm32_uart_ops,
};

ELFLOADER_DRIVER(stm32_uart);
