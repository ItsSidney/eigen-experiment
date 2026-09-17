/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * net.h - Core network subsystem, byte order macros, and protocol definitions.
 */

#ifndef KERNEL_NET_H
#define KERNEL_NET_H

#include <stdint.h>
#include <stddef.h>

/* EtherTypes */
#define ETHERTYPE_IPV4  0x0800
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IPV6  0x86DD

/* IP Protocols */
#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

/* Standard MTU */
#define ETH_MTU         1500
#define ETH_HDR_LEN     14
#define ETH_FRAME_MAX   1518

/* Byte order utilities (x86_64 is Little Endian; network is Big Endian) */
static inline uint16_t htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint16_t ntohs(uint16_t v) {
    return htons(v);
}

static inline uint32_t htonl(uint32_t v) {
    return ((v & 0x000000FFU) << 24) |
           ((v & 0x0000FF00U) << 8)  |
           ((v & 0x00FF0000U) >> 8)  |
           ((v & 0xFF000000U) >> 24);
}

static inline uint32_t ntohl(uint32_t v) {
    return htonl(v);
}

#define IP_ADDR(a, b, c, d) \
    ((uint32_t)(((uint8_t)(a)) | (((uint8_t)(b)) << 8) | (((uint8_t)(c)) << 16) | (((uint8_t)(d)) << 24)))

/* Ethernet Header */
struct eth_header {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;
} __attribute__((packed));

/* Network interface state */
struct netif {
    uint8_t  mac[6];
    uint32_t ip;        /* in network byte order */
    uint32_t netmask;   /* in network byte order */
    uint32_t gateway;   /* in network byte order */
    uint32_t dns;       /* in network byte order */
    int      configured;
};

extern struct netif g_netif;

/* Interface counters (frames incl. Ethernet header). */
extern uint64_t net_tx_packets;
extern uint64_t net_tx_bytes;
extern uint64_t net_rx_packets;
extern uint64_t net_rx_bytes;

void     net_init(void);
void     net_poll(void);
void     net_handle_packet(const uint8_t *packet, uint16_t len);
int      net_send_ethernet(const uint8_t dst_mac[6], uint16_t ethertype, const void *payload, uint16_t len);
uint16_t net_checksum(const void *data, size_t len);

void     net_print_ip(uint32_t ip);
void     net_print_info(void);

#endif /* KERNEL_NET_H */
