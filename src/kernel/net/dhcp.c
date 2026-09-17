/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * dhcp.c - DHCP client state machine and IP auto-configuration.
 */

#include "dhcp.h"
#include "udp.h"
#include "net.h"
#include "include/serial.h"
#include "drivers/timer/pit.h"

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

#define DHCP_OPT_PAD           0
#define DHCP_OPT_SUBNET_MASK   1
#define DHCP_OPT_ROUTER        3
#define DHCP_OPT_DNS           6
#define DHCP_OPT_REQUESTED_IP  50
#define DHCP_OPT_MSG_TYPE      53
#define DHCP_OPT_SERVER_ID     54
#define DHCP_OPT_PARAM_REQ     55
#define DHCP_OPT_END           255

static uint32_t dhcp_xid = 0x3903F326;
static volatile int dhcp_state = 0; /* 0=idle, 1=wait offer, 2=wait ack, 3=done */
static uint32_t offered_ip = 0;
static uint32_t server_ip = 0;
static uint32_t offered_mask = 0;
static uint32_t offered_gw = 0;
static uint32_t offered_dns = 0;

static void dhcp_udp_callback(uint32_t src_ip, uint16_t src_port,
                              uint16_t dst_port, const uint8_t *data,
                              uint16_t len) {
    (void)src_ip;
    (void)src_port;
    (void)dst_port;

    if (len < sizeof(struct dhcp_packet) - 312) return;

    const struct dhcp_packet *p = (const struct dhcp_packet *)data;
    if (p->op != DHCP_OP_REPLY || p->xid != dhcp_xid) return;

    /* Parse options */
    uint8_t msg_type = 0;
    const uint8_t *opts = p->options;
    uint16_t opt_len = len - (sizeof(struct dhcp_packet) - 312);
    uint16_t idx = 0;

    while (idx < opt_len && opts[idx] != DHCP_OPT_END) {
        uint8_t tag = opts[idx++];
        if (tag == DHCP_OPT_PAD) continue;
        if (idx >= opt_len) break;
        uint8_t l = opts[idx++];
        if (idx + l > opt_len) break;

        if (tag == DHCP_OPT_MSG_TYPE && l == 1) {
            msg_type = opts[idx];
        } else if (tag == DHCP_OPT_SUBNET_MASK && l == 4) {
            offered_mask = *(const uint32_t *)&opts[idx];
        } else if (tag == DHCP_OPT_ROUTER && l >= 4) {
            offered_gw = *(const uint32_t *)&opts[idx];
        } else if (tag == DHCP_OPT_DNS && l >= 4) {
            offered_dns = *(const uint32_t *)&opts[idx];
        } else if (tag == DHCP_OPT_SERVER_ID && l == 4) {
            server_ip = *(const uint32_t *)&opts[idx];
        }

        idx += l;
    }

    if (dhcp_state == 1 && msg_type == DHCP_OFFER) {
        offered_ip = p->yiaddr;
        dhcp_state = 2; /* Offer received, proceed to request */
    } else if (dhcp_state == 2 && msg_type == DHCP_ACK) {
        dhcp_state = 3; /* ACK received, configuration complete */
    }
}

static void dhcp_send_discover(void) {
    struct dhcp_packet p;
    for (size_t i = 0; i < sizeof(p); i++) ((uint8_t *)&p)[i] = 0;

    p.op    = DHCP_OP_REQUEST;
    p.htype = 1; /* 10Mb Ethernet */
    p.hlen  = 6; /* MAC length */
    p.hops  = 0;
    p.xid   = dhcp_xid;
    p.secs  = 0;
    p.flags = htons(0x8000); /* Broadcast flag */

    for (int i = 0; i < 6; i++) p.chaddr[i] = g_netif.mac[i];
    p.magic = htonl(DHCP_MAGIC_COOKIE);

    uint8_t *opt = p.options;
    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCP_DISCOVER;

    *opt++ = DHCP_OPT_PARAM_REQ;
    *opt++ = 3;
    *opt++ = DHCP_OPT_SUBNET_MASK;
    *opt++ = DHCP_OPT_ROUTER;
    *opt++ = DHCP_OPT_DNS;

    *opt++ = DHCP_OPT_END;

    udp_send(0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &p, sizeof(p));
}

static void dhcp_send_request(void) {
    struct dhcp_packet p;
    for (size_t i = 0; i < sizeof(p); i++) ((uint8_t *)&p)[i] = 0;

    p.op    = DHCP_OP_REQUEST;
    p.htype = 1;
    p.hlen  = 6;
    p.xid   = dhcp_xid;
    p.flags = htons(0x8000);

    for (int i = 0; i < 6; i++) p.chaddr[i] = g_netif.mac[i];
    p.magic = htonl(DHCP_MAGIC_COOKIE);

    uint8_t *opt = p.options;
    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCP_REQUEST;

    *opt++ = DHCP_OPT_REQUESTED_IP;
    *opt++ = 4;
    *(uint32_t *)opt = offered_ip;
    opt += 4;

    *opt++ = DHCP_OPT_SERVER_ID;
    *opt++ = 4;
    *(uint32_t *)opt = server_ip;
    opt += 4;

    *opt++ = DHCP_OPT_END;

    udp_send(0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &p, sizeof(p));
}

int dhcp_request_ip(uint32_t timeout_ms) {
    serial_puts("[DHCP] Starting DHCP discovery...\n");

    udp_bind_handler(DHCP_CLIENT_PORT, dhcp_udp_callback);
    dhcp_state = 1;

    dhcp_send_discover();

    uint64_t start = timer_get_ticks();
    uint64_t timeout_ticks = (timeout_ms / 10) + 1;

    while (timer_get_ticks() - start < timeout_ticks) {
        net_poll();

        if (dhcp_state == 2) {
            /* Received offer, send request */
            dhcp_send_request();
        } else if (dhcp_state == 3) {
            /* Success! */
            g_netif.ip         = offered_ip;
            g_netif.netmask    = offered_mask ? offered_mask : htonl(0xFFFFFF00);
            g_netif.gateway    = offered_gw;
            g_netif.dns        = offered_dns;
            g_netif.configured = 1;

            udp_unbind_handler(DHCP_CLIENT_PORT);
            serial_puts("[DHCP] Configured IP: ");
            net_print_ip(g_netif.ip);
            serial_puts(" Mask: ");
            net_print_ip(g_netif.netmask);
            serial_puts(" GW: ");
            net_print_ip(g_netif.gateway);
            serial_puts(" DNS: ");
            net_print_ip(g_netif.dns);
            serial_puts("\n");
            return 0;
        }

        __asm__ volatile("pause");
    }

    udp_unbind_handler(DHCP_CLIENT_PORT);
    serial_puts("[DHCP] Discovery timed out.\n");
    return -1;
}
