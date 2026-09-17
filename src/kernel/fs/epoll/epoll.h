/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * epoll.h - epoll event loop (create1/ctl/wait).
 *
 * Wayland's compositor and clients both park in epoll_wait, so this is a
 * prerequisite for the protocol stack. The implementation reuses the
 * existing readiness helpers: every registered fd is asked through the same
 * vfs_poll/socket path that poll(2) already uses, so "epoll says readable"
 * and "read would not block" can never disagree.
 */

#ifndef KERNEL_FS_EPOLL_H
#define KERNEL_FS_EPOLL_H

#include <stdint.h>

#define EPOLLIN   0x001u
#define EPOLLOUT  0x004u
#define EPOLLERR  0x008u
#define EPOLLHUP  0x010u

/* Event slot as Linux lays it out on x86_64: packed, 12 bytes. */
struct epoll_event_k {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

/* epoll_ctl operations */
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

int64_t sys_epoll_create1(int flags);
int64_t sys_epoll_ctl(int epfd, int op, int fd,
                      const struct epoll_event_k *ev);
int64_t sys_epoll_wait(int epfd, struct epoll_event_k *events,
                       int maxevents, int timeout_ms);

#endif /* KERNEL_FS_EPOLL_H */
