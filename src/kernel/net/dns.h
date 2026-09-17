/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * dns.h - Domain Name System (DNS) resolver client.
 */

#ifndef KERNEL_DNS_H
#define KERNEL_DNS_H

#include <stdint.h>
#include <stddef.h>

#define DNS_PORT 53

struct dns_header {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed));

void dns_init(void);
int  dns_resolve(const char *hostname, uint32_t *ip_out, uint32_t timeout_ms);

#endif /* KERNEL_DNS_H */
