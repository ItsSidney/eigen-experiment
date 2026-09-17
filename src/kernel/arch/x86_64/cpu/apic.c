/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * apic.c - Local APIC and I/O APIC setup, replaces legacy 8259 routing.
 */

#include <stdint.h>
#include "arch/x86_64/cpu/apic.h"
#include "acpi/acpi.h"
#include "mm/vmm.h"
#include "include/serial.h"

#define LAPIC_ID   0x20
#define LAPIC_TPR  0x80
#define LAPIC_EOI  0xB0
#define LAPIC_SVR  0xF0

#define IO_SEL 0x00
#define IO_WIN 0x10
#define IO_ID  0x00
#define IO_VER 0x01
#define IO_REDIR 0x10

static int have_apic;
static uint64_t lapic_virt;
static uint64_t ioapic_virt;
static uint32_t ioapic_max;
static uint8_t bsp_id;

static uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t *)(lapic_virt + reg);
}

static void lapic_write(uint32_t reg, uint32_t v) {
    *(volatile uint32_t *)(lapic_virt + reg) = v;
}

static uint32_t ioapic_read(uint8_t idx) {
    *(volatile uint32_t *)(ioapic_virt + IO_SEL) = idx;
    return *(volatile uint32_t *)(ioapic_virt + IO_WIN);
}

static void ioapic_write(uint8_t idx, uint32_t v) {
    *(volatile uint32_t *)(ioapic_virt + IO_SEL) = idx;
    *(volatile uint32_t *)(ioapic_virt + IO_WIN) = v;
}

static void cpuid_raw(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf));
}

static uint64_t rdmsr_raw(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr_raw(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}

static void redir_write(uint32_t gsi, uint32_t lo, uint32_t hi) {
    ioapic_write(IO_REDIR + gsi * 2, lo | (1 << 16));
    ioapic_write(IO_REDIR + gsi * 2 + 1, hi);
}

static int redir_program(uint8_t irq, int unmask) {
    uint32_t gsi = irq;
    uint16_t flags = 0;
    acpi_iso_get(irq, &gsi, &flags);
    if (gsi > ioapic_max + 1)
        return -1;
    uint8_t vec = 0x20 + irq;
    if (vec < 0x20 || vec > 0x2F)
        return -1;
    uint32_t pol = (flags & 3);
    uint32_t trg = (flags >> 2) & 3;
    uint32_t lo = vec;
    if (pol == 3)
        lo |= (1 << 13);
    if (trg == 3)
        lo |= (1 << 15);
    if (!unmask)
        lo |= (1 << 16);
    uint32_t hi = ((uint32_t)bsp_id << 24);
    ioapic_write(IO_REDIR + gsi * 2, lo | (1 << 16));
    ioapic_write(IO_REDIR + gsi * 2 + 1, hi);
    ioapic_write(IO_REDIR + gsi * 2, lo);
    return 0;
}

void apic_init(void) {
    uint32_t a, b, c, d;
    cpuid_raw(1, &a, &b, &c, &d);
    if (!(d & (1 << 9))) {
        serial_puts("[APIC] no CPU APIC bit, staying on PIC\n");
        return;
    }
    if (!acpi_available() || !acpi_ioapic_addr()) {
        serial_puts("[APIC] no MADT IOAPIC, staying on PIC\n");
        return;
    }
    uint64_t lapic_phys = acpi_lapic_base();
    uint64_t ioapic_phys = acpi_ioapic_addr();
    if (!lapic_phys) lapic_phys = 0xFEE00000;
    if (!ioapic_phys) ioapic_phys = 0xFEC00000;
    uint64_t hhdm = vmm_get_hhdm_offset();
    pml4_t *kpml4 = vmm_get_kernel_address_space();
    vmm_map_page(kpml4, hhdm + lapic_phys, lapic_phys,
                 PAGE_PRESENT | PAGE_WRITABLE | PAGE_PCD | PAGE_PWT);
    vmm_map_page(kpml4, hhdm + ioapic_phys, ioapic_phys,
                 PAGE_PRESENT | PAGE_WRITABLE | PAGE_PCD | PAGE_PWT);
    lapic_virt = hhdm + lapic_phys;
    ioapic_virt = hhdm + ioapic_phys;

    uint64_t msr = rdmsr_raw(0x1B);
    msr |= (1ULL << 11);
    wrmsr_raw(0x1B, msr);

    lapic_write(LAPIC_TPR, 0);
    bsp_id = (uint8_t)(lapic_read(LAPIC_ID) >> 24);
    lapic_write(LAPIC_SVR, 0xFF | (1 << 8));

    outb(0x22, 0x70);
    outb(0x23, 0x01);

    uint32_t ver = ioapic_read(IO_VER);
    ioapic_max = (ver >> 16) & 0xFF;

    for (uint32_t i = 0; i <= ioapic_max && i < 24; i++)
        redir_write(i, 0, 0);

    for (uint8_t irq = 0; irq < 16; irq++) {
        uint32_t gsi = irq;
        uint16_t fl = 0;
        acpi_iso_get(irq, &gsi, &fl);
        if (gsi > ioapic_max + 1)
            continue;
        uint32_t pol = (fl & 3);
        uint32_t trg = (fl >> 2) & 3;
        uint32_t lo = (0x20 + irq);
        if (pol == 3)
            lo |= (1 << 13);
        if (trg == 3)
            lo |= (1 << 15);
        lo |= (1 << 16);
        uint32_t hi = ((uint32_t)bsp_id << 24);
        ioapic_write(IO_REDIR + gsi * 2, lo | (1 << 16));
        ioapic_write(IO_REDIR + gsi * 2 + 1, hi);
        ioapic_write(IO_REDIR + gsi * 2, lo);
    }

    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    have_apic = 1;
    serial_puts("[APIC] LAPIC=");
    serial_puthex(lapic_phys);
    serial_puts(" IOAPIC=");
    serial_puthex(ioapic_phys);
    serial_puts(" max=");
    serial_puthex(ioapic_max);
    serial_puts(" bsp=");
    serial_puthex(bsp_id);
    serial_puts("\n");
}

int apic_available(void) {
    return have_apic;
}

void apic_eoi(void) {
    if (have_apic)
        lapic_write(LAPIC_EOI, 0);
}

int apic_irq_unmask(uint8_t irq) {
    if (!have_apic || irq >= 16)
        return -1;
    return redir_program(irq, 1);
}

int apic_irq_mask(uint8_t irq) {
    if (!have_apic || irq >= 16)
        return -1;
    return redir_program(irq, 0);
}
