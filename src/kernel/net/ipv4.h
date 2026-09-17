/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * ipv4.h - Internet Protocol version 4 (IPv4) definitions and functions.
 */

#ifndef KERNEL_IPV4_H
#define KERNEL_IPV4_H

#include <stdint.h>
#include <stddef.h>

struct ipv4_header {
    uint8_t  ver_ihl;      /* Version (4 bits) + IHL (4 bits) */
    uint8_t  tos;          /* Type of Service */
    uint16_t total_len;    /* Total Length */
    uint16_t id;           /* Identification */
    uint16_t flags_frag;   /* Flags (3 bits) + Fragment Offset (13 bits) */
    uint8_t  ttl;          /* Time to Live */
    uint8_t  protocol;     /* Protocol (TCP, UDP, ICMP) */
    uint16_t checksum;     /* Header Checksum */
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed));

void ipv4_handle_packet(const uint8_t *payload, uint16_t len);
int  ipv4_send(uint32_t dst_ip, uint8_t protocol, const void *payload, uint16_t payload_len);

#endif /* KERNEL_IPV4_H */
