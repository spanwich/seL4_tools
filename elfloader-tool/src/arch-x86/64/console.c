/*
 * Copyright 2026, PhD Research Project — multikernel-AMP MVP-Q (x86_64)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * COM1 16550 UART polled output. Used by printf for elfloader diagnostics.
 * No IRQs, no FIFO management — pure busy-wait on the THRE bit.
 */

#include <stdint.h>

#define COM1_BASE   0x3F8
#define UART_THR    (COM1_BASE + 0)  /* transmit holding register */
#define UART_DLL    (COM1_BASE + 0)  /* divisor latch low (when DLAB=1) */
#define UART_IER    (COM1_BASE + 1)  /* interrupt enable */
#define UART_DLH    (COM1_BASE + 1)  /* divisor latch high (when DLAB=1) */
#define UART_FCR    (COM1_BASE + 2)  /* FIFO control */
#define UART_LCR    (COM1_BASE + 3)  /* line control */
#define UART_MCR    (COM1_BASE + 4)  /* modem control */
#define UART_LSR    (COM1_BASE + 5)  /* line status */
#define LSR_THRE    0x20             /* transmit holding register empty */

static inline void outb(uint16_t port, uint8_t value)
{
    asm volatile("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void uart_init(void)
{
    /* Disable IRQs */
    outb(UART_IER, 0x00);
    /* DLAB=1 to set baud divisor */
    outb(UART_LCR, 0x80);
    /* 115200 baud (divisor = 1 with 1.8432 MHz clock) */
    outb(UART_DLL, 0x01);
    outb(UART_DLH, 0x00);
    /* 8N1, DLAB=0 */
    outb(UART_LCR, 0x03);
    /* Enable FIFO, clear, 14-byte threshold */
    outb(UART_FCR, 0xC7);
    /* DTR + RTS asserted; OUT2 for IRQ routing (unused here) */
    outb(UART_MCR, 0x0B);
}

void uart_putc(char c)
{
    while ((inb(UART_LSR) & LSR_THRE) == 0) {
        /* spin until transmit holding register is empty */
    }
    outb(UART_THR, (uint8_t)c);
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

void uart_puthex(uint32_t v)
{
    static const char hexd[] = "0123456789abcdef";
    uart_putc('0');
    uart_putc('x');
    for (int shift = 28; shift >= 0; shift -= 4) {
        uart_putc(hexd[(v >> shift) & 0xF]);
    }
}

void uart_putdec(uint32_t v)
{
    if (v == 0) {
        uart_putc('0');
        return;
    }
    char buf[24];
    int i = 0;
    while (v && i < (int)sizeof(buf)) {
        buf[i++] = '0' + (v % 10);
        v /= 10;
    }
    while (i--) {
        uart_putc(buf[i]);
    }
}
