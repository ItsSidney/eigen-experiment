/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * tcp.c - Transmission Control Protocol (TCP) connection state machine.
 */

#include "tcp.h"
#include "ipv4.h"
#include "net.h"
#include "proc/process.h"
#include "drivers/char/keyboard.h"
#include "include/serial.h"
#include "drivers/timer/pit.h"

static struct tcp_conn tcp_conns[MAX_TCP_CONNS];
static uint16_t tcp_ephemeral_port = 49152;

struct tcp_pseudo_header {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t tcp_len;
} __attribute__((packed));

void tcp_init(void) {
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        tcp_conns[i].active = 0;
        tcp_conns[i].state  = TCP_STATE_CLOSED;
    }
}

/* Free RX ring space, capped at 8K of advertised window. */
static uint16_t tcp_rx_window(struct tcp_conn *conn) {
    uint32_t used = (conn->rx_head >= conn->rx_tail)
        ? (uint32_t)(conn->rx_head - conn->rx_tail)
        : (uint32_t)(TCP_RX_BUF_SIZE - conn->rx_tail + conn->rx_head);
    uint32_t free = (TCP_RX_BUF_SIZE - 1) - used;
    if (free > 8192) free = 8192;
    return (uint16_t)free;
}

struct tcp_conn *tcp_alloc_conn(void) {
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (!tcp_conns[i].active) {
            tcp_conns[i].active      = 1;
            tcp_conns[i].state       = TCP_STATE_CLOSED;
            tcp_conns[i].local_port  = tcp_ephemeral_port++;
            if (tcp_ephemeral_port > 65000) tcp_ephemeral_port = 49152;
            tcp_conns[i].snd_nxt     = 0x1000 + i * 0x10000;
            tcp_conns[i].snd_una     = tcp_conns[i].snd_nxt;
            tcp_conns[i].rcv_nxt     = 0;
            tcp_conns[i].rx_head     = 0;
            tcp_conns[i].rx_tail     = 0;
            return &tcp_conns[i];
        }
    }
    return NULL;
}

void tcp_free_conn(struct tcp_conn *conn) {
    if (conn) {
        conn->active = 0;
        conn->state  = TCP_STATE_CLOSED;
    }
}

static uint16_t tcp_calc_checksum(uint32_t src_ip, uint32_t dst_ip, const void *tcp_seg, uint16_t seg_len) {
    struct tcp_pseudo_header ph;
    ph.src_ip   = src_ip;
    ph.dst_ip   = dst_ip;
    ph.zero     = 0;
    ph.protocol = IP_PROTO_TCP;
    ph.tcp_len  = htons(seg_len);

    uint32_t sum = 0;
    const uint16_t *p = (const uint16_t *)&ph;
    for (size_t i = 0; i < sizeof(ph) / 2; i++) sum += ntohs(p[i]);

    p = (const uint16_t *)tcp_seg;
    for (uint16_t i = 0; i < seg_len / 2; i++) sum += ntohs(p[i]);
    if (seg_len & 1) sum += (uint16_t)(((const uint8_t *)tcp_seg)[seg_len - 1] << 8);

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

static int tcp_send_segment(struct tcp_conn *conn, uint8_t flags, const void *data, uint16_t len) {
    uint8_t buffer[ETH_MTU];    struct tcp_header *tcp = (struct tcp_header *)buffer;

    tcp->src_port    = htons(conn->local_port);
    tcp->dst_port    = htons(conn->remote_port);
    tcp->seq_num     = htonl(conn->snd_nxt);
    tcp->ack_num     = htonl(conn->rcv_nxt);
    tcp->data_offset = (sizeof(struct tcp_header) / 4) << 4;
    tcp->flags       = flags;
    tcp->window_size = htons(tcp_rx_window(conn));
    tcp->checksum    = 0;
    tcp->urgent_ptr  = 0;

    if (len > 0 && data) {
        const uint8_t *s = (const uint8_t *)data;
        uint8_t *d = buffer + sizeof(struct tcp_header);
        for (uint16_t i = 0; i < len; i++) d[i] = s[i];
    }

    uint16_t total = sizeof(struct tcp_header) + len;
    tcp->checksum = tcp_calc_checksum(g_netif.ip, conn->remote_ip, tcp, total);

    return ipv4_send(conn->remote_ip, IP_PROTO_TCP, buffer, total);
}

int tcp_connect(struct tcp_conn *conn, uint32_t dst_ip, uint16_t dst_port, uint32_t timeout_ms) {
    if (!conn) return -1;

    conn->remote_ip   = dst_ip;
    conn->remote_port = dst_port;
    conn->state       = TCP_STATE_SYN_SENT;

    /* Send SYN */
    tcp_send_segment(conn, TCP_FLAG_SYN, NULL, 0);
    conn->snd_nxt++;

    uint64_t start = timer_get_ticks();
    uint64_t timeout_ticks = (timeout_ms / 10) + 1;

    while (timer_get_ticks() - start < timeout_ticks) {
        net_poll();
        tty_poll_signal();
        if (conn->state == TCP_STATE_ESTABLISHED) {
            return 0; /* Connected! */
        }
        if (conn->state == TCP_STATE_CLOSED) {
            return -1; /* Connection refused / RST */
        }
        if (current_process &&
            (current_process->sig_pending & ~current_process->sig_blocked)) {
            return -4; /* -EINTR */
        }
        __asm__ volatile("pause");
    }

    conn->state = TCP_STATE_CLOSED;
    return -1; /* Timeout */
}

int tcp_send(struct tcp_conn *conn, const void *data, uint16_t len) {
    if (!conn || conn->state != TCP_STATE_ESTABLISHED) return -1;

    int ret = tcp_send_segment(conn, TCP_FLAG_ACK | TCP_FLAG_PSH, data, len);
    if (ret == 0) {
        conn->snd_nxt += len;
    }
    return ret == 0 ? (int)len : -1;
}

int tcp_recv(struct tcp_conn *conn, void *buf, uint16_t max_len, int nonblock) {
    if (!conn) return -1;

    for (;;) {
        net_poll();
        tty_poll_signal();

        if (conn->rx_head != conn->rx_tail) {
            /* Copy available bytes */
            uint16_t copied = 0;
            uint8_t *dst = (uint8_t *)buf;
            while (conn->rx_tail != conn->rx_head && copied < max_len) {
                dst[copied++] = conn->rx_buf[conn->rx_tail];
                conn->rx_tail = (conn->rx_tail + 1) % TCP_RX_BUF_SIZE;
            }
            /* Window update: freed space may unblock the sender */
            if (conn->state == TCP_STATE_ESTABLISHED) {
                tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            }
            return (int)copied;
        }

        if (conn->state != TCP_STATE_ESTABLISHED) {
            return 0; /* EOF / Closed */
        }

        if (nonblock) return -11; /* -EAGAIN */

        if (current_process &&
            (current_process->sig_pending & ~current_process->sig_blocked)) {
            return -4; /* -EINTR */
        }

        __asm__ volatile("pause");
    }
}

int tcp_close(struct tcp_conn *conn) {
    if (!conn || conn->state == TCP_STATE_CLOSED) return 0;

    if (conn->state == TCP_STATE_ESTABLISHED) {
        tcp_send_segment(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        conn->snd_nxt++;
        conn->state = TCP_STATE_FIN_WAIT_1;
    }

    uint64_t start = timer_get_ticks();
    while (timer_get_ticks() - start < 100) { /* 1 sec max */
        net_poll();
        if (conn->state == TCP_STATE_CLOSED) break;
        __asm__ volatile("pause");
    }

    conn->state = TCP_STATE_CLOSED;
    return 0;
}

void tcp_handle_packet(uint32_t src_ip, const uint8_t *payload, uint16_t len) {
    if (len < sizeof(struct tcp_header)) return;

    const struct tcp_header *tcp = (const struct tcp_header *)payload;
    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint32_t seq      = ntohl(tcp->seq_num);
    uint32_t ack      = ntohl(tcp->ack_num);
    uint8_t  flags    = tcp->flags;

    uint8_t  hdr_len  = (tcp->data_offset >> 4) * 4;
    if (hdr_len > len) return;

    const uint8_t *data = payload + hdr_len;
    uint16_t data_len   = len - hdr_len;

    /* Find matching connection */
    struct tcp_conn *conn = NULL;
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_conns[i].active &&
            tcp_conns[i].local_port == dst_port &&
            (tcp_conns[i].remote_port == src_port || tcp_conns[i].remote_port == 0) &&
            (tcp_conns[i].remote_ip == src_ip || tcp_conns[i].remote_ip == 0)) {
            conn = &tcp_conns[i];
            break;
        }
    }

    if (!conn) return;

    /* Handle RST */
    if (flags & TCP_FLAG_RST) {
        conn->state = TCP_STATE_CLOSED;
        return;
    }

    switch (conn->state) {
        case TCP_STATE_SYN_SENT:
            if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                conn->rcv_nxt = seq + 1;
                conn->snd_una = ack;
                conn->state   = TCP_STATE_ESTABLISHED;
                /* Send ACK */
                tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_STATE_ESTABLISHED:
            if (data_len > 0) {
                if (seq != conn->rcv_nxt) {
                    /* Out-of-order segment (after drops): re-announce our
                     * position so the sender retransmits the gap. */
                    tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
                    break;
                }
                /* Push data to RX buffer, stopping when the ring is full */
                uint16_t accepted = 0;
                for (uint16_t b = 0; b < data_len; b++) {
                    uint32_t next = (conn->rx_head + 1) % TCP_RX_BUF_SIZE;
                    if (next == conn->rx_tail) break;
                    conn->rx_buf[conn->rx_head] = data[b];
                    conn->rx_head = next;
                    accepted++;
                }
                conn->rcv_nxt += accepted;
                /* ACK only what we stored; the sender retransmits the rest */
                tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            }

            if (flags & TCP_FLAG_FIN) {
                if (seq + data_len == conn->rcv_nxt) {
                    conn->rcv_nxt++;
                    conn->state = TCP_STATE_CLOSE_WAIT;
                }
                tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_STATE_FIN_WAIT_1:
            if (flags & TCP_FLAG_ACK) {
                conn->state = TCP_STATE_FIN_WAIT_2;
            }
            if (flags & TCP_FLAG_FIN) {
                conn->rcv_nxt++;
                tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
                conn->state = TCP_STATE_CLOSED;
            }
            break;

        case TCP_STATE_FIN_WAIT_2:
            if (flags & TCP_FLAG_FIN) {
                conn->rcv_nxt++;
                tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
                conn->state = TCP_STATE_CLOSED;
            }
            break;

        default:
            break;
    }
}
