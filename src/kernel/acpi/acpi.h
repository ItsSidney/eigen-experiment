/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * acpi.h - ACPI table discovery (RSDP/XSDT/MADT/MCFG/HPET/FADT).
 */

#ifndef KERNEL_ACPI_H
#define KERNEL_ACPI_H

#include <stdint.h>
#include <stddef.h>

void     acpi_init(void *rsdp);
int      acpi_available(void);
uint32_t acpi_cpu_count(void);
uint8_t  acpi_cpu_apic_id(uint32_t idx);
uint64_t acpi_lapic_base(void);
uint64_t acpi_ioapic_addr(void);
int      acpi_iso_get(uint8_t irq, uint32_t *gsi, uint16_t *flags);
uint64_t acpi_hpet_addr(void);
uint64_t acpi_mcfg_base(void);
int      acpi_reset(void);
int      acpi_poweroff(void);

/* Raw table lookup by 4-char signature, e.g. "FACP". Returns mapping or NULL. */
const void *acpi_get_table(const char sig[4], uint32_t *len_out);

#endif /* KERNEL_ACPI_H */
