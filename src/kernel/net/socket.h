/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * socket.h - BSD Socket API layer and kernel socket descriptor table.
 */

#ifndef KERNEL_SOCKET_H
#define KERNEL_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include "tcp.h"

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
} __attribute__((packed));

struct sockaddr_un {
    uint16_t sun_family;
    char     sun_path[108];
} __attribute__((packed));

#define UNIX_PATH_MAX    108
#define UNIX_BACKLOG_MAX 8

/* AF_UNIX address: filesystem path or abstract name (leading NUL). */
struct unix_addr {
    char path[UNIX_PATH_MAX];
    int  len;
    int  abstract;
};

struct socket_entry {
    int              active;
    int              refcount;
    int              domain;
    int              type;
    int              protocol;
    uint32_t         owner_pid;   /* creator; a dead owner makes this stale */
    struct tcp_conn *tcp;
    uint16_t         local_port;
    uint32_t         remote_ip;
    uint16_t         remote_port;

    /* UDP reception queue */
    uint8_t          udp_rx_buf[2048];
    uint16_t         udp_rx_len;
    uint32_t         udp_src_ip;
    uint16_t         udp_src_port;
    volatile int     udp_has_data;

    /* ICMP raw socket queue (for ping) */
    uint8_t          icmp_rx_buf[2048];
    uint16_t         icmp_rx_len;
    uint32_t         icmp_src_ip;
    volatile int     icmp_has_data;
    int              is_icmp;

    /* AF_UNIX socketpair endpoint (bidirectional byte stream) */
    int              is_unix;
    int              unix_peer_idx; /* index into sockets[], -1 if none */
    uint8_t         *unix_rx_buf;
    uint16_t         unix_rx_head;
    uint16_t         unix_rx_tail;
    uint16_t         unix_rx_count;

    /* AF_UNIX bound/listening state (filesystem + abstract names) */
    int              unix_bound;
    int              unix_listening;
    struct unix_addr unix_name;   /* bound name (listener or explicit bind) */
    int              unix_backlog_q[UNIX_BACKLOG_MAX];
    int              unix_backlog_n;
    struct unix_addr unix_conn;   /* server name (connect side, getpeername) */
    struct unix_addr unix_peer;   /* client name (accept side, getpeername) */
    struct unix_addr unix_dgsrc;  /* last datagram sender */
};

#define UNIX_SOCK_BUF 8192

#define MAX_KERNEL_SOCKETS 32
#define SOCKET_FD_BASE     64

void socket_init(void);
int  sys_sock_socket(int domain, int type, int protocol);
int  sys_sock_socketpair(int domain, int type, int protocol, int sv[2]);
int  sys_sock_bind(int fd, const struct sockaddr_un *addr, uint32_t addrlen);
int  sys_sock_listen(int fd, int backlog);
int  sys_sock_accept(int fd, struct sockaddr_un *addr, uint32_t *addrlen);
int  sys_sock_shutdown(int fd, int how);
int  sys_sock_getsockname(int fd, struct sockaddr_un *addr, uint32_t *addrlen);
int  sys_sock_getpeername(int fd, struct sockaddr_un *addr, uint32_t *addrlen);
int  sock_get_type(int fd);int  sys_sock_connect(int fd, const struct sockaddr_in *addr, uint32_t addrlen);
int  sys_sock_send(int fd, const void *buf, size_t len, int flags);
int  sys_sock_recv(int fd, void *buf, size_t len, int flags);
int  sys_sock_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr_in *dest_addr, uint32_t addrlen);
int  sys_sock_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr_in *src_addr, uint32_t *addrlen);
int  sys_sock_close(int fd);
void sys_sock_incref(int idx);
int  sys_sock_poll(int fd, int events);
int  is_socket_fd(int fd);
void icmp_deliver_to_raw(uint32_t src_ip, const void *data, uint16_t len);

#endif /* KERNEL_SOCKET_H */
