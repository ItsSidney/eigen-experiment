/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * udp.h - User Datagram Protocol (UDP) definitions and API.
 */

#ifndef KERNEL_UDP_H
#define KERNEL_UDP_H

#include <stdint.h>
#include <stddef.h>

struct udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

/* Destination port is part of the callback contract: a protocol handler
 * cannot demultiplex without it. Without it, every UDP reply is handed to
 * whichever socket happens to sit first in the table, so DNS, NTP and any
 * other userland client steal each other's datagrams. */
typedef void (*udp_handler_t)(uint32_t src_ip, uint16_t src_port,
                              uint16_t dst_port, const uint8_t *data,
                              uint16_t len);

void udp_init(void);
int  udp_bind_handler(uint16_t port, udp_handler_t handler);
void udp_unbind_handler(uint16_t port);
void udp_handle_packet(uint32_t src_ip, const uint8_t *payload, uint16_t len);
int  udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, const void *data, uint16_t len);

#endif /* KERNEL_UDP_H */
