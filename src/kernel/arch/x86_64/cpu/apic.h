/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * apic.h - Local APIC and I/O APIC driver interface.
 */

#ifndef KERNEL_APIC_H
#define KERNEL_APIC_H

#include <stdint.h>

void apic_init(void);
int  apic_available(void);
void apic_eoi(void);
int  apic_irq_unmask(uint8_t irq);
int  apic_irq_mask(uint8_t irq);

#endif /* KERNEL_APIC_H */
