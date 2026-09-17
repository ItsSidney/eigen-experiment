/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * net.c - Network layer initialization, polling, checksums, and packet dispatch.
 */

#include "net.h"
#include "arp.h"
#include "ipv4.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "dhcp.h"
#include "socket.h"
#include "ipv6.h"
#include "drivers/net/e1000.h"
#include "drivers/net/pcnet.h"
#include "drivers/char/console.h"
#include "include/serial.h"

struct netif g_netif;

/* Interface counters for /proc/net/dev. */
uint64_t net_tx_packets = 0;
uint64_t net_tx_bytes = 0;
uint64_t net_rx_packets = 0;
uint64_t net_rx_bytes = 0;

uint16_t net_checksum(const void *data, size_t len) {
    uint32_t sum = 0;
    const uint16_t *p = (const uint16_t *)data;

    for (size_t i = 0; i < len / 2; i++) {
        sum += ntohs(p[i]);
    }

    if (len & 1) {
        sum += (uint16_t)(((const uint8_t *)data)[len - 1] << 8);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return htons((uint16_t)~sum);
}

int net_send_ethernet(const uint8_t dst_mac[6], uint16_t ethertype, const void *payload, uint16_t len) {
    if (len + sizeof(struct eth_header) > ETH_FRAME_MAX) return -1;

    uint8_t frame[ETH_FRAME_MAX];
    struct eth_header *eth = (struct eth_header *)frame;

    for (int i = 0; i < 6; i++) {
        eth->dst_mac[i] = dst_mac[i];
        eth->src_mac[i] = g_netif.mac[i];
    }
    eth->ethertype = htons(ethertype);

    const uint8_t *src = (const uint8_t *)payload;
    uint8_t *dst = frame + sizeof(struct eth_header);
    for (uint16_t i = 0; i < len; i++) dst[i] = src[i];

    uint16_t total = sizeof(struct eth_header) + len;
    int r = -1;
    if (e1000_available) {
        r = e1000_send(frame, total);
    } else if (pcnet_available) {
        r = pcnet_send(frame, total);
    }
    if (r == 0) {
        net_tx_packets++;
        net_tx_bytes += total;
    }
    return r;
}

void net_handle_packet(const uint8_t *packet, uint16_t len) {
    if (len < sizeof(struct eth_header)) return;

    const struct eth_header *eth = (const struct eth_header *)packet;

    /* Verify destination is our MAC or broadcast */
    int for_us = 1;
    int is_bcast = 1;
    for (int i = 0; i < 6; i++) {
        if (eth->dst_mac[i] != g_netif.mac[i]) for_us = 0;
        if (eth->dst_mac[i] != 0xFF) is_bcast = 0;
    }

    if (!for_us && !is_bcast) return;

    uint16_t ethertype = ntohs(eth->ethertype);
    const uint8_t *payload = packet + sizeof(struct eth_header);
    uint16_t payload_len   = len - sizeof(struct eth_header);

    switch (ethertype) {
        case ETHERTYPE_ARP:
            arp_handle_packet(payload, payload_len);
            break;
        case ETHERTYPE_IPV4:
            ipv4_handle_packet(payload, payload_len);
            break;
        case ETHERTYPE_IPV6:
            ipv6_handle_packet(eth->src_mac, payload, payload_len);
            break;
        default:
            break;
    }
}

void net_poll(void) {
    uint8_t rx_buf[ETH_FRAME_MAX];
    uint16_t rx_len = 0;

    if (e1000_available) {
        while (e1000_recv(rx_buf, &rx_len) == 0) {
            if (rx_len > 0) {
                net_rx_packets++;
                net_rx_bytes += rx_len;
                net_handle_packet(rx_buf, rx_len);
            }
        }
    } else if (pcnet_available) {
        while (pcnet_recv(rx_buf, &rx_len) == 0) {
            if (rx_len > 0) {
                net_rx_packets++;
                net_rx_bytes += rx_len;
                net_handle_packet(rx_buf, rx_len);
            }
        }
    }
}

void net_print_ip(uint32_t ip) {
    uint8_t *b = (uint8_t *)&ip;
    for (int i = 0; i < 4; i++) {
        /* Print decimal byte */
        uint8_t val = b[i];
        if (val >= 100) serial_putc('0' + (val / 100));
        if (val >= 10)  serial_putc('0' + ((val / 10) % 10));
        serial_putc('0' + (val % 10));
        if (i < 3) serial_putc('.');
    }
}

void net_init(void) {
    if (e1000_available) {
        e1000_get_mac(g_netif.mac);
    } else if (pcnet_available) {
        pcnet_get_mac(g_netif.mac);
    } else {
        serial_puts("[NET] No network card available; skipping net stack init.\n");
        return;
    }

    g_netif.ip         = 0;
    g_netif.netmask    = 0;
    g_netif.gateway    = 0;
    g_netif.dns        = 0;
    g_netif.configured = 0;

    arp_init();
    icmp_init();
    udp_init();
    tcp_init();
    socket_init();

    serial_puts("[NET] Network subsystem initialized.\n");

    /* Attempt DHCP configuration */
    if (dhcp_request_ip(2000) != 0) {
        /* Fallback default QEMU/VirtualBox user NAT settings */
        serial_puts("[NET] Setting fallback IP 10.0.2.15\n");
        g_netif.ip         = IP_ADDR(10, 0, 2, 15);
        g_netif.netmask    = IP_ADDR(255, 255, 255, 0);
        g_netif.gateway    = IP_ADDR(10, 0, 2, 2);
        g_netif.dns        = IP_ADDR(10, 0, 2, 3);
        g_netif.configured = 1;
    }
}

void net_print_info(void) {
    if (e1000_available) {
        e1000_print_info();
    } else if (pcnet_available) {
        pcnet_print_info();
    } else {
        console_puts("net: No network adapter detected.\n");
        return;
    }
    if (g_netif.configured) {
        console_puts("  IPv4 Address:   ");
        uint8_t *b = (uint8_t *)&g_netif.ip;
        for (int i = 0; i < 4; i++) {
            uint8_t val = b[i];
            if (val >= 100) console_putc('0' + (val / 100));
            if (val >= 10)  console_putc('0' + ((val / 10) % 10));
            console_putc('0' + (val % 10));
            if (i < 3) console_putc('.');
        }
        console_puts("\n  Subnet Mask:    ");
        b = (uint8_t *)&g_netif.netmask;
        for (int i = 0; i < 4; i++) {
            uint8_t val = b[i];
            if (val >= 100) console_putc('0' + (val / 100));
            if (val >= 10)  console_putc('0' + ((val / 10) % 10));
            console_putc('0' + (val % 10));
            if (i < 3) console_putc('.');
        }
        console_puts("\n  Default Router: ");
        b = (uint8_t *)&g_netif.gateway;
        for (int i = 0; i < 4; i++) {
            uint8_t val = b[i];
            if (val >= 100) console_putc('0' + (val / 100));
            if (val >= 10)  console_putc('0' + ((val / 10) % 10));
            console_putc('0' + (val % 10));
            if (i < 3) console_putc('.');
        }
        console_puts("\n  DNS Resolver:   ");
        b = (uint8_t *)&g_netif.dns;
        for (int i = 0; i < 4; i++) {
            uint8_t val = b[i];
            if (val >= 100) console_putc('0' + (val / 100));
            if (val >= 10)  console_putc('0' + ((val / 10) % 10));
            console_putc('0' + (val % 10));
            if (i < 3) console_putc('.');
        }
        console_putc('\n');
    }
}
