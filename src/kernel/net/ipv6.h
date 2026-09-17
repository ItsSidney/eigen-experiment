/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * ipv6.h - Internet Protocol version 6 (IPv6) header and handling.
 */

#ifndef KERNEL_IPV6_H
#define KERNEL_IPV6_H

#include <stdint.h>
#include <stddef.h>

#define IPV6_NEXT_ICMPV6 58
#define IPV6_NEXT_TCP    6
#define IPV6_NEXT_UDP    17

struct ipv6_header {
    uint32_t ver_tc_fl;    /* Version (4 bits) + TC (8) + Flow (20) */
    uint16_t payload_len;  /* Length of payload */
    uint8_t  next_header;  /* Next header type */
    uint8_t  hop_limit;    /* Hop limit */
    uint8_t  src_ip[16];   /* Source address */
    uint8_t  dst_ip[16];   /* Destination address */
} __attribute__((packed));

void ipv6_init(void);
void ipv6_handle_packet(const uint8_t *src_mac, const uint8_t *payload, uint16_t len);

#endif /* KERNEL_IPV6_H */
