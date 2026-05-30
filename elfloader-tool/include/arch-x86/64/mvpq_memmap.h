/*
 * Copyright 2026, PhD Research Project — multikernel-AMP MVP-Q (x86_64)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Canonical multikernel-AMP physical memory map (Task A: 4 GiB QEMU).
 *
 * This is the single source of truth for the per-kernel partitioning. The
 * same constants are mirrored (kept identical, by hand) in two other trees
 * that cannot share this header:
 *
 *   - kernel/src/arch/x86/kernel/boot_sys.c        (kernel-direct handshake)
 *   - projects/multikernel_qemu_x86_64/rootserver/ (user-space ring, Task B)
 *
 * If you change a value here, change it there too. The research note
 * projects/multikernel_qemu_x86_64/docs/task_a_memory_map.md explains the
 * rationale for every number.
 *
 * Hard constraints:
 *   - Each kernel's ELF load base (KLO_BASE(i)) MUST be < 0x40000000
 *     (1 GiB). seL4 x86_64 asserts KERNEL_ELF_BASE lands in PDPT slot 510
 *     (src/arch/x86/64/kernel/vspace.c). KLO_BASE(7) = 0x39000000 < 1 GiB,
 *     so up to 8 kernels are addressable.
 *   - All RAM windows must lie below QEMU's `-m 4G` low-RAM ceiling
 *     (~0xe0000000 on the default pc machine) and below
 *     CONFIG_PADDR_USER_DEVICE_TOP (0x800000000000).
 */

#ifndef _MVPQ_MEMMAP_H_
#define _MVPQ_MEMMAP_H_

/* ---- Per-kernel low window: kernel ELF + rootserver + initial untyped ----
 * Must be < 1 GiB (PDPT slot 510). 128 MiB stride, 128 MiB window.
 * KLO_BASE(i) is also the value passed as KernelX86_64ELFPaddrBase for
 * slot i (see projects/multikernel_qemu_x86_64/build.sh).
 */
#define MVPQ_KLO_BASE0   0x01000000u            /* K0 low base (16 MiB)   */
#define MVPQ_KLO_STRIDE  0x08000000u            /* 128 MiB between kernels */
#define MVPQ_KLO_SIZE    0x08000000u            /* 128 MiB per kernel      */
#define MVPQ_KLO_BASE(i) (MVPQ_KLO_BASE0 + (unsigned)(i) * MVPQ_KLO_STRIDE)

/* ---- Per-kernel high RAM pool: above 1 GiB (Task A "use the space
 * above 1 GiB" — rootserver data / shared pools / future growth). ----
 * 256 MiB stride/size. K0..K7 span [0x40000000, 0xC0000000).
 */
#define MVPQ_KHI_BASE0   0x40000000u            /* 1 GiB                   */
#define MVPQ_KHI_STRIDE  0x10000000u            /* 256 MiB between kernels */
#define MVPQ_KHI_SIZE    0x10000000u            /* 256 MiB per kernel      */
#define MVPQ_KHI_BASE(i) (MVPQ_KHI_BASE0 + (unsigned)(i) * MVPQ_KHI_STRIDE)

/* ---- Shared inter-kernel region ----
 * Excluded from every kernel's usable mmap, so seL4's create_untypeds()
 * exposes it as a *device* untyped (device retype does NOT zero memory —
 * essential for a region two independent kernels must both see). The two
 * guard pages below/above force this region into its own device-untyped
 * gap so its untyped base == MVPQ_SHARED_BASE exactly (one 2 MiB UT).
 *
 * 2.25 GiB: above every kernel window, below the `-m 4G` low-RAM ceiling.
 */
#define MVPQ_SHARED_BASE 0x90000000u            /* 2.25 GiB                */
#define MVPQ_SHARED_SIZE 0x00200000u            /* 2 MiB (2^21)            */
#define MVPQ_SHARED_END  (MVPQ_SHARED_BASE + MVPQ_SHARED_SIZE)

#define MVPQ_GUARD_SIZE  0x00002000u            /* 8 KiB guard pages       */
#define MVPQ_GUARD_LO    (MVPQ_SHARED_BASE - MVPQ_GUARD_SIZE)
#define MVPQ_GUARD_HI    MVPQ_SHARED_END

/* Kernel slot id derivable from its compile-time ELF load base. */
#define MVPQ_KID_FROM_PADDR(p) (((unsigned)(p) - MVPQ_KLO_BASE0) / MVPQ_KLO_STRIDE)

/* Trampoline + AP conventions. AP i is assumed to have APIC ID i, and
 * runs kernel slot i (BSP = APIC 0 = K0). */
#define MVPQ_TRAMPOLINE_PADDR 0x8000u
#define MVPQ_MAX_KERNELS      8

#endif /* _MVPQ_MEMMAP_H_ */
