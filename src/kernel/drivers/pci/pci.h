/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * pci.h - Peripheral Component Interconnect (PCI) bus definitions and driver API.
 */

#ifndef KERNEL_PCI_H
#define KERNEL_PCI_H

#include <stdint.h>
#include <stddef.h>

#define PCI_CONFIG_ADDRESS  0x0CF8
#define PCI_CONFIG_DATA     0x0CFC

/* Common PCI Offsets */
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_CACHE_LINE_SIZE 0x0C
#define PCI_LATENCY_TIMER   0x0D
#define PCI_HEADER_TYPE     0x0E
#define PCI_BIST            0x0F
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D

/* Command Register Bits */
#define PCI_CMD_IO_SPACE        (1 << 0)
#define PCI_CMD_MEMORY_SPACE    (1 << 1)
#define PCI_CMD_BUS_MASTER      (1 << 2)
#define PCI_CMD_INT_DISABLE     (1 << 10)

/* Maximum detected devices */
#define PCI_MAX_DEVICES     64

/* Base Address Register */
struct pci_bar {
    uint64_t base;
    uint64_t size;
    uint8_t  is_io;       /* 1 = I/O port, 0 = MMIO */
    uint8_t  is_64bit;
    uint8_t  is_prefetch;
};

/* PCI Device Structure */
struct pci_device {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;

    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_id;
    uint8_t  subclass_id;
    uint8_t  prog_if;
    uint8_t  header_type;

    uint8_t  irq_line;
    uint8_t  irq_pin;

    struct pci_bar bars[6];
};

void     pci_init(void);
uint32_t pci_read_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_read_config16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t  pci_read_config8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_write_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
void     pci_write_config16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val);
void     pci_write_config8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t val);

void     pci_enable_bus_mastering(struct pci_device *dev);
void     pci_enable_mmio(struct pci_device *dev);

struct pci_device *pci_find_device(uint16_t vendor_id, uint16_t device_id);
struct pci_device *pci_find_class(uint8_t class_id, uint8_t subclass_id);
uint32_t           pci_get_device_count(void);
struct pci_device *pci_get_device(uint32_t index);

const char *pci_class_to_string(uint8_t class_id, uint8_t subclass_id);
const char *pci_device_to_string(uint16_t vendor_id, uint16_t device_id);

void pci_print_devices(void);

#endif /* KERNEL_PCI_H */
