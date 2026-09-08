/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <binaries/efi/efi.h>
#include <elfloader_common.h>

void *__application_handle = NULL;             // current efi application handler
efi_system_table_t *__efi_system_table = NULL; // current efi system table

extern void _start(void);

static uint16_t efi_conout_buf[160];

void efi_stall(unsigned long usec)
{
    efi_boot_services_t *bts;

    if (!__efi_system_table || !__efi_system_table->boottime) {
        return;
    }

    bts = get_efi_boot_services();
    if (bts && bts->stall) {
        bts->stall(usec);
    }
}

void efi_conout_puts(const char *s)
{
    efi_simple_text_output_protocol_t *con_out;
    unsigned int i = 0;

    if (!__efi_system_table || !__efi_system_table->con_out) {
        return;
    }

    con_out = (efi_simple_text_output_protocol_t *)__efi_system_table->con_out;
    if (!con_out->output_string) {
        return;
    }

    while (s[i] && i < (sizeof(efi_conout_buf) / sizeof(efi_conout_buf[0])) - 3) {
        if (s[i] == '\n') {
            efi_conout_buf[i++] = '\r';
            if (i >= (sizeof(efi_conout_buf) / sizeof(efi_conout_buf[0])) - 2) {
                break;
            }
            efi_conout_buf[i] = '\n';
        } else {
            efi_conout_buf[i] = (uint16_t)s[i];
        }
        i++;
    }
    efi_conout_buf[i++] = '\r';
    efi_conout_buf[i++] = '\n';
    efi_conout_buf[i] = 0;
    con_out->output_string(con_out, efi_conout_buf);
}

void efi_conout_put_hex64(const char *label, uintptr_t value)
{
    static char line[112];
    static const char hex[] = "0123456789abcdef";
    unsigned int i = 0;

    while (label && label[i] && i < sizeof(line) - 22) {
        line[i] = label[i];
        i++;
    }
    if (i < sizeof(line) - 1) {
        line[i++] = '=';
    }
    if (i < sizeof(line) - 1) {
        line[i++] = ' ';
    }
    if (i < sizeof(line) - 2) {
        line[i++] = '0';
        line[i++] = 'x';
    }
    for (int shift = (int)(sizeof(uintptr_t) * 8) - 4; shift >= 0 && i < sizeof(line) - 1; shift -= 4) {
        line[i++] = hex[(value >> shift) & 0xfu];
    }
    line[i] = 0;
    efi_conout_puts(line);
}

unsigned int efi_main(uintptr_t application_handle, uintptr_t efi_system_table)
{
    __application_handle = (void *)application_handle;
    __efi_system_table = (efi_system_table_t *)efi_system_table;
    efi_conout_puts("BF2-ELFLOADER-EFI: efi_main entered");
    efi_stall(500000);
    clear_bss();
    __application_handle = (void *)application_handle;
    __efi_system_table = (efi_system_table_t *)efi_system_table;
    efi_conout_puts("BF2-ELFLOADER-EFI: after clear_bss, before _start");
    efi_stall(2000000);
    efi_conout_puts("BF2-ELFLOADER-EFI: entering _start now");
    efi_stall(500000);
    _start();
    efi_conout_puts("BF2-ELFLOADER-EFI: _start returned");
    efi_stall(500000);
    return 0;
}

void *efi_get_fdt(void)
{
    efi_guid_t fdt_guid = make_efi_guid(0xb1b621d5, 0xf19c, 0x41a5,  0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0);
    efi_config_table_t *tables = (efi_config_table_t *)__efi_system_table->tables;

    for (uint32_t i = 0; i < __efi_system_table->nr_tables; i++) {
        if (!efi_guideq(fdt_guid, tables[i].guid))
            continue;

        return (void *)tables[i].table;
    }

    return NULL;
}

/* Before starting the kernel we should notify the UEFI firmware about it
 * otherwise the internal watchdog may reboot us after 5 min.
 *
 * This means boot time services are not available anymore. We should store
 * system information e.g. current memory map and pass them to kernel.
 */
unsigned long efi_exit_boot_services(void)
{
    unsigned long status;
    efi_memory_desc_t *memory_map = NULL;
    unsigned long map_size = 0;
    unsigned long buffer_size = 0;
    unsigned long desc_size = 0, key = 0;
    uint32_t desc_version = 0;

    efi_boot_services_t *bts = get_efi_boot_services();

    if (!bts || !bts->allocate_pool || !bts->get_memory_map || !bts->exit_boot_services) {
        printf("BF2-EFI-EXIT: invalid boot services table bts=%p\n", bts);
        return EFI_LOAD_ERROR;
    }

    status = bts->get_memory_map(&map_size, NULL, &key, &desc_size, &desc_version);
    printf("BF2-EFI-EXIT: probe status=%lx map_size=%lu desc_size=%lu desc_ver=%u\n",
           status, map_size, desc_size, desc_version);
    if (status != EFI_BUFFER_TOO_SMALL) {
        return EFI_LOAD_ERROR;
    }

    buffer_size = map_size + desc_size * 8;
    status = bts->allocate_pool(EFI_LOADER_DATA, buffer_size, (void **)&memory_map);
    printf("BF2-EFI-EXIT: allocate status=%lx buffer_size=%lu map=%p\n",
           status, buffer_size, memory_map);
    if (status != EFI_SUCCESS) {
        return status;
    }

    map_size = buffer_size;
    status = bts->get_memory_map(&map_size, memory_map, &key, &desc_size, &desc_version);
    printf("BF2-EFI-EXIT: final-map status=%lx map_size=%lu key=%lx desc_size=%lu desc_ver=%u\n",
           status, map_size, key, desc_size, desc_version);
    if (status != EFI_SUCCESS) {
        bts->free_pool(memory_map);
        return status;
    }

    status = bts->exit_boot_services(__application_handle, key);
    printf("BF2-EFI-EXIT: exit status=%lx key=%lx\n", status, key);

    if (status == EFI_INVALID_PARAMETER) {
        /* Linux EFI stub pattern: after ExitBootServices fails due to a stale
         * key, do not allocate/free. Reuse the slack buffer, refresh the map,
         * and retry ExitBootServices once.
         */
        map_size = buffer_size;
        status = bts->get_memory_map(&map_size, memory_map, &key, &desc_size, &desc_version);
        printf("BF2-EFI-EXIT: retry-map status=%lx map_size=%lu key=%lx desc_size=%lu desc_ver=%u\n",
               status, map_size, key, desc_size, desc_version);
        if (status != EFI_SUCCESS) {
            return status;
        }

        status = bts->exit_boot_services(__application_handle, key);
        printf("BF2-EFI-EXIT: retry-exit status=%lx key=%lx\n", status, key);
    }

    return status;
}




