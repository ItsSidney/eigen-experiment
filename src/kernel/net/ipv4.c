/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * ipv4.c - IPv4 packet transmission, routing, and reception dispatcher.
 */

#include "ipv4.h"
#include "net.h"
#include "arp.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "include/serial.h"

static uint16_t ip_id_counter = 1000;

/* Forward: reception dispatcher below (used for local delivery). */
void ipv4_handle_packet(const uint8_t *payload, uint16_t len);

int ipv4_send(uint32_t dst_ip, uint8_t protocol, const void *payload, uint16_t payload_len) {
    if (payload_len + sizeof(struct ipv4_header) > ETH_MTU) return -1;

    /* Buffer for IP header + payload */
    uint8_t packet[ETH_MTU];
    struct ipv4_header *ip = (struct ipv4_header *)packet;

    ip->ver_ihl     = (4 << 4) | (sizeof(struct ipv4_header) / 4);
    ip->tos         = 0;
    ip->total_len   = htons(sizeof(struct ipv4_header) + payload_len);
    ip->id          = htons(ip_id_counter++);
    ip->flags_frag  = htons(0x4000); /* Don't Fragment */
    ip->ttl         = 64;
    ip->protocol    = protocol;
    ip->checksum    = 0;
    ip->src_ip      = g_netif.ip;
    ip->dst_ip      = dst_ip;

    /* Compute IPv4 header checksum */
    ip->checksum = net_checksum(ip, sizeof(struct ipv4_header));

    /* Copy payload */
    const uint8_t *src = (const uint8_t *)payload;
    uint8_t *dst = packet + sizeof(struct ipv4_header);
    for (uint16_t i = 0; i < payload_len; i++) dst[i] = src[i];

    uint16_t total = sizeof(struct ipv4_header) + payload_len;

    /* Local delivery: packets to our own address never touch the wire.
     * (No loopback device exists, so without this self-ping and any
     * TCP/UDP-to-self traffic is blackholed by the gateway.) ICMP and
     * UDP loop back directly; TCP self-connect stays on the wire path. */
    if (g_netif.ip != 0 && dst_ip == g_netif.ip &&
        (protocol == IP_PROTO_ICMP || protocol == IP_PROTO_UDP)) {
        ipv4_handle_packet(packet, total);
        return 0;
    }

    /* Routing: QEMU user networking is a point-to-point link behind slirp.
     * Everything that is not a broadcast goes to the gateway's MAC, and
     * slirp decides what to do with it based on the IP header — including
     * same-subnet addresses like the DNS proxy at 10.0.2.3, which does not
     * answer ARP directly. The old code tried to ARP same-subnet targets,
     * which worked for 10.0.2.2 but silently dropped 10.0.2.3. */
    uint32_t next_hop;
    if (dst_ip == 0xFFFFFFFF || (dst_ip & ~g_netif.netmask) == ~g_netif.netmask) {
        next_hop = 0xFFFFFFFF;
    } else if (g_netif.gateway) {
        next_hop = g_netif.gateway;
    } else {
        next_hop = dst_ip;
    }

    uint8_t dst_mac[6];
    if (arp_resolve(next_hop, dst_mac) != 0) {
        /* Direct ARP failed (or was never needed because we route through
         * the gateway). Without a gateway there is nowhere to send. */
        return -1;
    }

    return net_send_ethernet(dst_mac, ETHERTYPE_IPV4, packet, total);
}

void ipv4_handle_packet(const uint8_t *payload, uint16_t len) {
    if (len < sizeof(struct ipv4_header)) return;

    const struct ipv4_header *ip = (const struct ipv4_header *)payload;
    uint8_t version = (ip->ver_ihl >> 4) & 0x0F;
    uint8_t ihl     = (ip->ver_ihl & 0x0F) * 4;

    /* TEMP-DEBUG */
    serial_puts("[IP4] rx src=");
    serial_puthex(ip->src_ip);
    serial_puts(" dst=");
    serial_puthex(ip->dst_ip);
    serial_puts(" proto=");
    serial_puthex(ip->protocol);
    serial_puts("\n");

    if (version != 4 || ihl < sizeof(struct ipv4_header) || ihl > len) return;

    uint16_t total_len = ntohs(ip->total_len);
    if (total_len > len) return;

    /* Verify destination is our IP or broadcast */
    if (ip->dst_ip != g_netif.ip &&
        ip->dst_ip != 0xFFFFFFFF &&
        g_netif.ip != 0) {
        return;
    }

    const uint8_t *data = payload + ihl;
    uint16_t data_len = total_len - ihl;

    switch (ip->protocol) {
        case IP_PROTO_ICMP:
            icmp_handle_packet(ip->src_ip, data, data_len);
            break;
        case IP_PROTO_UDP:
            udp_handle_packet(ip->src_ip, data, data_len);
            break;
        case IP_PROTO_TCP:
            tcp_handle_packet(ip->src_ip, data, data_len);
            break;
        default:
            break;
    }
}
