/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * icmp.h - Internet Control Message Protocol (ICMP) echo and ping.
 */

#ifndef KERNEL_ICMP_H
#define KERNEL_ICMP_H

#include <stdint.h>
#include <stddef.h>

#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_ECHO_REQUEST  8

struct icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} __attribute__((packed));

void icmp_init(void);
void icmp_handle_packet(uint32_t src_ip, const uint8_t *payload, uint16_t len);
int  icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq, const void *data, uint16_t len);
int  icmp_ping(uint32_t dst_ip, uint32_t timeout_ms);

#endif /* KERNEL_ICMP_H */
