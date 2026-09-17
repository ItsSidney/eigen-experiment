/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * arp.h - Address Resolution Protocol (ARP) definitions and cache.
 */

#ifndef KERNEL_ARP_H
#define KERNEL_ARP_H

#include <stdint.h>
#include <stddef.h>

#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

struct arp_header {
    uint16_t htype;      /* Hardware type (1 = Ethernet) */
    uint16_t ptype;      /* Protocol type (0x0800 = IPv4) */
    uint8_t  hlen;       /* Hardware address length (6) */
    uint8_t  plen;       /* Protocol address length (4) */
    uint16_t opcode;     /* Opcode (1 = Request, 2 = Reply) */
    uint8_t  sender_mac[6];
    uint32_t sender_ip;
    uint8_t  target_mac[6];
    uint32_t target_ip;
} __attribute__((packed));

void arp_init(void);
void arp_cache_insert(uint32_t ip, const uint8_t mac[6]);
void arp_handle_packet(const uint8_t *payload, uint16_t len);
int  arp_resolve(uint32_t ip, uint8_t mac_out[6]);
void arp_send_request(uint32_t ip);

#endif /* KERNEL_ARP_H */
