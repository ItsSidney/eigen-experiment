/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * udp.c - UDP packet transmission and port binding.
 */

#include "udp.h"
#include "ipv4.h"
#include "net.h"
#include "include/serial.h"

#define MAX_UDP_BINDINGS 48

struct udp_binding {
    uint16_t      port;
    udp_handler_t handler;
    int           active;
};

static struct udp_binding udp_bindings[MAX_UDP_BINDINGS];

void udp_init(void) {
    for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
        udp_bindings[i].active = 0;
    }
}

int udp_bind_handler(uint16_t port, udp_handler_t handler) {
    for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
        if (!udp_bindings[i].active) {
            udp_bindings[i].port = port;
            udp_bindings[i].handler = handler;
            udp_bindings[i].active = 1;
            return 0;
        }
    }
    return -1;
}

void udp_unbind_handler(uint16_t port) {
    for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
        if (udp_bindings[i].active && udp_bindings[i].port == port) {
            udp_bindings[i].active = 0;
            return;
        }
    }
}

int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, const void *data, uint16_t len) {
    uint8_t buffer[ETH_MTU];
    struct udp_header *udp = (struct udp_header *)buffer;

    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons(sizeof(struct udp_header) + len);
    udp->checksum = 0; /* UDP checksum is optional over IPv4 */

    const uint8_t *src = (const uint8_t *)data;
    uint8_t *dst = buffer + sizeof(struct udp_header);
    for (uint16_t i = 0; i < len; i++) dst[i] = src[i];

    uint16_t total = sizeof(struct udp_header) + len;
    /* TEMP-DEBUG */
    serial_puts("[UDP] tx dst=");
    serial_puthex(dst_ip);
    serial_puts(" sport=");
    serial_puthex(src_port);
    serial_puts(" dport=");
    serial_puthex(dst_port);
    serial_puts("\n");
    if (dst_port == 53) {
        uint16_t k;
        serial_puts("[UDP] dns bytes:");
        for (k = 0; k < len && k < 48; k++) {
            serial_puts(" ");
            serial_puthex(((const uint8_t *)data)[k] & 0xFF);
        }
        serial_puts("\n");
    }
    return ipv4_send(dst_ip, IP_PROTO_UDP, buffer, total);
}

void udp_handle_packet(uint32_t src_ip, const uint8_t *payload, uint16_t len) {
    if (len < sizeof(struct udp_header)) return;

    const struct udp_header *udp = (const struct udp_header *)payload;
    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t src_port = ntohs(udp->src_port);
    /* TEMP-DEBUG */
    serial_puts("[UDP] rx sport=");
    serial_puthex(src_port);
    serial_puts(" dport=");
    serial_puthex(dst_port);
    serial_puts(" len=");
    serial_puthex(len);
    serial_puts("\n");
    uint16_t udp_len  = ntohs(udp->length);
    if (udp_len > len || udp_len < sizeof(struct udp_header)) return;

    const uint8_t *data = payload + sizeof(struct udp_header);
    uint16_t data_len   = udp_len - sizeof(struct udp_header);

    for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
        if (udp_bindings[i].active && udp_bindings[i].port == dst_port) {
            udp_bindings[i].handler(src_ip, src_port, dst_port, data, data_len);
            return;
        }
    }
}
