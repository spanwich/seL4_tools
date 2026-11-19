/*
 * BlueField-2 OOB GigE DMA quiesce (elfloader, MLXBF2 only).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <autoconf.h>
#include <elfloader/gen_config.h>

#ifdef CONFIG_PLAT_MLXBF2
/* Put the OOB GigE MAC (0x03000000) into a known-dead DMA state. Must be
 * called once, on the boot CPU, after UEFI ExitBootServices and before any
 * kernel runs. Never aborts; prints BF2-OOB-QUIESCE lines. */
void bf2_oob_quiesce(void);
#endif
