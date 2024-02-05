/*
 * Copyright 2024, seL4 project maintainers
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <elfloader_common.h>
#include <devices_gen.h>
#include <drivers/common.h>

#include <printf.h>
#include <types.h>

/* Register bits */
#define GICD_CTLR_RWP                BIT(31)
#define GICD_CTLR_ARE_NS             BIT(4)
#define GICD_CTLR_ENABLE_G1NS         BIT(1)
#define GICD_CTLR_ENABLE_G0          BIT(0)

#define GICD_TYPE_LINESNR 0x01f

#define GIC_REG_WIDTH   32

/* Shared Peripheral Interrupts */
#define SPI_START         32u
#define GIC_PRI_IRQ        0xa0
#define IRQ_SET_ALL 0xffffffff


/* Memory map for GIC distributor */
struct gic_dist_map {
    uint32_t ctlr;                /* 0x0000 */
    uint32_t typer;               /* 0x0004 */
    uint32_t iidr;                /* 0x0008 */
    uint32_t res0;                /* 0x000C */
    uint32_t statusr;             /* 0x0010 */
    uint32_t res1[11];            /* [0x0014, 0x0040) */
    uint32_t setspi_nsr;          /* 0x0040 */
    uint32_t res2;                /* 0x0044 */
    uint32_t clrspi_nsr;          /* 0x0048 */
    uint32_t res3;                /* 0x004C */
    uint32_t setspi_sr;           /* 0x0050 */
    uint32_t res4;                /* 0x0054 */
    uint32_t clrspi_sr;           /* 0x0058 */
    uint32_t res5[9];             /* [0x005C, 0x0080) */
    uint32_t igrouprn[32];        /* [0x0080, 0x0100) */

    uint32_t isenablern[32];        /* [0x100, 0x180) */
    uint32_t icenablern[32];        /* [0x180, 0x200) */
    uint32_t ispendrn[32];          /* [0x200, 0x280) */
    uint32_t icpendrn[32];          /* [0x280, 0x300) */
    uint32_t isactivern[32];        /* [0x300, 0x380) */
    uint32_t icactivern[32];        /* [0x380, 0x400) */

    uint32_t ipriorityrn[255];      /* [0x400, 0x7FC) */
    uint32_t res6;                  /* 0x7FC */

    uint32_t itargetsrn[254];       /* [0x800, 0xBF8) */
    uint32_t res7[2];               /* 0xBF8 */

    uint32_t icfgrn[64];            /* [0xC00, 0xD00) */
    uint32_t igrpmodrn[64];         /* [0xD00, 0xE00) */
    uint32_t nsacrn[64];            /* [0xE00, 0xF00) */
    uint32_t sgir;                  /* 0xF00 */
    uint32_t res8[3];               /* [0xF04, 0xF10) */
    uint32_t cpendsgirn[4];         /* [0xF10, 0xF20) */
    uint32_t spendsgirn[4];         /* [0xF20, 0xF30) */
    uint32_t res9[5236];            /* [0x0F30, 0x6100) */

    uint64_t iroutern[960];         /* [0x6100, 0x7F00) irouter<n> to configure IRQs
                                     * with INTID from 32 to 1019. iroutern[0] is the
                                     * interrupt routing for SPI 32 */
};

/* Wait for completion of a distributor change */
/** DONT_TRANSLATE */
static void gicv3_do_wait_for_rwp(volatile uint32_t *ctlr_addr)
{
    /* Check the value before reading the generic timer */
    uint32_t val = *ctlr_addr;
    while (val & GICD_CTLR_RWP) {
        return;
    }
}

static int gic_v3_init(struct elfloader_device *dev,
                          UNUSED void *match_data)
{

	// Track whether initialization has happened incase this is called again.
	static int initialized = 0;
	if (initialized) return 0;
    volatile struct gic_dist_map *dist = dev->region_bases[0];

    uint32_t ctlr = dist->ctlr;
    const uint32_t ctlr_mask = GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS;
    if ((ctlr & ctlr_mask) != ctlr_mask) {

        if (ctlr !=  (ctlr & ~(GICD_CTLR_ENABLE_G1NS))) {
            printf("GICv3: GICD_CTLR 0x%x -> 0x%lx (Disabling Grp1NS)\n", ctlr, ctlr & ~(GICD_CTLR_ENABLE_G1NS));
            ctlr = ctlr & ~(GICD_CTLR_ENABLE_G1NS);
            dist->ctlr = ctlr;
            gicv3_do_wait_for_rwp(&dist->ctlr);
        }
        printf("GICv3: GICD_CTLR 0x%x -> 0x%x (Enabling Grp1NS and ARE_NS)\n", ctlr, ctlr | ctlr_mask);
        dist->ctlr = ctlr | ctlr_mask;
        gicv3_do_wait_for_rwp(&dist->ctlr);
    }

    uint32_t type = dist->typer;
    unsigned int nr_lines = GIC_REG_WIDTH * ((type & GICD_TYPE_LINESNR) + 1);

    /* Disable and clear all global interrupts */
	word_t i;
    for (i = SPI_START; i < nr_lines; i += 32) {
        dist->icenablern[(i / 32)] = IRQ_SET_ALL;
        dist->icpendrn[(i / 32)] = IRQ_SET_ALL;
     }

    /* Set level triggered level-triggered */
    for (i = SPI_START; i < nr_lines; i += 16) {
        dist->icfgrn[(i / 16)] = 0;
    }

    /* Default priority for global interrupts */
    uint32_t priority = (GIC_PRI_IRQ << 24 | GIC_PRI_IRQ << 16 | GIC_PRI_IRQ << 8 |
                GIC_PRI_IRQ);
    for (i = SPI_START; i < nr_lines; i += 4) {
        dist->ipriorityrn[(i / 4)] = priority;
    }


    initialized = 1;
    return 0;
}

static const struct dtb_match_table gic_v3_matches[] = {
    { .compatible = "arm,gic-v3" },
    { .compatible = NULL /* sentinel */ },
};

static const struct elfloader_driver gic_v3 = {
    .match_table = gic_v3_matches,
    .type = DRIVER_IRQ,
    .init = &gic_v3_init,
    .ops = NULL,
};

ELFLOADER_DRIVER(gic_v3);
