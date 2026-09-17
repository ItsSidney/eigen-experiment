/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * pcnet.c - AMD PCnet-FAST III (Am79C973) driver for Oracle VirtualBox and QEMU.
 */

#include "pcnet.h"
#include "drivers/pci/pci.h"
#include "drivers/char/console.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "include/serial.h"
#include "drivers/timer/pit.h"

#define PCNET_RDP16  0x10
#define PCNET_RAP16  0x12
#define PCNET_RESET  0x14
#define PCNET_BDP16  0x16

#define PCNET_RDP32  0x10
#define PCNET_RAP32  0x14
#define PCNET_RESET32 0x18
#define PCNET_BDP32  0x1C

int pcnet_available = 0;
static uint16_t pcnet_io_base = 0;
static uint8_t  pcnet_mac[6];

static struct pcnet_init_block *init_block;
static uint32_t                 init_block_phys;

static struct pcnet_rx_desc    *rx_ring;
static uint32_t                 rx_ring_phys;
static uint8_t                 *rx_buffers[PCNET_NUM_RX_DESC];
static uint32_t                 rx_buf_phys[PCNET_NUM_RX_DESC];
static uint32_t                 rx_tail = 0;

static struct pcnet_tx_desc    *tx_ring;
static uint32_t                 tx_ring_phys;
static uint8_t                 *tx_buffers[PCNET_NUM_TX_DESC];
static uint32_t                 tx_buf_phys[PCNET_NUM_TX_DESC];
static uint32_t                 tx_tail = 0;

/* Read/Write 16-bit CSR */
static inline uint16_t pcnet_read_csr(uint16_t csr) {
    outw(pcnet_io_base + PCNET_RAP16, csr);
    return inw(pcnet_io_base + PCNET_RDP16);
}

static inline void pcnet_write_csr(uint16_t csr, uint16_t val) {
    outw(pcnet_io_base + PCNET_RAP16, csr);
    outw(pcnet_io_base + PCNET_RDP16, val);
}

/* Read/Write 16-bit BCR */
static inline uint16_t pcnet_read_bcr(uint16_t bcr) {
    outw(pcnet_io_base + PCNET_RAP16, bcr);
    return inw(pcnet_io_base + PCNET_BDP16);
}

static inline void pcnet_write_bcr(uint16_t bcr, uint16_t val) {
    outw(pcnet_io_base + PCNET_RAP16, bcr);
    outw(pcnet_io_base + PCNET_BDP16, val);
}

static uint64_t pcnet_alloc_page(void) {
    uint64_t phys = (uint64_t)pmm_alloc_page();
    if (!phys) return 0;
    uint8_t *v = (uint8_t *)(phys + vmm_get_hhdm_offset());
    for (int i = 0; i < 4096; i++) v[i] = 0;
    return phys;
}

int pcnet_init(void) {
    struct pci_device *pdev = pci_find_device(0x1022, 0x2000);
    if (!pdev) {
        return -1;
    }

    serial_puts("[pcnet] Found AMD PCnet-FAST III (Am79C973) on PCI ");
    serial_puthex(pdev->bus);
    serial_puts(":");
    serial_puthex(pdev->slot);
    serial_puts("\n");

    pci_enable_bus_mastering(pdev);

    /* Get I/O Port base from BAR0 */
    pcnet_io_base = (uint16_t)pdev->bars[0].base;
    if (!pdev->bars[0].is_io) {
        serial_puts("[pcnet] BAR0 is not I/O port!\n");
        return -1;
    }

    /* Read MAC from APROM (first 6 bytes of I/O port space) */
    for (int i = 0; i < 6; i++) {
        pcnet_mac[i] = inb(pcnet_io_base + i);
    }

    serial_puts("[pcnet] MAC: ");
    for (int i = 0; i < 6; i++) {
        serial_puthex(pcnet_mac[i]);
        if (i < 5) serial_puts(":");
    }
    serial_puts("\n");

    /* Reset device: 16-bit reset read, then 32-bit reset read */
    inw(pcnet_io_base + PCNET_RESET);
    inl(pcnet_io_base + PCNET_RESET32);

    /* Small delay after reset */
    for (volatile int d = 0; d < 100000; d++);

    /* Set 32-bit software mode (SWSTYLE = 2) in BCR20 */
    pcnet_write_bcr(20, 0x0102);

    /* Stop card */
    pcnet_write_csr(0, 0x0004);

    /* Allocate physical memory for init block, rings, and buffers */
    uint64_t hhdm = vmm_get_hhdm_offset();
    uint64_t page1_phys = pcnet_alloc_page();
    if (!page1_phys) return -1;

    init_block_phys = (uint32_t)page1_phys;
    init_block      = (struct pcnet_init_block *)(page1_phys + hhdm);

    /* Descriptors inside page 1 */
    rx_ring_phys = (uint32_t)(page1_phys + 128);
    rx_ring      = (struct pcnet_rx_desc *)(page1_phys + 128 + hhdm);

    tx_ring_phys = (uint32_t)(page1_phys + 512);
    tx_ring      = (struct pcnet_tx_desc *)(page1_phys + 512 + hhdm);

    /* Allocate buffers */
    for (int i = 0; i < PCNET_NUM_RX_DESC; i++) {
        uint64_t bp = pcnet_alloc_page();
        if (!bp) return -1;
        rx_buf_phys[i] = (uint32_t)bp;
        rx_buffers[i]  = (uint8_t *)(bp + hhdm);

        rx_ring[i].buf_phys = (uint32_t)bp;
        rx_ring[i].buf_len  = (int16_t)((-PCNET_BUF_SIZE) | 0xF000);
        rx_ring[i].status   = 0x8000; /* Owned by card */
        rx_ring[i].msg_len  = 0;
        rx_ring[i].reserved = 0;
    }

    for (int i = 0; i < PCNET_NUM_TX_DESC; i++) {
        uint64_t bp = pcnet_alloc_page();
        if (!bp) return -1;
        tx_buf_phys[i] = (uint32_t)bp;
        tx_buffers[i]  = (uint8_t *)(bp + hhdm);

        tx_ring[i].buf_phys = (uint32_t)bp;
        tx_ring[i].buf_len  = 0;
        tx_ring[i].status   = 0; /* Owned by host */
        tx_ring[i].misc     = 0;
        tx_ring[i].reserved = 0;
    }

    rx_tail = 0;
    tx_tail = 0;

    /* Fill init block */
    init_block->mode         = 0; /* Normal mode */
    init_block->rlen         = (4 << 4); /* 16 RX descriptors */
    init_block->tlen         = (4 << 4); /* 16 TX descriptors */
    for (int i = 0; i < 6; i++) init_block->mac[i] = pcnet_mac[i];
    init_block->reserved     = 0;
    init_block->ladrf[0]     = 0;
    init_block->ladrf[1]     = 0;
    init_block->rx_ring_phys = rx_ring_phys;
    init_block->tx_ring_phys = tx_ring_phys;

    /* Write Init Block Address to CSR1 and CSR2 */
    pcnet_write_csr(1, (uint16_t)(init_block_phys & 0xFFFF));
    pcnet_write_csr(2, (uint16_t)((init_block_phys >> 16) & 0xFFFF));

    /* Initialize controller */
    pcnet_write_csr(0, 0x0001);

    /* Wait for IDON (Initialization Done, bit 8) */
    uint32_t timeout = 100000;
    while (!(pcnet_read_csr(0) & (1 << 8)) && --timeout);
    if (!timeout) {
        serial_puts("[pcnet] Initialization timed out!\n");
        return -1;
    }

    /* Start the controller */
    pcnet_write_csr(0, 0x0042); /* STRT (bit 1) | IENA (bit 6) */

    pcnet_available = 1;
    serial_puts("[pcnet] Driver initialized. Card active and ready.\n");
    return 0;
}

int pcnet_send(const void *data, uint16_t len) {
    if (!pcnet_available || len > PCNET_BUF_SIZE) return -1;

    uint32_t cur = tx_tail;
    if (tx_ring[cur].status & 0x8000) {
        /* Ring full */
        return -1;
    }

    /* Copy data */
    const uint8_t *s = (const uint8_t *)data;
    uint8_t *d = tx_buffers[cur];
    for (uint16_t i = 0; i < len; i++) d[i] = s[i];

    tx_ring[cur].buf_len = (int16_t)((-len) | 0xF000);
    tx_ring[cur].misc    = 0;
    tx_ring[cur].status  = 0x8300; /* OWN | STP | ENP */

    /* Trigger transmit: TDMD = bit 3 */
    pcnet_write_csr(0, 0x0048);

    tx_tail = (cur + 1) % PCNET_NUM_TX_DESC;
    return 0;
}

int pcnet_recv(void *buf, uint16_t *out_len) {
    if (!pcnet_available) return -1;

    uint32_t cur = rx_tail;
    if (rx_ring[cur].status & 0x8000) {
        /* Still owned by card */
        return -1;
    }

    uint16_t len = (uint16_t)(rx_ring[cur].msg_len & 0x0FFF);
    if (len > PCNET_BUF_SIZE) len = PCNET_BUF_SIZE;

    const uint8_t *s = rx_buffers[cur];
    uint8_t *d = (uint8_t *)buf;
    for (uint16_t i = 0; i < len; i++) d[i] = s[i];
    *out_len = len;

    /* Hand descriptor back to card */
    rx_ring[cur].status = 0x8000;
    rx_tail = (cur + 1) % PCNET_NUM_RX_DESC;

    return 0;
}

void pcnet_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = pcnet_mac[i];
}

int pcnet_link_up(void) {
    return pcnet_available;
}

void pcnet_print_info(void) {
    if (!pcnet_available) {
        console_puts("pcnet: No NIC detected.\n");
        return;
    }
    console_puts("AMD PCnet-FAST III (Am79C973) 10/100 Ethernet\n");
    console_puts("  MAC: ");
    for (int i = 0; i < 6; i++) {
        static const char h[] = "0123456789ABCDEF";
        console_putc(h[(pcnet_mac[i] >> 4) & 0xF]);
        console_putc(h[pcnet_mac[i] & 0xF]);
        if (i < 5) console_putc(':');
    }
    console_putc('\n');
    console_puts("  Link: UP (100 Mbps)\n");
    console_puts("  RX descriptors: 16  TX descriptors: 16\n");
}
