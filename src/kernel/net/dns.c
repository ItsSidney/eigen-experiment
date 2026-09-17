/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * dns.c - Domain Name System (DNS) query engine and A-record parser.
 */

#include "dns.h"
#include "udp.h"
#include "net.h"
#include "include/serial.h"
#include "drivers/timer/pit.h"

static uint16_t dns_trans_id = 0x5412;
static volatile int dns_reply_received = 0;
static uint32_t dns_resolved_ip = 0;
static uint16_t dns_client_port = 53053;

/* Convert "www.google.com" into "\x03www\x06google\x03com\x00" */
static int dns_format_qname(const char *hostname, uint8_t *qname) {
    int qi = 0;
    const char *p = hostname;

    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int len = (int)(dot - p);
        if (len <= 0 || len > 63) return -1;

        qname[qi++] = (uint8_t)len;
        for (int i = 0; i < len; i++) qname[qi++] = (uint8_t)p[i];

        if (*dot == '.') p = dot + 1;
        else p = dot;
    }
    qname[qi++] = 0;
    return qi;
}

static void dns_udp_callback(uint32_t src_ip, uint16_t src_port,
                             uint16_t dst_port, const uint8_t *data,
                             uint16_t len) {
    (void)src_ip;
    (void)src_port;
    (void)dst_port;

    if (len < sizeof(struct dns_header)) return;

    const struct dns_header *dns = (const struct dns_header *)data;
    if (ntohs(dns->id) != dns_trans_id) return;

    uint16_t ancount = ntohs(dns->ancount);
    if (ancount == 0) return;

    /* Skip header and Question section */
    uint16_t offset = sizeof(struct dns_header);

    /* Skip QNAME */
    while (offset < len && data[offset] != 0) {
        offset += data[offset] + 1;
    }
    offset++; /* Skip trailing 0 */
    offset += 4; /* Skip QTYPE (2) + QCLASS (2) */

    /* Parse Answer section */
    for (uint16_t a = 0; a < ancount && offset < len; a++) {
        /* Check for compression pointer (starts with 11xxxxxx in binary, or >= 0xC0) */
        if (data[offset] >= 0xC0) {
            offset += 2; /* 2-byte pointer */
        } else {
            while (offset < len && data[offset] != 0) {
                offset += data[offset] + 1;
            }
            offset++;
        }

        if (offset + 10 > len) break;

        uint16_t rtype  = (uint16_t)((data[offset] << 8) | data[offset + 1]);
        uint16_t rclass = (uint16_t)((data[offset + 2] << 8) | data[offset + 3]);
        offset += 8; /* Skip rtype(2) + rclass(2) + ttl(4) */

        uint16_t rdlen  = (uint16_t)((data[offset] << 8) | data[offset + 1]);
        offset += 2;

        if (rtype == 1 && rclass == 1 && rdlen == 4 && offset + 4 <= len) {
            /* Type A, Class IN, IPv4 address found! */
            dns_resolved_ip = *(const uint32_t *)&data[offset];
            dns_reply_received = 1;
            return;
        }

        offset += rdlen;
    }
}

void dns_init(void) {
    dns_reply_received = 0;
    dns_resolved_ip = 0;
}

int dns_resolve(const char *hostname, uint32_t *ip_out, uint32_t timeout_ms) {
    if (!hostname || !ip_out) return -1;

    dns_reply_received = 0;
    dns_resolved_ip = 0;
    dns_trans_id++;

    uint8_t buffer[512];
    struct dns_header *dns = (struct dns_header *)buffer;

    dns->id      = htons(dns_trans_id);
    dns->flags   = htons(0x0100); /* Standard query, recursion desired */
    dns->qdcount = htons(1);
    dns->ancount = 0;
    dns->nscount = 0;
    dns->arcount = 0;

    int qname_len = dns_format_qname(hostname, buffer + sizeof(struct dns_header));
    if (qname_len <= 0) return -1;

    uint16_t offset = sizeof(struct dns_header) + qname_len;
    buffer[offset++] = 0; buffer[offset++] = 1; /* QTYPE = A (1) */
    buffer[offset++] = 0; buffer[offset++] = 1; /* QCLASS = IN (1) */

    udp_bind_handler(dns_client_port, dns_udp_callback);

    uint32_t dns_server = g_netif.dns ? g_netif.dns : IP_ADDR(10, 0, 2, 3);
    udp_send(dns_server, dns_client_port, DNS_PORT, buffer, offset);

    uint64_t start = timer_get_ticks();
    uint64_t timeout_ticks = (timeout_ms / 10) + 1;

    while (timer_get_ticks() - start < timeout_ticks) {
        net_poll();
        if (dns_reply_received) {
            udp_unbind_handler(dns_client_port);
            *ip_out = dns_resolved_ip;
            return 0; /* Resolved successfully */
        }
        __asm__ volatile("pause");
    }

    udp_unbind_handler(dns_client_port);
    return -1; /* Timed out */
}
