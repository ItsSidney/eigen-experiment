/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * pcnet.h - AMD PCnet-FAST III (Am79C973) network interface driver.
 * Default VirtualBox adapter (PCI Vendor 0x1022, Device 0x2000).
 */

#ifndef KERNEL_PCNET_H
#define KERNEL_PCNET_H

#include <stdint.h>
#include <stddef.h>

#define PCNET_NUM_RX_DESC 16
#define PCNET_NUM_TX_DESC 16
#define PCNET_BUF_SIZE    1536

/* Initialization Block (32-bit mode) */
struct pcnet_init_block {
    uint16_t mode;
    uint8_t  rlen;              /* (log2(num_rx) << 4) */
    uint8_t  tlen;              /* (log2(num_tx) << 4) */
    uint8_t  mac[6];
    uint16_t reserved;
    uint32_t ladrf[2];          /* Logical address filter */
    uint32_t rx_ring_phys;
    uint32_t tx_ring_phys;
} __attribute__((packed, aligned(4)));

/* RX Descriptor (32-bit software mode) */
struct pcnet_rx_desc {
    uint32_t buf_phys;
    int16_t  buf_len;           /* 2's complement of buffer size | 0xF000 */
    uint16_t status;            /* Bit 15: OWN (1=card, 0=host) */
    uint32_t msg_len;           /* Bit 0..11: actual length received */
    uint32_t reserved;
} __attribute__((packed, aligned(4)));

/* TX Descriptor (32-bit software mode) */
struct pcnet_tx_desc {
    uint32_t buf_phys;
    int16_t  buf_len;           /* 2's complement of length | 0xF000 */
    uint16_t status;            /* Bit 15: OWN, bit 9: STP, bit 8: ENP */
    uint32_t misc;
    uint32_t reserved;
} __attribute__((packed, aligned(4)));

extern int pcnet_available;

int  pcnet_init(void);
int  pcnet_send(const void *data, uint16_t len);
int  pcnet_recv(void *buf, uint16_t *out_len);
void pcnet_get_mac(uint8_t mac[6]);
int  pcnet_link_up(void);
void pcnet_print_info(void);

#endif /* KERNEL_PCNET_H */
