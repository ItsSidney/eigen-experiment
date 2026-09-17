/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * pit.c - Programmable Interval Timer (PIT 8254) driver.
 */

#include <stdint.h>
#include "pit.h"
#include "arch/x86_64/cpu/idt.h"
#include "arch/x86_64/cpu/apic.h"
#include "proc/process.h"
#include "include/serial.h"

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND       0x43
#define PIT_BASE_FREQUENCY 1193182

static volatile uint64_t ticks = 0;
static uint32_t current_freq = 100;

static void pit_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    ticks++;
    proc_timer_tick();
}

void pit_init(uint32_t frequency_hz) {
    current_freq = frequency_hz;
    uint32_t divisor = PIT_BASE_FREQUENCY / frequency_hz;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;

    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    register_interrupt_handler(32, pit_irq_handler);

    if (apic_available()) {
        apic_irq_unmask(0);
    } else {
        uint8_t mask = inb(0x21);
        outb(0x21, mask & ~0x01);
    }

    serial_puts("[PIT] Timer initialized at ");
    serial_puthex(frequency_hz);
    serial_puts(" Hz\n");
}

uint64_t timer_get_ticks(void) {
    return ticks;
}

uint32_t timer_get_freq(void) {
    return current_freq;
}

void timer_sleep_ms(uint32_t ms) {
    uint64_t target_ticks = ticks + (ms * current_freq / 1000);
    while (ticks < target_ticks) {
        __asm__ volatile("hlt");
    }
}
