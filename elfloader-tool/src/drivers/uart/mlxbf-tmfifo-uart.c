/*
 * Copyright 2026, PhD Research Project
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <devices_gen.h>
#include <drivers/common.h>
#include <drivers/uart.h>

#include <elfloader_common.h>

#define MLXBF_TMFIFO_TX_DATA       0x00u
#define MLXBF_TMFIFO_TX_STS        0x08u
#define MLXBF_TMFIFO_TX_CTL        0x10u
#define MLXBF_TMFIFO_TX_STS_COUNT  0x1ffu
#define MLXBF_TMFIFO_TX_CTL_MAX_ENTRIES_SHIFT 32u
#define MLXBF_TMFIFO_TX_CTL_MAX_ENTRIES_MASK  0x1ffu
#define MLXBF_TMFIFO_MIN_TX_WORDS  2u
#define MLXBF_TMFIFO_WAIT_LIMIT    10000000u
#define MLXBF_TMFIFO_VIRTIO_ID_CONSOLE 3ull

#define TMFIFO_REG64(mmio, x) ((volatile uint64_t *)((uintptr_t)(mmio) + (x)))

static uint32_t mlxbf_tmfifo_tx_fifo_size(volatile void *mmio)
{
    uint64_t ctl = *TMFIFO_REG64(mmio, MLXBF_TMFIFO_TX_CTL);
    uint32_t size = (uint32_t)((ctl >> MLXBF_TMFIFO_TX_CTL_MAX_ENTRIES_SHIFT) &
                               MLXBF_TMFIFO_TX_CTL_MAX_ENTRIES_MASK);

    return size ? size : 256u;
}

static uint32_t mlxbf_tmfifo_tx_avail(volatile void *mmio)
{
    uint32_t size = mlxbf_tmfifo_tx_fifo_size(mmio);
    uint64_t sts = *TMFIFO_REG64(mmio, MLXBF_TMFIFO_TX_STS);
    uint32_t used = (uint32_t)(sts & MLXBF_TMFIFO_TX_STS_COUNT);

    if (used >= size - 1u) {
        return 0;
    }

    return size - 1u - used;
}


static void mlxbf_tmfifo_putchar_mmio(volatile void *mmio, unsigned int c)
{
    uint64_t header;
    uint64_t payload = c & 0xffu;
    uint32_t wait;

    for (wait = 0; wait < MLXBF_TMFIFO_WAIT_LIMIT; wait++) {
        if (mlxbf_tmfifo_tx_avail(mmio) >= MLXBF_TMFIFO_MIN_TX_WORDS) {
            break;
        }
    }

    if (wait == MLXBF_TMFIFO_WAIT_LIMIT) {
        return;
    }

    header = MLXBF_TMFIFO_VIRTIO_ID_CONSOLE | (1ull << 16);
    *TMFIFO_REG64(mmio, MLXBF_TMFIFO_TX_DATA) = header;
    *TMFIFO_REG64(mmio, MLXBF_TMFIFO_TX_DATA) = payload;
}

void bf2_early_tmfifo_puts(const char *s)
{
    volatile void *mmio = (volatile void *)(uintptr_t)0x800a40;

    while (*s) {
        if (*s == '\n') {
            mlxbf_tmfifo_putchar_mmio(mmio, '\r');
        }
        mlxbf_tmfifo_putchar_mmio(mmio, (unsigned int)*s++);
    }
    mlxbf_tmfifo_putchar_mmio(mmio, '\r');
    mlxbf_tmfifo_putchar_mmio(mmio, '\n');
}

static int mlxbf_tmfifo_putchar(struct elfloader_device *dev, unsigned int c)
{
    mlxbf_tmfifo_putchar_mmio(dev->region_bases[1], c);
    return 0;
}

static int mlxbf_tmfifo_init(struct elfloader_device *dev, UNUSED void *match_data)
{
    if (!dev->region_bases[1]) {
        return -1;
    }
    uart_set_out(dev);
    return 0;
}

static const struct dtb_match_table mlxbf_tmfifo_matches[] = {
    { .compatible = "mellanox,bluefield-tmfifo" },
    { .compatible = NULL /* sentinel */ },
};

static const struct elfloader_uart_ops mlxbf_tmfifo_ops = {
    .putc = &mlxbf_tmfifo_putchar,
};

static const struct elfloader_driver mlxbf_tmfifo = {
    .match_table = mlxbf_tmfifo_matches,
    .type = DRIVER_UART,
    .init = &mlxbf_tmfifo_init,
    .ops = &mlxbf_tmfifo_ops,
};

ELFLOADER_DRIVER(mlxbf_tmfifo);
