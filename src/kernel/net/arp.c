/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * arp.c - Address Resolution Protocol (ARP) cache and packet processing.
 */

#include "arp.h"
#include "net.h"
#include "include/serial.h"
#include "drivers/timer/pit.h"

#define ARP_CACHE_SIZE 32

struct arp_entry {
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;
};

static struct arp_entry arp_cache[ARP_CACHE_SIZE];

void arp_init(void) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        arp_cache[i].valid = 0;
    }
}

void arp_cache_insert(uint32_t ip, const uint8_t mac[6]) {
    /* Update existing */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            for (int k = 0; k < 6; k++) arp_cache[i].mac[k] = mac[k];
            return;
        }
    }
    /* Find free slot */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            for (int k = 0; k < 6; k++) arp_cache[i].mac[k] = mac[k];
            arp_cache[i].valid = 1;
            return;
        }
    }
    /* Overwrite first slot */
    arp_cache[0].ip = ip;
    for (int k = 0; k < 6; k++) arp_cache[0].mac[k] = mac[k];
    arp_cache[0].valid = 1;
}

int arp_resolve(uint32_t ip, uint8_t mac_out[6]) {
    /* Broadcast address */
    if (ip == 0xFFFFFFFF || ip == (g_netif.ip | ~g_netif.netmask)) {
        for (int k = 0; k < 6; k++) mac_out[k] = 0xFF;
        return 0;
    }

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            for (int k = 0; k < 6; k++) mac_out[k] = arp_cache[i].mac[k];
            return 0;
        }
    }

    /* Not found in cache; send request and poll briefly for reply */
    arp_send_request(ip);

    uint64_t start = timer_get_ticks();
    while (timer_get_ticks() - start < 50) { /* 500ms */
        net_poll();
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (arp_cache[i].valid && arp_cache[i].ip == ip) {
                for (int k = 0; k < 6; k++) mac_out[k] = arp_cache[i].mac[k];
                return 0;
            }
        }
        __asm__ volatile("pause");
    }

    return -1;
}

void arp_send_request(uint32_t ip) {
    struct arp_header arp;
    arp.htype = htons(1);           /* Ethernet */
    arp.ptype = htons(ETHERTYPE_IPV4);
    arp.hlen  = 6;
    arp.plen  = 4;
    arp.opcode = htons(ARP_OP_REQUEST);

    for (int i = 0; i < 6; i++) arp.sender_mac[i] = g_netif.mac[i];
    arp.sender_ip = g_netif.ip;

    for (int i = 0; i < 6; i++) arp.target_mac[i] = 0x00;
    arp.target_ip = ip;

    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    net_send_ethernet(bcast, ETHERTYPE_ARP, &arp, sizeof(arp));
}

void arp_handle_packet(const uint8_t *payload, uint16_t len) {
    if (len < sizeof(struct arp_header)) return;

    const struct arp_header *arp = (const struct arp_header *)payload;
    if (ntohs(arp->htype) != 1 || ntohs(arp->ptype) != ETHERTYPE_IPV4) return;

    uint16_t op = ntohs(arp->opcode);

    /* Update cache with sender info */
    arp_cache_insert(arp->sender_ip, arp->sender_mac);

    if (op == ARP_OP_REQUEST && arp->target_ip == g_netif.ip && g_netif.ip != 0) {
        /* Send ARP reply */
        struct arp_header reply;
        reply.htype = htons(1);
        reply.ptype = htons(ETHERTYPE_IPV4);
        reply.hlen  = 6;
        reply.plen  = 4;
        reply.opcode = htons(ARP_OP_REPLY);

        for (int i = 0; i < 6; i++) reply.sender_mac[i] = g_netif.mac[i];
        reply.sender_ip = g_netif.ip;

        for (int i = 0; i < 6; i++) reply.target_mac[i] = arp->sender_mac[i];
        reply.target_ip = arp->sender_ip;

        net_send_ethernet(arp->sender_mac, ETHERTYPE_ARP, &reply, sizeof(reply));
    }
}
