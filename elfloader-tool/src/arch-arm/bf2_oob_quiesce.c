/*
 * BlueField-2 OOB GigE (mlxbf_gige) DMA quiesce.
 *
 * UEFI leaves the OOB MAC with RX/TX ring base addresses programmed into
 * UEFI-owned memory. Put the block into the same state Linux's
 * mlxbf_gige_stop() leaves it in, before any seL4 kernel runs, so a stale
 * enable can never DMA into memory later used by a kernel.
 *
 * Sequence (drivers/net/ethernet/mellanox/mlxbf_gige/ in linux-6.6):
 *   mlxbf_gige_stop()            main.c:187-205
 *     INT_EN = 0                 main.c:191
 *     rx_deinit:  RX_DMA.EN = 0 (RMW) rx.c:187-189; RX_WQ_BASE=0, RX_CQ_BASE=0 rx.c:212-213
 *     tx_deinit:  TX_WQ_BASE=0, TX_CI_UPDATE_ADDRESS=0   tx.c:85-86
 *     clean_port                 main.c:103-127 (CONTROL.CLEAN_PORT_EN, mb,
 *                                poll STATUS.READY 100us x 100ms, clear bit)
 *   mlxbf_gige_shutdown()        main.c:486-491 (INT_EN=0; clean_port)
 *   CONTROL.PORT_EN is only ever *set* by Linux (probe, main.c:383-386).
 *
 * MAC filter decision: Linux main.c:140-148 documents that clean_port resets
 * only the RX polarity (plus stats counters, main.c:~92-101 mlxbf_gige_cache_stats).
 * Linux open() after stop() never reprograms the MAC filter (ndo_set_mac_address
 * is plain eth_mac_addr, and mlxbf_gige_initial_mac() runs only from probe), so
 * the filter must survive clean_port. We nevertheless snapshot filters 0..3,
 * the DMAC range regs and the CONTROL filter-enable bits (MAC_ID_RANGE_EN,
 * EN_SPECIFIC_MAC<<idx) and restore any that clean_port changed. Cheap and
 * guarantees the UEFI MAC survives for later drivers.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <autoconf.h>
#include <elfloader/gen_config.h>

#ifdef CONFIG_PLAT_MLXBF2

#include <types.h>
#include <printf.h>
#include <arch-arm/bf2_oob_quiesce.h>

#define OOB_BASE                    0x03000000UL

#define GIGE_STATUS                 0x0010
#define GIGE_STATUS_READY           (1UL << 0)
#define GIGE_INT_EN                 0x0030
#define GIGE_CONTROL                0x0040
#define GIGE_CONTROL_PORT_EN        (1UL << 0)
#define GIGE_CONTROL_MAC_ID_RANGE_EN (1UL << 1)
#define GIGE_CONTROL_EN_SPECIFIC_MAC (1UL << 4)   /* << filter index */
#define GIGE_CONTROL_CLEAN_PORT_EN  (1UL << 31)
#define GIGE_RX_WQ_BASE             0x0200
#define GIGE_RX_CQ_BASE             0x0210
#define GIGE_TX_WQ_BASE             0x0218
#define GIGE_TX_CI_UPDATE_ADDRESS   0x0228
#define GIGE_RX_MAC_FILTER          0x0240
#define GIGE_RX_MAC_FILTER_STRIDE   0x0008
#define GIGE_RX_MAC_FILTER_COUNT    4
#define GIGE_DMAC_RANGE_START       0x0520
#define GIGE_DMAC_RANGE_END         0x0528
#define GIGE_RX_DMA                 0x0580
#define GIGE_RX_DMA_EN              (1UL << 0)

#define CONTROL_FILTER_BITS (GIGE_CONTROL_MAC_ID_RANGE_EN | (0xFUL << 4))

#define CLEAN_PORT_TIMEOUT_MS       100UL

static inline uint64_t oob_rd(unsigned long off)
{
    uint64_t v = *(volatile uint64_t *)(OOB_BASE + off);
    __asm__ volatile("dsb sy" ::: "memory");
    return v;
}

static inline void oob_wr(unsigned long off, uint64_t v)
{
    *(volatile uint64_t *)(OOB_BASE + off) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline uint64_t oob_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(v) :: "memory");
    return v;
}

static inline uint64_t oob_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static void oob_print_state(const char *tag)
{
    printf("BF2-OOB-QUIESCE: %s ctl=%lx rx_dma=%lx rx_wq=%lx rx_cq=%lx tx_wq=%lx tx_ci=%lx filt1=%lx",
           tag,
           (unsigned long)oob_rd(GIGE_CONTROL),
           (unsigned long)oob_rd(GIGE_RX_DMA),
           (unsigned long)oob_rd(GIGE_RX_WQ_BASE),
           (unsigned long)oob_rd(GIGE_RX_CQ_BASE),
           (unsigned long)oob_rd(GIGE_TX_WQ_BASE),
           (unsigned long)oob_rd(GIGE_TX_CI_UPDATE_ADDRESS),
           (unsigned long)oob_rd(GIGE_RX_MAC_FILTER + GIGE_RX_MAC_FILTER_STRIDE));
}

/* Linux mlxbf_gige_clean_port(), main.c:103-127. Returns 0 on success. */
static int oob_clean_port(void)
{
    uint64_t control = oob_rd(GIGE_CONTROL);
    oob_wr(GIGE_CONTROL, control | GIGE_CONTROL_CLEAN_PORT_EN);
    __asm__ volatile("dsb sy" ::: "memory");

    uint64_t ticks = oob_cntfrq() * CLEAN_PORT_TIMEOUT_MS / 1000UL;
    uint64_t start = oob_cntpct();
    int err = -1;
    for (;;) {
        if (oob_rd(GIGE_STATUS) & GIGE_STATUS_READY) {
            err = 0;
            break;
        }
        if (oob_cntpct() - start > ticks) {
            /* one last look, as readq_poll_timeout_atomic does */
            if (oob_rd(GIGE_STATUS) & GIGE_STATUS_READY) {
                err = 0;
            }
            break;
        }
    }

    control = oob_rd(GIGE_CONTROL);
    oob_wr(GIGE_CONTROL, control & ~GIGE_CONTROL_CLEAN_PORT_EN);
    return err;
}

void bf2_oob_quiesce(void)
{
    uint64_t filt[GIGE_RX_MAC_FILTER_COUNT];
    uint64_t range_start, range_end, ctl_filter_bits;
    int i;

    oob_print_state("before");
    printf("\n");

    /* Snapshot MAC filter state (restored only if clean_port clobbers it). */
    for (i = 0; i < GIGE_RX_MAC_FILTER_COUNT; i++) {
        filt[i] = oob_rd(GIGE_RX_MAC_FILTER + i * GIGE_RX_MAC_FILTER_STRIDE);
    }
    range_start = oob_rd(GIGE_DMAC_RANGE_START);
    range_end = oob_rd(GIGE_DMAC_RANGE_END);
    ctl_filter_bits = oob_rd(GIGE_CONTROL) & CONTROL_FILTER_BITS;

    /* main.c:191 */
    oob_wr(GIGE_INT_EN, 0);

    /* rx.c:187-189: RMW clear RX_DMA.EN */
    oob_wr(GIGE_RX_DMA, oob_rd(GIGE_RX_DMA) & ~GIGE_RX_DMA_EN);

    /* PORT_EN = 0 (Linux never clears it; required here so nothing is live) */
    oob_wr(GIGE_CONTROL, oob_rd(GIGE_CONTROL) & ~GIGE_CONTROL_PORT_EN);

    /* rx.c:212-213, tx.c:85-86 */
    oob_wr(GIGE_RX_WQ_BASE, 0);
    oob_wr(GIGE_RX_CQ_BASE, 0);
    oob_wr(GIGE_TX_WQ_BASE, 0);
    oob_wr(GIGE_TX_CI_UPDATE_ADDRESS, 0);

    /* main.c:202 */
    int err = oob_clean_port();
    if (err) {
        printf("BF2-OOB-QUIESCE: clean_port TIMEOUT\n");
    }

    /* Restore MAC filter state if clean_port changed it. */
    int restored = 0;
    for (i = 0; i < GIGE_RX_MAC_FILTER_COUNT; i++) {
        unsigned long off = GIGE_RX_MAC_FILTER + i * GIGE_RX_MAC_FILTER_STRIDE;
        if (oob_rd(off) != filt[i]) {
            oob_wr(off, filt[i]);
            restored = 1;
        }
    }
    if (oob_rd(GIGE_DMAC_RANGE_START) != range_start) {
        oob_wr(GIGE_DMAC_RANGE_START, range_start);
        restored = 1;
    }
    if (oob_rd(GIGE_DMAC_RANGE_END) != range_end) {
        oob_wr(GIGE_DMAC_RANGE_END, range_end);
        restored = 1;
    }
    uint64_t ctl = oob_rd(GIGE_CONTROL);
    if ((ctl & CONTROL_FILTER_BITS) != ctl_filter_bits) {
        oob_wr(GIGE_CONTROL, (ctl & ~CONTROL_FILTER_BITS) | ctl_filter_bits);
        restored = 1;
    }

    /* Final sanity: make sure DMA enable / bases are still dead. */
    int residue = (oob_rd(GIGE_RX_DMA) & GIGE_RX_DMA_EN) ||
                  oob_rd(GIGE_RX_WQ_BASE) || oob_rd(GIGE_RX_CQ_BASE) ||
                  oob_rd(GIGE_TX_WQ_BASE) || oob_rd(GIGE_TX_CI_UPDATE_ADDRESS);
    oob_print_state("after");
    printf(" filt_restored=%s residue=%s\n", restored ? "yes" : "no", residue ? "yes" : "no");
}

#endif /* CONFIG_PLAT_MLXBF2 */
