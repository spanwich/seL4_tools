/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <elfloader.h>
#include <types.h>
#include <mode/structures.h>

/* Paging structures for kernel mapping */
uint64_t _boot_pgd_up[CONFIG_MAX_NUM_NODES][BIT(PGD_BITS)] ALIGN(BIT(PGD_SIZE_BITS));
uint64_t _boot_pud_up[CONFIG_MAX_NUM_NODES][BIT(PUD_BITS)] ALIGN(BIT(PUD_SIZE_BITS));
uint64_t _boot_pmd_up[CONFIG_MAX_NUM_NODES][BIT(PMD_BITS)] ALIGN(BIT(PMD_SIZE_BITS));

/* Paging structures for identity mapping */
uint64_t _boot_pgd_down[CONFIG_MAX_NUM_NODES][BIT(PGD_BITS)] ALIGN(BIT(PGD_SIZE_BITS));
uint64_t _boot_pud_down[CONFIG_MAX_NUM_NODES][BIT(PUD_BITS)] ALIGN(BIT(PUD_SIZE_BITS));
