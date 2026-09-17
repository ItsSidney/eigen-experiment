/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * e1000.h - Intel 82540EM (e1000) Gigabit Ethernet driver.
 * Supports QEMU (-net nic,model=e1000) and VirtualBox (Intel PRO/1000 MT).
 * Both use PCI Vendor 0x8086, Device 0x100E.
 */

#ifndef KERNEL_E1000_H
#define KERNEL_E1000_H

#include <stdint.h>
#include <stddef.h>

/* ── e1000 Register Offsets (from BAR0 MMIO base) ────────────────────────── */
#define E1000_CTRL      0x0000  /* Device Control Register        */
#define E1000_STATUS    0x0008  /* Device Status Register         */
#define E1000_EECD      0x0010  /* EEPROM/Flash Control           */
#define E1000_EERD      0x0014  /* EEPROM Read Register           */
#define E1000_ICR       0x00C0  /* Interrupt Cause Read           */
#define E1000_ITR       0x00C4  /* Interrupt Throttling Rate      */
#define E1000_ICS       0x00C8  /* Interrupt Cause Set            */
#define E1000_IMS       0x00D0  /* Interrupt Mask Set/Read        */
#define E1000_IMC       0x00D8  /* Interrupt Mask Clear           */
#define E1000_RCTL      0x0100  /* Receive Control                */
#define E1000_TCTL      0x0400  /* Transmit Control               */
#define E1000_TIPG      0x0410  /* TX Inter-Packet Gap            */
#define E1000_RDBAL     0x2800  /* RX Descriptor Base Addr Low    */
#define E1000_RDBAH     0x2804  /* RX Descriptor Base Addr High   */
#define E1000_RDLEN     0x2808  /* RX Descriptor Ring Length      */
#define E1000_RDH       0x2810  /* RX Descriptor Head             */
#define E1000_RDT       0x2818  /* RX Descriptor Tail             */
#define E1000_TDBAL     0x3800  /* TX Descriptor Base Addr Low    */
#define E1000_TDBAH     0x3804  /* TX Descriptor Base Addr High   */
#define E1000_TDLEN     0x3808  /* TX Descriptor Ring Length      */
#define E1000_TDH       0x3810  /* TX Descriptor Head             */
#define E1000_TDT       0x3818  /* TX Descriptor Tail             */
#define E1000_MTA       0x5200  /* Multicast Table Array (128x4)  */
#define E1000_RAL0      0x5400  /* Receive Address Low  [0]       */
#define E1000_RAH0      0x5404  /* Receive Address High [0]       */

/* ── CTRL Register Bits ───────────────────────────────────────────────────── */
#define E1000_CTRL_FD       (1 << 0)   /* Full-Duplex                   */
#define E1000_CTRL_LRST     (1 << 3)   /* Link Reset                    */
#define E1000_CTRL_ASDE     (1 << 5)   /* Auto-Speed Detection Enable   */
#define E1000_CTRL_SLU      (1 << 6)   /* Set Link Up                   */
#define E1000_CTRL_ILOS     (1 << 7)   /* Invert Loss-of-Signal         */
#define E1000_CTRL_SPEED_10   (0 << 8) /* Speed: 10 Mbps                */
#define E1000_CTRL_SPEED_100  (1 << 8) /* Speed: 100 Mbps               */
#define E1000_CTRL_SPEED_1000 (2 << 8) /* Speed: 1000 Mbps              */
#define E1000_CTRL_SPEED_MASK (3 << 8)
#define E1000_CTRL_FRCSPD   (1 << 11)  /* Force Speed                   */
#define E1000_CTRL_FRCDPX   (1 << 12)  /* Force Duplex                  */
#define E1000_CTRL_RST      (1 << 26)  /* Device Reset (self-clearing)  */
#define E1000_CTRL_PHY_RST  (1 << 31)  /* PHY Reset                     */

/* ── RCTL (Receive Control) Bits ─────────────────────────────────────────── */
#define E1000_RCTL_EN       (1 << 1)   /* Receiver Enable               */
#define E1000_RCTL_SBP      (1 << 2)   /* Store Bad Packets             */
#define E1000_RCTL_UPE      (1 << 3)   /* Unicast Promiscuous Enable    */
#define E1000_RCTL_MPE      (1 << 4)   /* Multicast Promiscuous Enable  */
#define E1000_RCTL_LPE      (1 << 5)   /* Long Packet Enable (>1522)    */
#define E1000_RCTL_BAM      (1 << 15)  /* Broadcast Accept Mode         */
#define E1000_RCTL_BSIZE_2048  (0 << 16) /* Buffer size: 2048 bytes     */
#define E1000_RCTL_BSIZE_1024  (1 << 16) /* Buffer size: 1024 bytes     */
#define E1000_RCTL_BSIZE_512   (2 << 16) /* Buffer size: 512 bytes      */
#define E1000_RCTL_BSIZE_256   (3 << 16) /* Buffer size: 256 bytes      */
#define E1000_RCTL_SECRC    (1 << 26)  /* Strip Ethernet CRC            */

/* ── TCTL (Transmit Control) Bits ────────────────────────────────────────── */
#define E1000_TCTL_EN       (1 << 1)   /* Transmitter Enable            */
#define E1000_TCTL_PSP      (1 << 3)   /* Pad Short Packets             */
#define E1000_TCTL_CT_SHIFT  4
#define E1000_TCTL_COLD_SHIFT 12

/* ── Interrupt Cause Bits (ICR) ──────────────────────────────────────────── */
#define E1000_ICR_TXDW      (1 << 0)   /* TX Descriptor Written Back    */
#define E1000_ICR_TXQE      (1 << 1)   /* TX Queue Empty                */
#define E1000_ICR_LSC       (1 << 2)   /* Link Status Change            */
#define E1000_ICR_RXSEQ     (1 << 3)   /* RX Sequence Error             */
#define E1000_ICR_RXDMT0    (1 << 4)   /* RX Descriptor Min Threshold   */
#define E1000_ICR_RXO       (1 << 6)   /* RX Overrun                    */
#define E1000_ICR_RXT0      (1 << 7)   /* RX Timer Interrupt            */

/* ── EEPROM Read (EERD) bits ──────────────────────────────────────────────── */
#define E1000_EERD_START    (1 << 0)   /* Start EEPROM read             */
#define E1000_EERD_DONE     (1 << 4)   /* EEPROM read complete          */
#define E1000_EERD_ADDR_SHIFT 8
#define E1000_EERD_DATA_SHIFT 16

/* ── RAH0 bits ────────────────────────────────────────────────────────────── */
#define E1000_RAH_AV        (1U << 31)  /* Address Valid                 */

/* ── Descriptor sizes ────────────────────────────────────────────────────── */
#define E1000_NUM_RX_DESC   32
#define E1000_NUM_TX_DESC   32
#define E1000_RX_BUF_SIZE   2048
#define E1000_TX_BUF_SIZE   2048

/* ── RX Descriptor (16 bytes, hardware spec) ─────────────────────────────── */
struct e1000_rx_desc {
    volatile uint64_t addr;     /* Physical address of RX buffer        */
    volatile uint16_t length;   /* Length of received data              */
    volatile uint16_t checksum; /* Checksum                             */
    volatile uint8_t  status;   /* Descriptor status (bit 0 = DD done)  */
    volatile uint8_t  errors;   /* Receive errors                       */
    volatile uint16_t special;  /* VLAN info                            */
} __attribute__((packed));

/* ── TX Descriptor (16 bytes, hardware spec) ─────────────────────────────── */
struct e1000_tx_desc {
    volatile uint64_t addr;     /* Physical address of TX buffer        */
    volatile uint16_t length;   /* Length of data to transmit           */
    volatile uint8_t  cso;      /* Checksum Offset                      */
    volatile uint8_t  cmd;      /* Command (EOP, IFCS, RS, etc.)        */
    volatile uint8_t  status;   /* Descriptor status (bit 0 = DD done)  */
    volatile uint8_t  css;      /* Checksum Start                       */
    volatile uint16_t special;  /* VLAN tag                             */
} __attribute__((packed));

/* TX Command bits */
#define E1000_TXD_CMD_EOP   (1 << 0)   /* End of Packet                 */
#define E1000_TXD_CMD_IFCS  (1 << 1)   /* Insert FCS (CRC)              */
#define E1000_TXD_CMD_RS    (1 << 3)   /* Report Status (set DD)        */

/* TX/RX Status bits */
#define E1000_TXD_STAT_DD   (1 << 0)   /* Descriptor Done               */
#define E1000_RXD_STAT_DD   (1 << 0)   /* Descriptor Done               */
#define E1000_RXD_STAT_EOP  (1 << 1)   /* End of Packet                 */

/* ── Driver state ────────────────────────────────────────────────────────── */
struct e1000_dev {
    uint64_t  mmio_base;        /* Kernel virtual address of BAR0 MMIO  */
    uint8_t   mac[6];           /* MAC address read from EEPROM          */

    /* RX ring */
    struct e1000_rx_desc *rx_descs;  /* Kernel virtual addr of RX ring   */
    uint64_t              rx_phys;   /* Physical address of RX ring       */
    uint8_t  *rx_bufs[E1000_NUM_RX_DESC];  /* Per-descriptor RX buffers  */
    uint64_t  rx_bufs_phys[E1000_NUM_RX_DESC];
    uint32_t  rx_tail;

    /* TX ring */
    struct e1000_tx_desc *tx_descs;  /* Kernel virtual addr of TX ring   */
    uint64_t              tx_phys;   /* Physical address of TX ring       */
    uint8_t  *tx_bufs[E1000_NUM_TX_DESC];  /* Per-descriptor TX buffers  */
    uint64_t  tx_bufs_phys[E1000_NUM_TX_DESC];
    uint32_t  tx_tail;

    int       link_up;           /* 1 if link is up                      */
    uint32_t  irq_line;          /* IRQ line from PCI config             */
};

extern struct e1000_dev g_e1000;
extern int e1000_available;

/* Public API */
int     e1000_init(void);
int     e1000_send(const void *data, uint16_t len);
int     e1000_recv(void *buf, uint16_t *out_len);
void    e1000_get_mac(uint8_t mac[6]);
int     e1000_link_up(void);
void    e1000_print_info(void);

#endif /* KERNEL_E1000_H */
