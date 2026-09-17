/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * pit.h - Programmable Interval Timer (PIT 8254) driver.
 */

#ifndef KERNEL_PIT_H
#define KERNEL_PIT_H

#include <stdint.h>

void pit_init(uint32_t frequency_hz);
uint64_t timer_get_ticks(void);
uint32_t timer_get_freq(void);
void timer_sleep_ms(uint32_t ms);

#endif /* KERNEL_PIT_H */
