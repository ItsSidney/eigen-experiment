/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * pci.c - PCI bus controller, device enumeration, BAR configuration, and naming.
 */

#include <stdint.h>
#include <stddef.h>
#include "pci.h"
#include "drivers/char/console.h"
#include "arch/x86_64/cpu/idt.h"
#include "include/serial.h"

static struct pci_device pci_devices[PCI_MAX_DEVICES];
static uint32_t pci_device_count = 0;

/* ── PCI Low-level I/O Accessors ────────────────────────────────────────── */

static inline uint32_t pci_make_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (1U << 31) |
           ((uint32_t)bus  << 16) |
           ((uint32_t)(slot & 0x1F) << 11) |
           ((uint32_t)(func & 0x07) << 8)  |
           ((uint32_t)(offset & 0xFC));
}

uint32_t pci_read_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read_config16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, slot, func, offset));
    return (uint16_t)((inl(PCI_CONFIG_DATA) >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t pci_read_config8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, slot, func, offset));
    return (uint8_t)((inl(PCI_CONFIG_DATA) >> ((offset & 3) * 8)) & 0xFF);
}

void pci_write_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

void pci_write_config16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t addr = pci_make_address(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDRESS, addr);
    uint32_t current = inl(PCI_CONFIG_DATA);
    uint32_t shift = (offset & 2) * 8;
    current &= ~(0xFFFFU << shift);
    current |= ((uint32_t)val << shift);
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, current);
}

void pci_write_config8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t val) {
    uint32_t addr = pci_make_address(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDRESS, addr);
    uint32_t current = inl(PCI_CONFIG_DATA);
    uint32_t shift = (offset & 3) * 8;
    current &= ~(0xFFU << shift);
    current |= ((uint32_t)val << shift);
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, current);
}

/* ── Device Management ──────────────────────────────────────────────────── */

void pci_enable_bus_mastering(struct pci_device *dev) {
    if (!dev) return;
    uint16_t cmd = pci_read_config16(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= (PCI_CMD_BUS_MASTER | PCI_CMD_MEMORY_SPACE | PCI_CMD_IO_SPACE);
    pci_write_config16(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

void pci_enable_mmio(struct pci_device *dev) {
    if (!dev) return;
    uint16_t cmd = pci_read_config16(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_MEMORY_SPACE;
    pci_write_config16(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

static void pci_probe_bar(struct pci_device *dev, int bar_idx) {
    uint8_t offset = PCI_BAR0 + (bar_idx * 4);
    uint32_t bar_low = pci_read_config32(dev->bus, dev->slot, dev->func, offset);

    if (bar_low == 0) return;

    struct pci_bar *bar = &dev->bars[bar_idx];

    /* Check if I/O or MMIO */
    if (bar_low & 1) {
        bar->is_io = 1;
        bar->base = bar_low & ~0x3ULL;
    } else {
        bar->is_io = 0;
        bar->is_64bit = ((bar_low >> 1) & 0x03) == 2;
        bar->is_prefetch = (bar_low >> 3) & 1;
        bar->base = bar_low & ~0xFULL;

        if (bar->is_64bit && bar_idx < 5) {
            uint32_t bar_high = pci_read_config32(dev->bus, dev->slot, dev->func, offset + 4);
            bar->base |= ((uint64_t)bar_high << 32);
        }
    }

    /* Size probing: write all 1s, read back mask */
    pci_write_config32(dev->bus, dev->slot, dev->func, offset, 0xFFFFFFFF);
    uint32_t mask = pci_read_config32(dev->bus, dev->slot, dev->func, offset);
    pci_write_config32(dev->bus, dev->slot, dev->func, offset, bar_low);

    if (bar->is_io) {
        mask &= ~0x3U;
        bar->size = (~mask) + 1;
    } else {
        mask &= ~0xFU;
        bar->size = (~mask) + 1;
    }
}

static void pci_probe_device(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor_id = pci_read_config16(bus, slot, func, PCI_VENDOR_ID);
    if (vendor_id == 0xFFFF || vendor_id == 0x0000) return;

    if (pci_device_count >= PCI_MAX_DEVICES) return;

    struct pci_device *dev = &pci_devices[pci_device_count++];
    dev->bus  = bus;
    dev->slot = slot;
    dev->func = func;

    dev->vendor_id   = vendor_id;
    dev->device_id   = pci_read_config16(bus, slot, func, PCI_DEVICE_ID);
    dev->prog_if     = pci_read_config8(bus, slot, func, PCI_PROG_IF);
    dev->subclass_id = pci_read_config8(bus, slot, func, PCI_SUBCLASS);
    dev->class_id    = pci_read_config8(bus, slot, func, PCI_CLASS);
    dev->header_type = pci_read_config8(bus, slot, func, PCI_HEADER_TYPE);
    dev->irq_line    = pci_read_config8(bus, slot, func, PCI_INTERRUPT_LINE);
    dev->irq_pin     = pci_read_config8(bus, slot, func, PCI_INTERRUPT_PIN);

    /* Probe BARs (up to 6 for standard header type 0) */
    if ((dev->header_type & 0x7F) == 0) {
        for (int b = 0; b < 6; b++) {
            pci_probe_bar(dev, b);
            if (dev->bars[b].is_64bit) b++; /* skip high dword */
        }
    }
}

void pci_init(void) {
    pci_device_count = 0;

    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            uint16_t vendor = pci_read_config16((uint8_t)bus, (uint8_t)slot, 0, PCI_VENDOR_ID);
            if (vendor == 0xFFFF || vendor == 0x0000) continue;

            pci_probe_device((uint8_t)bus, (uint8_t)slot, 0);

            uint8_t header_type = pci_read_config8((uint8_t)bus, (uint8_t)slot, 0, PCI_HEADER_TYPE);
            if (header_type & 0x80) {
                /* Multi-function device: probe functions 1..7 */
                for (uint8_t func = 1; func < 8; func++) {
                    uint16_t func_vendor = pci_read_config16((uint8_t)bus, (uint8_t)slot, func, PCI_VENDOR_ID);
                    if (func_vendor != 0xFFFF && func_vendor != 0x0000) {
                        pci_probe_device((uint8_t)bus, (uint8_t)slot, func);
                    }
                }
            }
        }
    }

    serial_puts("[PCI] Bus scan completed. Detected ");
    serial_puthex(pci_device_count);
    serial_puts(" devices.\n");
}

uint32_t pci_get_device_count(void) {
    return pci_device_count;
}

struct pci_device *pci_get_device(uint32_t index) {
    if (index >= pci_device_count) return NULL;
    return &pci_devices[index];
}

struct pci_device *pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (uint32_t i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id && pci_devices[i].device_id == device_id) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

struct pci_device *pci_find_class(uint8_t class_id, uint8_t subclass_id) {
    for (uint32_t i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].class_id == class_id && pci_devices[i].subclass_id == subclass_id) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

/* ── PCI Device & Class String Descriptions ───────────────────────────────── */

const char *pci_class_to_string(uint8_t class_id, uint8_t subclass_id) {
    switch (class_id) {
        case 0x00: return "Legacy Device";
        case 0x01:
            switch (subclass_id) {
                case 0x01: return "IDE Interface";
                case 0x06: return "SATA Controller (AHCI)";
                case 0x08: return "Non-Volatile Memory (NVMe)";
                default:   return "Mass Storage Controller";
            }
        case 0x02:
            switch (subclass_id) {
                case 0x00: return "Ethernet Controller";
                default:   return "Network Controller";
            }
        case 0x03:
            switch (subclass_id) {
                case 0x00: return "VGA Compatible Controller";
                default:   return "Display Controller";
            }
        case 0x04: return "Multimedia Controller";
        case 0x05: return "Memory Controller";
        case 0x06:
            switch (subclass_id) {
                case 0x00: return "Host Bridge";
                case 0x01: return "ISA Bridge";
                case 0x04: return "PCI-to-PCI Bridge";
                default:   return "Bridge Device";
            }
        case 0x07: return "Communication Controller";
        case 0x0C:
            switch (subclass_id) {
                case 0x03: return "USB Controller";
                case 0x05: return "SMBus Controller";
                default:   return "Serial Bus Controller";
            }
        default: return "Unknown Device";
    }
}

const char *pci_device_to_string(uint16_t vendor_id, uint16_t device_id) {
    if (vendor_id == 0x8086) { /* Intel */
        switch (device_id) {
            case 0x100E: return "Intel 82540EM Gigabit Ethernet (e1000)";
            case 0x100F: return "Intel 82545EM Gigabit Ethernet";
            case 0x10D3: return "Intel 82574L Gigabit Ethernet";
            case 0x1237: return "Intel 440FX - 82441FX PMC (Host Bridge)";
            case 0x7000: return "Intel 82371SB PIIX3 ISA Bridge";
            case 0x7010: return "Intel 82371SB PIIX3 IDE Controller";
            case 0x7111: return "Intel 82371AB/EB/MB PIIX4 IDE Controller";
            case 0x7113: return "Intel 82371AB/EB/MB PIIX4 ACPI";
            case 0x2415: return "Intel 82801AA AC'97 Audio Controller";
            case 0x2829: return "Intel ICH8M SATA Controller (AHCI)";
            case 0x2918: return "Intel ICH9 LPC Interface";
            case 0x2922: return "Intel ICH9 6-port SATA Controller";
            case 0x29C0: return "Intel Core DRAM Controller";
            default:     return "Intel Device";
        }
    } else if (vendor_id == 0x1234) { /* QEMU / Bochs */
        if (device_id == 0x1111) return "QEMU Standard VGA Graphics Adapter";
    } else if (vendor_id == 0x1AF4) { /* Red Hat / Virtio */
        switch (device_id) {
            case 0x1000: return "VirtIO Network Device";
            case 0x1001: return "VirtIO Block Device";
            default:     return "VirtIO Device";
        }
    } else if (vendor_id == 0x15AD) { /* VMware */
        switch (device_id) {
            case 0x0405: return "VMware SVGA II Adapter";
            case 0x0740: return "VMware VMCI Device";
            default:     return "VMware Device";
        }
    } else if (vendor_id == 0x106B) { /* Apple */
        return "Apple Device";
    } else if (vendor_id == 0x1022) { /* AMD */
        switch (device_id) {
            case 0x2000: return "AMD PCnet-FAST III (Am79C973)";
            default:     return "AMD Device";
        }
    } else if (vendor_id == 0x10DE) { /* NVIDIA */
        return "NVIDIA Device";
    }

    return "Generic PCI Device";
}

/* Helper to format 2-digit hex */
static void print_hex2(uint8_t val) {
    static const char hex[] = "0123456789ABCDEF";
    console_putc(hex[(val >> 4) & 0x0F]);
    console_putc(hex[val & 0x0F]);
}

/* Helper to format 4-digit hex */
static void print_hex4(uint16_t val) {
    static const char hex[] = "0123456789ABCDEF";
    console_putc(hex[(val >> 12) & 0x0F]);
    console_putc(hex[(val >> 8) & 0x0F]);
    console_putc(hex[(val >> 4) & 0x0F]);
    console_putc(hex[val & 0x0F]);
}

void pci_print_devices(void) {
    console_puts("Detected PCI Devices:\n");
    console_puts("BDF     Vendor:Device  Class Description              Device Name\n");
    console_puts("----------------------------------------------------------------------------\n");

    for (uint32_t i = 0; i < pci_device_count; i++) {
        struct pci_device *dev = &pci_devices[i];

        print_hex2(dev->bus);
        console_putc(':');
        print_hex2(dev->slot);
        console_putc('.');
        print_hex2(dev->func);
        console_puts("  ");

        print_hex4(dev->vendor_id);
        console_putc(':');
        print_hex4(dev->device_id);
        console_puts("   ");

        const char *cls = pci_class_to_string(dev->class_id, dev->subclass_id);
        console_puts(cls);

        /* Space padding */
        size_t clen = 0;
        while (cls[clen]) clen++;
        for (size_t s = clen; s < 30; s++) console_putc(' ');

        console_puts(pci_device_to_string(dev->vendor_id, dev->device_id));

        if (dev->bars[0].base != 0) {
            console_puts(" [BAR0: ");
            if (dev->bars[0].is_io) {
                console_puts("I/O 0x");
                print_hex4((uint16_t)dev->bars[0].base);
            } else {
                console_puts("MMIO 0x");
                print_hex4((uint16_t)(dev->bars[0].base >> 16));
                print_hex4((uint16_t)dev->bars[0].base);
            }
            console_puts("]");
        }

        console_putc('\n');
    }
}
