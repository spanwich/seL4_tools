/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

 #include <assembler.h>
 #include <mode/assembler.h>

.text

.extern invalid_vector_entry
.extern el1_sync

.macro ventry label, vector=0
.align 7
    mov     x0, #\vector
    b       \label
.endm

.align 12
BEGIN_FUNC(arm_vector_table)
    ventry  invalid_vector_entry, 0        // Synchronous EL1t
    ventry  invalid_vector_entry, 1        // IRQ EL1t
    ventry  invalid_vector_entry, 2        // FIQ EL1t
    ventry  invalid_vector_entry, 3        // SError EL1t

    ventry  el1_sync, 4                    // Synchronous EL1h
    ventry  invalid_vector_entry, 5        // IRQ EL1h
    ventry  invalid_vector_entry, 6        // FIQ EL1h
    ventry  invalid_vector_entry, 7        // SError EL1h

    ventry  invalid_vector_entry, 8        // Synchronous 64-bit EL0
    ventry  invalid_vector_entry, 9        // IRQ 64-bit EL0
    ventry  invalid_vector_entry, 10       // FIQ 64-bit EL0
    ventry  invalid_vector_entry, 11       // SError 64-bit EL0

    ventry  invalid_vector_entry, 12       // Synchronous 32-bit EL0
    ventry  invalid_vector_entry, 13       // IRQ 32-bit EL0
    ventry  invalid_vector_entry, 14       // FIQ 32-bit EL0
    ventry  invalid_vector_entry, 15       // SError 32-bit EL0
END_FUNC(arm_vector_table)
