/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * icmp.c - ICMP Echo processing and ping utility.
 */

#include "icmp.h"
#include "ipv4.h"
#include "net.h"
#include "net/socket.h"
#include "include/serial.h"
#include "drivers/timer/pit.h"

static volatile int last_reply_received = 0;
static volatile uint16_t last_reply_seq = 0;

void icmp_init(void) {
    last_reply_received = 0;
}

int icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq, const void *data, uint16_t len) {
    uint8_t buffer[ETH_MTU];
    struct icmp_header *icmp = (struct icmp_header *)buffer;

    icmp->type     = ICMP_TYPE_ECHO_REQUEST;
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = htons(id);
    icmp->sequence = htons(seq);

    const uint8_t *payload = (const uint8_t *)data;
    uint8_t *dst = buffer + sizeof(struct icmp_header);
    for (uint16_t i = 0; i < len; i++) dst[i] = payload[i];

    uint16_t total = sizeof(struct icmp_header) + len;
    icmp->checksum = net_checksum(icmp, total);

    return ipv4_send(dst_ip, IP_PROTO_ICMP, buffer, total);
}

void icmp_handle_packet(uint32_t src_ip, const uint8_t *payload, uint16_t len) {
    if (len < sizeof(struct icmp_header)) return;

    const struct icmp_header *icmp = (const struct icmp_header *)payload;

    if (icmp->type == ICMP_TYPE_ECHO_REQUEST) {
        /* Reply to ping */
        uint8_t reply[ETH_MTU];
        for (uint16_t i = 0; i < len && i < ETH_MTU; i++) reply[i] = payload[i];

        struct icmp_header *r = (struct icmp_header *)reply;
        r->type     = ICMP_TYPE_ECHO_REPLY;
        r->code     = 0;
        r->checksum = 0;
        r->checksum = net_checksum(r, len);

        ipv4_send(src_ip, IP_PROTO_ICMP, reply, len);
    } else if (icmp->type == ICMP_TYPE_ECHO_REPLY) {
        last_reply_received = 1;
        last_reply_seq = ntohs(icmp->sequence);
        /* Deliver to any raw ICMP sockets (for userland ping) */
        icmp_deliver_to_raw(src_ip, payload, len);
    }
}

int icmp_ping(uint32_t dst_ip, uint32_t timeout_ms) {
    last_reply_received = 0;

    /* Standard Linux ping payload: 56 data bytes (64-byte ICMP packet, 84-byte IP frame) */
    static const char ping_data[] = "12345678901234567890123456789012345678901234567890123456";
    int sent = -1;
    for (int retry = 0; retry < 5; retry++) {
        if (icmp_send_echo(dst_ip, 0x1234, 1, ping_data, 56) == 0) {
            sent = 0;
            break;
        }
        net_poll();
        timer_sleep_ms(50);
    }
    if (sent != 0) return -1;

    uint64_t start = timer_get_ticks();
    uint64_t timeout_ticks = (timeout_ms / 10) + 1;

    while (timer_get_ticks() - start < timeout_ticks) {
        net_poll();
        if (last_reply_received) {
            return 0; /* Ping success! */
        }
        __asm__ volatile("pause");
    }

    return -1; /* Timed out */
}
