/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * tcp.h - Transmission Control Protocol (TCP) connection engine and state machine.
 */

#ifndef KERNEL_TCP_H
#define KERNEL_TCP_H

#include <stdint.h>
#include <stddef.h>

#define TCP_FLAG_FIN  (1 << 0)
#define TCP_FLAG_SYN  (1 << 1)
#define TCP_FLAG_RST  (1 << 2)
#define TCP_FLAG_PSH  (1 << 3)
#define TCP_FLAG_ACK  (1 << 4)
#define TCP_FLAG_URG  (1 << 5)

enum tcp_state {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RCVD,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_TIME_WAIT
};

struct tcp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset; /* (offset >> 4) = 4-byte dwords */
    uint8_t  flags;
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed));

#define TCP_RX_BUF_SIZE 16384

struct tcp_conn {
    uint32_t       remote_ip;
    uint16_t       remote_port;
    uint16_t       local_port;
    enum tcp_state state;

    uint32_t       snd_una;
    uint32_t       snd_nxt;
    uint32_t       rcv_nxt;

    /* Circular RX ring buffer */
    uint8_t        rx_buf[TCP_RX_BUF_SIZE];
    uint32_t       rx_head;
    uint32_t       rx_tail;

    int            active;
};

#define MAX_TCP_CONNS 16

void             tcp_init(void);
struct tcp_conn *tcp_alloc_conn(void);
void             tcp_free_conn(struct tcp_conn *conn);
int              tcp_connect(struct tcp_conn *conn, uint32_t dst_ip, uint16_t dst_port, uint32_t timeout_ms);
int              tcp_send(struct tcp_conn *conn, const void *data, uint16_t len);
int              tcp_recv(struct tcp_conn *conn, void *buf, uint16_t max_len, int nonblock);
int              tcp_close(struct tcp_conn *conn);
void             tcp_handle_packet(uint32_t src_ip, const uint8_t *payload, uint16_t len);

#endif /* KERNEL_TCP_H */
