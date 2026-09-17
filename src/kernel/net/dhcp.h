/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * dhcp.h - Dynamic Host Configuration Protocol (DHCP) client.
 */

#ifndef KERNEL_DHCP_H
#define KERNEL_DHCP_H

#include <stdint.h>
#include <stddef.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_OP_REQUEST  1
#define DHCP_OP_REPLY    2

#define DHCP_MAGIC_COOKIE 0x63825363

struct dhcp_packet {
    uint8_t  op;           /* 1 = BOOTREQUEST, 2 = BOOTREPLY */
    uint8_t  htype;        /* Hardware type (1 = Ethernet) */
    uint8_t  hlen;         /* Hardware address length (6) */
    uint8_t  hops;
    uint32_t xid;          /* Transaction ID */
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;       /* Client IP */
    uint32_t yiaddr;       /* Your IP (from server) */
    uint32_t siaddr;       /* Server IP */
    uint32_t giaddr;       /* Gateway IP */
    uint8_t  chaddr[16];   /* Client hardware address */
    uint8_t  sname[64];    /* Server host name */
    uint8_t  file[128];    /* Boot file name */
    uint32_t magic;        /* 0x63825363 */
    uint8_t  options[312]; /* Options */
} __attribute__((packed));

int dhcp_request_ip(uint32_t timeout_ms);

#endif /* KERNEL_DHCP_H */
