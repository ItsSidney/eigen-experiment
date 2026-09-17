/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * ipv6.c - IPv6 packet decoding, ICMPv6 echo replies, and neighbor discovery.
 */

#include "ipv6.h"
#include "net.h"
#include "include/serial.h"

#define ICMPV6_ECHO_REQUEST          128
#define ICMPV6_ECHO_REPLY            129
#define ICMPV6_NEIGHBOR_SOLICIT      135
#define ICMPV6_NEIGHBOR_ADVERT       136

static void get_link_local_ip6(uint8_t ip6[16]) {
    ip6[0]  = 0xFE;
    ip6[1]  = 0x80;
    for (int i = 2; i < 8; i++) ip6[i] = 0;
    ip6[8]  = g_netif.mac[0] ^ 0x02;
    ip6[9]  = g_netif.mac[1];
    ip6[10] = g_netif.mac[2];
    ip6[11] = 0xFF;
    ip6[12] = 0xFE;
    ip6[13] = g_netif.mac[3];
    ip6[14] = g_netif.mac[4];
    ip6[15] = g_netif.mac[5];
}

static uint16_t icmpv6_checksum(const uint8_t src[16], const uint8_t dst[16], const void *payload, uint32_t len) {
    uint32_t sum = 0;
    const uint16_t *p;

    p = (const uint16_t *)src;
    for (int i = 0; i < 8; i++) sum += ntohs(p[i]);

    p = (const uint16_t *)dst;
    for (int i = 0; i < 8; i++) sum += ntohs(p[i]);

    sum += (len >> 16);
    sum += (len & 0xFFFF);
    sum += IPV6_NEXT_ICMPV6;

    p = (const uint16_t *)payload;
    uint32_t words = len / 2;
    for (uint32_t i = 0; i < words; i++) {
        sum += ntohs(p[i]);
    }
    if (len & 1) {
        sum += (uint16_t)(((const uint8_t *)payload)[len - 1] << 8);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return htons((uint16_t)~sum);
}

void ipv6_init(void) {
}

void ipv6_handle_packet(const uint8_t *src_mac, const uint8_t *payload, uint16_t len) {
    if (len < sizeof(struct ipv6_header)) return;

    const struct ipv6_header *ip6 = (const struct ipv6_header *)payload;
    uint8_t version = (ntohl(ip6->ver_tc_fl) >> 28) & 0x0F;
    if (version != 6) return;

    uint16_t plen = ntohs(ip6->payload_len);
    if (plen + sizeof(struct ipv6_header) > len) return;

    const uint8_t *data = payload + sizeof(struct ipv6_header);
    uint16_t data_len   = plen;

    uint8_t my_ip6[16];
    get_link_local_ip6(my_ip6);

    if (ip6->next_header == IPV6_NEXT_ICMPV6) {
        if (data_len < 4) return;
        uint8_t icmp6_type = data[0];

        if (icmp6_type == ICMPV6_ECHO_REQUEST) {
            uint8_t frame[ETH_MTU];
            struct ipv6_header *rep_ip6 = (struct ipv6_header *)frame;
            uint8_t *rep_icmp = frame + sizeof(struct ipv6_header);

            rep_ip6->ver_tc_fl   = htonl(0x60000000);
            rep_ip6->payload_len = htons(data_len);
            rep_ip6->next_header = IPV6_NEXT_ICMPV6;
            rep_ip6->hop_limit   = 64;

            for (int i = 0; i < 16; i++) {
                rep_ip6->src_ip[i] = my_ip6[i];
                rep_ip6->dst_ip[i] = ip6->src_ip[i];
            }

            for (uint16_t i = 0; i < data_len; i++) {
                rep_icmp[i] = data[i];
            }
            rep_icmp[0] = ICMPV6_ECHO_REPLY;
            rep_icmp[1] = 0;
            rep_icmp[2] = 0;
            rep_icmp[3] = 0;

            uint16_t csum = icmpv6_checksum(rep_ip6->src_ip, rep_ip6->dst_ip, rep_icmp, data_len);
            rep_icmp[2] = (uint8_t)(csum & 0xFF);
            rep_icmp[3] = (uint8_t)(csum >> 8);

            net_send_ethernet(src_mac, ETHERTYPE_IPV6, frame, sizeof(struct ipv6_header) + data_len);
        } else if (icmp6_type == ICMPV6_NEIGHBOR_SOLICIT) {
            if (data_len < 20) return;
            uint8_t frame[ETH_MTU];
            struct ipv6_header *rep_ip6 = (struct ipv6_header *)frame;
            uint8_t *rep_icmp = frame + sizeof(struct ipv6_header);
            uint16_t na_len = 32;

            rep_ip6->ver_tc_fl   = htonl(0x60000000);
            rep_ip6->payload_len = htons(na_len);
            rep_ip6->next_header = IPV6_NEXT_ICMPV6;
            rep_ip6->hop_limit   = 255;

            for (int i = 0; i < 16; i++) {
                rep_ip6->src_ip[i] = my_ip6[i];
                rep_ip6->dst_ip[i] = ip6->src_ip[i];
            }

            rep_icmp[0] = ICMPV6_NEIGHBOR_ADVERT;
            rep_icmp[1] = 0;
            rep_icmp[2] = 0;
            rep_icmp[3] = 0;
            /* Flags: Solicited (0x40) + Override (0x20) */
            rep_icmp[4] = 0x60;
            rep_icmp[5] = 0;
            rep_icmp[6] = 0;
            rep_icmp[7] = 0;

            /* Target Address */
            for (int i = 0; i < 16; i++) {
                rep_icmp[8 + i] = my_ip6[i];
            }

            /* Option: Target Link-Layer Address (Type 2, Length 1, MAC) */
            rep_icmp[24] = 2;
            rep_icmp[25] = 1;
            for (int i = 0; i < 6; i++) {
                rep_icmp[26 + i] = g_netif.mac[i];
            }

            uint16_t csum = icmpv6_checksum(rep_ip6->src_ip, rep_ip6->dst_ip, rep_icmp, na_len);
            rep_icmp[2] = (uint8_t)(csum & 0xFF);
            rep_icmp[3] = (uint8_t)(csum >> 8);

            net_send_ethernet(src_mac, ETHERTYPE_IPV6, frame, sizeof(struct ipv6_header) + na_len);
        }
    }
}
