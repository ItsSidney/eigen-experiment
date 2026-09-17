/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * e1000.c - Intel 82540EM (e1000) Gigabit Ethernet driver for Eigen.
 *
 * Supported hardware:
 *   QEMU:       -net nic,model=e1000       (PCI 8086:100E, MMIO BAR0)
 *   VirtualBox: Network -> Intel PRO/1000 MT Desktop (82540EM)
 *
 * Driver overview:
 *   1. PCI discovery via pci_find_device(0x8086, 0x100E)
 *   2. Enable bus mastering and MMIO space
 *   3. Map BAR0 MMIO via HHDM offset (no TLB change needed)
 *   4. Read MAC from EEPROM via EERD register
 *   5. Reset card, setup RX/TX descriptor rings
 *   6. Enable RX/TX, register IRQ handler
 *
 * The RX/TX rings use physically contiguous memory allocated from PMM.
 * All descriptor and buffer addresses written to the card are PHYSICAL.
 * Kernel accesses them via HHDM virtual addresses (phys + HHDM_OFFSET).
 */

#include <stdint.h>
#include <stddef.h>
#include "e1000.h"
#include "drivers/pci/pci.h"
#include "drivers/char/console.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "arch/x86_64/cpu/idt.h"
#include "arch/x86_64/cpu/apic.h"
#include "include/serial.h"

/* ── Global driver state ─────────────────────────────────────────────────── */
struct e1000_dev g_e1000;
int e1000_available = 0;

/* ── HHDM offset (filled in during init) ─────────────────────────────────── */
static uint64_t e1000_hhdm_offset = 0;

/* ── MMIO register accessors ─────────────────────────────────────────────── */
static inline void e1000_write(uint32_t reg, uint32_t val) {
    *((volatile uint32_t *)(g_e1000.mmio_base + reg)) = val;
    /* Memory barrier: ensure write reaches device before next operation */
    __asm__ volatile("mfence" ::: "memory");
}

static inline uint32_t e1000_read(uint32_t reg) {
    uint32_t val = *((volatile uint32_t *)(g_e1000.mmio_base + reg));
    __asm__ volatile("mfence" ::: "memory");
    return val;
}

/* ── EEPROM read (via EERD) ──────────────────────────────────────────────── */
static uint16_t e1000_eeprom_read(uint8_t addr) {
    e1000_write(E1000_EERD,
        ((uint32_t)addr << E1000_EERD_ADDR_SHIFT) | E1000_EERD_START);

    uint32_t val;
    uint32_t timeout = 10000;
    do {
        val = e1000_read(E1000_EERD);
    } while (!(val & E1000_EERD_DONE) && --timeout);

    return (uint16_t)(val >> E1000_EERD_DATA_SHIFT);
}

/* ── Detect whether EEPROM exists (some cards use registers instead) ──────── */
static int e1000_has_eeprom(void) {
    e1000_write(E1000_EERD, E1000_EERD_START);
    uint32_t timeout = 10000;
    while (--timeout) {
        uint32_t v = e1000_read(E1000_EERD);
        if (v & E1000_EERD_DONE) return 1;
    }
    return 0;
}

/* ── Read MAC address ────────────────────────────────────────────────────── */
static void e1000_read_mac(void) {
    if (e1000_has_eeprom()) {
        /* Read from EEPROM: 3 words of 2 bytes each */
        uint16_t w0 = e1000_eeprom_read(0);
        uint16_t w1 = e1000_eeprom_read(1);
        uint16_t w2 = e1000_eeprom_read(2);
        g_e1000.mac[0] = w0 & 0xFF;
        g_e1000.mac[1] = (w0 >> 8) & 0xFF;
        g_e1000.mac[2] = w1 & 0xFF;
        g_e1000.mac[3] = (w1 >> 8) & 0xFF;
        g_e1000.mac[4] = w2 & 0xFF;
        g_e1000.mac[5] = (w2 >> 8) & 0xFF;
    } else {
        /* Read directly from RAL/RAH registers (QEMU typically puts MAC here) */
        uint32_t ral = e1000_read(E1000_RAL0);
        uint32_t rah = e1000_read(E1000_RAH0);
        g_e1000.mac[0] = ral & 0xFF;
        g_e1000.mac[1] = (ral >> 8)  & 0xFF;
        g_e1000.mac[2] = (ral >> 16) & 0xFF;
        g_e1000.mac[3] = (ral >> 24) & 0xFF;
        g_e1000.mac[4] = rah & 0xFF;
        g_e1000.mac[5] = (rah >> 8)  & 0xFF;
    }
}

/* ── Simple physical page allocator wrapper ──────────────────────────────── */
/* Allocate N contiguous pages, zero them, return physical address.          */
static uint64_t e1000_alloc_phys(size_t n_pages) {
    /* PMM allocates one page at a time; for descriptor rings we need 1 page.
     * For simplicity, only handle single-page allocations here.             */
    if (n_pages != 1) return 0;
    uint64_t phys = (uint64_t)pmm_alloc_page();
    if (!phys) return 0;
    /* Zero the page via HHDM */
    uint8_t *virt = (uint8_t *)(phys + e1000_hhdm_offset);
    for (size_t i = 0; i < 4096; i++) virt[i] = 0;
    return phys;
}

/* ── IRQ handler ─────────────────────────────────────────────────────────── */
static void e1000_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint32_t icr = e1000_read(E1000_ICR);  /* Reading ICR clears it */

    if (icr & E1000_ICR_LSC) {
        /* Link status changed */
        uint32_t status = e1000_read(E1000_STATUS);
        g_e1000.link_up = (status & (1 << 1)) ? 1 : 0;
        serial_puts("[e1000] Link status change: ");
        serial_puts(g_e1000.link_up ? "UP\n" : "DOWN\n");
    }

    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXDMT0)) {
        /* RX interrupt: data arrived, will be picked up by e1000_recv() */
    }
}

/* ── Initialize RX ring ──────────────────────────────────────────────────── */
static int e1000_setup_rx(void) {
    /* Allocate one page for the descriptor ring */
    uint64_t ring_phys = e1000_alloc_phys(1);
    if (!ring_phys) return -1;

    g_e1000.rx_phys  = ring_phys;
    g_e1000.rx_descs = (struct e1000_rx_desc *)(ring_phys + e1000_hhdm_offset);
    g_e1000.rx_tail  = 0;

    /* Allocate one buffer page per descriptor */
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        uint64_t buf_phys = e1000_alloc_phys(1);
        if (!buf_phys) return -1;
        g_e1000.rx_bufs_phys[i] = buf_phys;
        g_e1000.rx_bufs[i]      = (uint8_t *)(buf_phys + e1000_hhdm_offset);
        g_e1000.rx_descs[i].addr   = buf_phys;
        g_e1000.rx_descs[i].status = 0;
    }

    /* Program hardware */
    e1000_write(E1000_RDBAL, (uint32_t)(ring_phys & 0xFFFFFFFF));
    e1000_write(E1000_RDBAH, (uint32_t)(ring_phys >> 32));
    e1000_write(E1000_RDLEN, E1000_NUM_RX_DESC * sizeof(struct e1000_rx_desc));
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, E1000_NUM_RX_DESC - 1);

    /* RCTL: enable, broadcast, 2048-byte buffers, strip CRC */
    e1000_write(E1000_RCTL,
        E1000_RCTL_EN    |
        E1000_RCTL_BAM   |
        E1000_RCTL_SECRC |
        E1000_RCTL_BSIZE_2048);

    return 0;
}

/* ── Initialize TX ring ──────────────────────────────────────────────────── */
static int e1000_setup_tx(void) {
    uint64_t ring_phys = e1000_alloc_phys(1);
    if (!ring_phys) return -1;

    g_e1000.tx_phys  = ring_phys;
    g_e1000.tx_descs = (struct e1000_tx_desc *)(ring_phys + e1000_hhdm_offset);
    g_e1000.tx_tail  = 0;

    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        uint64_t buf_phys = e1000_alloc_phys(1);
        if (!buf_phys) return -1;
        g_e1000.tx_bufs_phys[i] = buf_phys;
        g_e1000.tx_bufs[i]      = (uint8_t *)(buf_phys + e1000_hhdm_offset);
        /* Mark all descriptors as done initially */
        g_e1000.tx_descs[i].status = E1000_TXD_STAT_DD;
    }

    e1000_write(E1000_TDBAL, (uint32_t)(ring_phys & 0xFFFFFFFF));
    e1000_write(E1000_TDBAH, (uint32_t)(ring_phys >> 32));
    e1000_write(E1000_TDLEN, E1000_NUM_TX_DESC * sizeof(struct e1000_tx_desc));
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);

    /* TCTL: enable, pad short packets, collision threshold=15, distance=64 */
    e1000_write(E1000_TCTL,
        E1000_TCTL_EN  |
        E1000_TCTL_PSP |
        (15   << E1000_TCTL_CT_SHIFT)   |  /* Collision threshold  */
        (64   << E1000_TCTL_COLD_SHIFT));   /* Collision distance   */

    /* TX Inter-Packet Gap: recommended values for 802.3 (ipgt=10, ipgr1=8, ipgr2=6) */
    e1000_write(E1000_TIPG, (6 << 20) | (8 << 10) | 10);

    return 0;
}

/* ── e1000_init ──────────────────────────────────────────────────────────── */
int e1000_init(void) {
    /* Step 1: Find the e1000 on the PCI bus */
    struct pci_device *pdev = pci_find_device(0x8086, 0x100E);
    if (!pdev) {
        /* Also try 82545EM variant */
        pdev = pci_find_device(0x8086, 0x100F);
    }
    if (!pdev) {
        serial_puts("[e1000] No Intel e1000 NIC found on PCI bus.\n");
        return -1;
    }

    serial_puts("[e1000] Found Intel 82540EM at PCI ");
    serial_puthex(pdev->bus);
    serial_puts(":");
    serial_puthex(pdev->slot);
    serial_puts(".");
    serial_puthex(pdev->func);
    serial_puts("\n");

    /* Step 2: Enable Bus Mastering + MMIO space in PCI command register */
    pci_enable_bus_mastering(pdev);

    /* Step 3: Get BAR0 (MMIO base), map via HHDM */
    uint64_t bar0_phys = pdev->bars[0].base;
    if (!bar0_phys || pdev->bars[0].is_io) {
        serial_puts("[e1000] BAR0 is not MMIO or missing!\n");
        return -1;
    }

    /* Get HHDM offset from VMM */
    e1000_hhdm_offset = vmm_get_hhdm_offset();
    g_e1000.mmio_base = bar0_phys + e1000_hhdm_offset;
    g_e1000.irq_line  = pdev->irq_line;

    /* Map BAR0 MMIO pages into kernel address space (PCD | PWT for uncacheable hardware MMIO) */
    uint64_t bar_size = pdev->bars[0].size;
    if (bar_size < 0x20000) bar_size = 0x20000;
    pml4_t *kpml4 = vmm_get_kernel_address_space();
    for (uint64_t off = 0; off < bar_size; off += PAGE_SIZE) {
        vmm_map_page(kpml4, g_e1000.mmio_base + off, bar0_phys + off,
                     PAGE_PRESENT | PAGE_WRITABLE | PAGE_PCD | PAGE_PWT);
    }

    serial_puts("[e1000] MMIO BAR0 phys=0x");
    serial_puthex(bar0_phys);
    serial_puts(" virt=0x");
    serial_puthex(g_e1000.mmio_base);
    serial_puts("\n");

    /* Step 4: Full device reset */
    e1000_write(E1000_CTRL, E1000_CTRL_RST);
    /* Wait for reset to complete (self-clearing) */
    uint32_t timeout = 100000;
    while ((e1000_read(E1000_CTRL) & E1000_CTRL_RST) && --timeout);
    if (!timeout) {
        serial_puts("[e1000] Reset timed out!\n");
        return -1;
    }

    /* Small delay post-reset */
    for (volatile int d = 0; d < 100000; d++);

    /* Step 5: Read MAC address */
    e1000_read_mac();
    serial_puts("[e1000] MAC: ");
    for (int i = 0; i < 6; i++) {
        serial_puthex(g_e1000.mac[i]);
        if (i < 5) serial_puts(":");
    }
    serial_puts("\n");

    /* Step 6: Program MAC into Receive Address registers */
    uint32_t ral = (uint32_t)g_e1000.mac[0]        |
                   ((uint32_t)g_e1000.mac[1] << 8)  |
                   ((uint32_t)g_e1000.mac[2] << 16) |
                   ((uint32_t)g_e1000.mac[3] << 24);
    uint32_t rah = (uint32_t)g_e1000.mac[4]        |
                   ((uint32_t)g_e1000.mac[5] << 8)  |
                   E1000_RAH_AV;
    e1000_write(E1000_RAL0, ral);
    e1000_write(E1000_RAH0, rah);

    /* Step 7: Zero the Multicast Table Array (required before enabling RX) */
    for (int i = 0; i < 128; i++) {
        e1000_write(E1000_MTA + (i * 4), 0);
    }

    /* Step 8: Enable auto-speed detection + set link up */
    uint32_t ctrl = e1000_read(E1000_CTRL);
    ctrl |=  (E1000_CTRL_ASDE | E1000_CTRL_SLU);
    ctrl &= ~(E1000_CTRL_LRST | E1000_CTRL_PHY_RST | E1000_CTRL_ILOS);
    e1000_write(E1000_CTRL, ctrl);

    /* Step 9: Setup RX and TX descriptor rings */
    if (e1000_setup_rx() != 0) {
        serial_puts("[e1000] RX ring setup failed!\n");
        return -1;
    }
    if (e1000_setup_tx() != 0) {
        serial_puts("[e1000] TX ring setup failed!\n");
        return -1;
    }

    /* Step 10: Enable interrupts (RX timer, link change) */
    e1000_write(E1000_IMS, E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_LSC);

    /* Step 11: Register IRQ handler with IDT */
    uint8_t irq_vec = 0x20 + (uint8_t)g_e1000.irq_line;
    register_interrupt_handler(irq_vec, e1000_irq_handler);

    if (apic_available()) {
        apic_irq_unmask((uint8_t)g_e1000.irq_line);
    } else if (g_e1000.irq_line < 8) {
        uint8_t mask = inb(0x21);
        mask &= ~(1 << g_e1000.irq_line);
        outb(0x21, mask);
    } else {
        uint8_t mask = inb(0xA1);
        mask &= ~(1 << (g_e1000.irq_line - 8));
        outb(0xA1, mask);
        /* Also unmask cascade IRQ2 on master PIC */
        uint8_t master_mask = inb(0x21);
        master_mask &= ~(1 << 2);
        outb(0x21, master_mask);
    }

    /* Check initial link status */
    uint32_t status = e1000_read(E1000_STATUS);
    g_e1000.link_up = (status & (1 << 1)) ? 1 : 0;

    e1000_available = 1;
    serial_puts("[e1000] Driver initialized. Link: ");
    serial_puts(g_e1000.link_up ? "UP\n" : "DOWN (waiting for DHCP/cable)\n");

    return 0;
}

/* ── e1000_send ──────────────────────────────────────────────────────────── */
/* Transmit a raw Ethernet frame. len must be <= E1000_TX_BUF_SIZE.         */
int e1000_send(const void *data, uint16_t len) {
    if (!e1000_available) return -1;
    if (len > E1000_TX_BUF_SIZE) return -1;

    uint32_t tail = g_e1000.tx_tail;

    /* Wait for descriptor to be free (status DD=1) */
    uint32_t timeout = 1000000;
    while (!(g_e1000.tx_descs[tail].status & E1000_TXD_STAT_DD) && --timeout);
    if (!timeout) return -1;  /* TX ring full */

    /* Copy frame into buffer */
    const uint8_t *src = (const uint8_t *)data;
    uint8_t *dst = g_e1000.tx_bufs[tail];
    for (uint16_t i = 0; i < len; i++) dst[i] = src[i];

    /* Fill descriptor */
    g_e1000.tx_descs[tail].addr   = g_e1000.tx_bufs_phys[tail];
    g_e1000.tx_descs[tail].length = len;
    g_e1000.tx_descs[tail].cso    = 0;
    g_e1000.tx_descs[tail].css    = 0;
    g_e1000.tx_descs[tail].special = 0;
    g_e1000.tx_descs[tail].status  = 0;
    g_e1000.tx_descs[tail].cmd     = E1000_TXD_CMD_EOP  |
                                      E1000_TXD_CMD_IFCS |
                                      E1000_TXD_CMD_RS;

    /* Advance tail and notify hardware */
    g_e1000.tx_tail = (tail + 1) % E1000_NUM_TX_DESC;
    e1000_write(E1000_TDT, g_e1000.tx_tail);

    return 0;
}

/* ── e1000_recv ──────────────────────────────────────────────────────────── */
/* Non-blocking: copies next received frame into buf, sets *out_len.        */
/* Returns 0 on success, -1 if no frame available.                          */
int e1000_recv(void *buf, uint16_t *out_len) {
    if (!e1000_available) return -1;

    uint32_t tail = (e1000_read(E1000_RDT) + 1) % E1000_NUM_RX_DESC;

    if (!(g_e1000.rx_descs[tail].status & E1000_RXD_STAT_DD)) {
        return -1;  /* No packet ready */
    }

    uint16_t len = g_e1000.rx_descs[tail].length;
    if (len > E1000_RX_BUF_SIZE) len = E1000_RX_BUF_SIZE;

    /* Copy received data */
    uint8_t *src = g_e1000.rx_bufs[tail];
    uint8_t *dst = (uint8_t *)buf;
    for (uint16_t i = 0; i < len; i++) dst[i] = src[i];
    *out_len = len;

    /* Reset descriptor and give it back to hardware */
    g_e1000.rx_descs[tail].status = 0;
    e1000_write(E1000_RDT, tail);

    return 0;
}
/* ── Accessors ───────────────────────────────────────────────────────────── */
void e1000_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = g_e1000.mac[i];
}

int e1000_link_up(void) {
    if (!e1000_available) return 0;
    return g_e1000.link_up;
}

void e1000_print_info(void) {
    if (!e1000_available) {
        console_puts("e1000: No NIC detected.\n");
        return;
    }
    console_puts("e1000 Intel 82540EM Gigabit Ethernet\n");
    console_puts("  MAC: ");
    for (int i = 0; i < 6; i++) {
        static const char h[] = "0123456789ABCDEF";
        console_putc(h[(g_e1000.mac[i] >> 4) & 0xF]);
        console_putc(h[g_e1000.mac[i] & 0xF]);
        if (i < 5) console_putc(':');
    }
    console_putc('\n');
    console_puts("  Link: ");
    console_puts(g_e1000.link_up ? "UP (1000 Mbps)\n" : "DOWN\n");
    console_puts("  RX descriptors: ");
    console_puts("32  TX descriptors: 32\n");
}
