/*
 * Copyright 2024, seL4 project maintainers
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <autoconf.h>
#undef CONFIG_MAX_NUM_NODES
#include <elfloader/gen_config.h>
#include <elfloader_common.h>
#include <devices_gen.h>
#include <drivers/common.h>

#include <printf.h>
#include <types.h>

#define SPI_START         32u
#define GIC_REG_WIDTH   32
#define GICD_TYPE_LINESNR 0x01f
#define IRQ_SET_ALL 0xffffffff
#ifdef CONFIG_ARM_HYPERVISOR_SUPPORT
#define GIC_PRI_IRQ        0x80
#else
#define GIC_PRI_IRQ        0x00
#endif

/* Memory map for GIC distributor */
struct gic_dist_map {
    uint32_t enable;                /* 0x000 */
    uint32_t ic_type;               /* 0x004 */
    uint32_t dist_ident;            /* 0x008 */
    uint32_t res1[29];              /* [0x00C, 0x080) */

    uint32_t security[32];          /* [0x080, 0x100) */

    uint32_t enable_set[32];        /* [0x100, 0x180) */
    uint32_t enable_clr[32];        /* [0x180, 0x200) */
    uint32_t pending_set[32];       /* [0x200, 0x280) */
    uint32_t pending_clr[32];       /* [0x280, 0x300) */
    uint32_t active[32];            /* [0x300, 0x380) */
    uint32_t res2[32];              /* [0x380, 0x400) */

    uint32_t priority[255];         /* [0x400, 0x7FC) */
    uint32_t res3;                  /* 0x7FC */

    uint32_t targets[255];            /* [0x800, 0xBFC) */
    uint32_t res4;                  /* 0xBFC */

    uint32_t config[64];             /* [0xC00, 0xD00) */

    uint32_t spi[32];               /* [0xD00, 0xD80) */
    uint32_t res5[20];              /* [0xD80, 0xDD0) */
    uint32_t res6;                  /* 0xDD0 */
    uint32_t legacy_int;            /* 0xDD4 */
    uint32_t res7[2];               /* [0xDD8, 0xDE0) */
    uint32_t match_d;               /* 0xDE0 */
    uint32_t enable_d;              /* 0xDE4 */
    uint32_t res8[70];               /* [0xDE8, 0xF00) */

    uint32_t sgi_control;           /* 0xF00 */
    uint32_t res9[3];               /* [0xF04, 0xF10) */
    uint32_t sgi_pending_clr[4];    /* [0xF10, 0xF20) */
    uint32_t res10[40];             /* [0xF20, 0xFC0) */

    uint32_t periph_id[12];         /* [0xFC0, 0xFF0) */
    uint32_t component_id[4];       /* [0xFF0, 0xFFF] */
};


static int gic_v2_init(struct elfloader_device *dev,
                          UNUSED void *match_data)
{

	// Track whether initialization has happened incase this is called again.
	static int initialized = 0;
	if (initialized) return 0;
    volatile struct gic_dist_map *dist = dev->region_bases[0];

    uint32_t ctlr = dist->enable;
    const uint32_t ctlr_mask = BIT(0);
    if ((ctlr & ctlr_mask) != ctlr_mask) {

        printf("GICv2: GICD_CTLR 0x%x -> 0x%x (Enabling GIC distributor)\n", ctlr, ctlr | ctlr_mask);
        dist->enable = ctlr | ctlr_mask;
    }

    uint32_t type = dist->ic_type;
    unsigned int nr_lines = GIC_REG_WIDTH * ((type & GICD_TYPE_LINESNR) + 1);

    /* Disable and clear all global interrupts */
	word_t i;
    for (i = SPI_START; i < nr_lines; i += 32) {
        dist->enable_clr[(i / 32)] = IRQ_SET_ALL;
        dist->pending_clr[(i / 32)] = IRQ_SET_ALL;
     }

    /* level-triggered, 1-N */
    for (i = SPI_START; i < nr_lines; i += 16) {
        dist->config[(i / 16)] = 0x55555555; 
    }

    /* group 0 for secure; group 1 for non-secure */
    for (i = SPI_START; i < nr_lines; i += 32) {
#if defined(CONFIG_ARM_HYPERVISOR_SUPPORT) && !defined(CONFIG_PLAT_QEMU_ARM_VIRT)
        dist->security[i >> 5] = 0xffffffff;
#else
        dist->security[i >> 5] = 0;
#endif
    }

    /* Default priority for global interrupts */
    uint32_t priority = (GIC_PRI_IRQ << 24 | GIC_PRI_IRQ << 16 | GIC_PRI_IRQ << 8 |
                GIC_PRI_IRQ);
    for (i = SPI_START; i < nr_lines; i += 4) {
        dist->priority[(i / 4)] = priority;
    }


    initialized = 1;
    return 0;
}

static const struct dtb_match_table gic_v2_matches[] = {
    { .compatible = "arm,cortex-a15-gic" },
    { .compatible = NULL /* sentinel */ },
};

static const struct elfloader_driver gic_v2 = {
    .match_table = gic_v2_matches,
    .type = DRIVER_IRQ,
    .init = &gic_v2_init,
    .ops = NULL,
};

ELFLOADER_DRIVER(gic_v2);
