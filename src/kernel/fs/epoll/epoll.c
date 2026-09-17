/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * epoll.c - epoll instance objects.
 *
 * An instance is just a table of (fd, events, data) plus a node so it can
 * live in the fd table. Waiting is a re-check loop over that table, using
 * the same readiness helpers poll() uses; the loop parks on hlt so an idle
 * compositor costs no CPU, and it still wakes for signals and the network.
 */

#include <stdint.h>
#include <stddef.h>
#include "epoll.h"
#include "dev/chan/chanfd.h"
#include "fs/vfs/vfs.h"
#include "net/socket.h"
#include "net/net.h"
#include "proc/process.h"
#include "mm/kheap.h"
#include "drivers/timer/pit.h"

#define EPOLL_MAX_WATCH 32

struct epoll_watch {
    int      fd;
    uint32_t events;
    uint64_t data;
    int      used;
};

struct epoll_inst {
    struct vfs_node    node;
    struct epoll_watch watch[EPOLL_MAX_WATCH];
    int                nwatch;
};

/* The VFS currently reports every non-special file as ready, which is wrong
 * for chanfds (an unexpired timerfd has nothing to say). Before falling
 * through to vfs_poll, consult a channel's own counter. */
static uint32_t ep_ready(int fd, uint32_t events) {
    int idx = vfs_sock_fd(fd);
    if (idx >= 0) return (uint32_t)sys_sock_poll(idx, (int)events);

    struct file_descriptor *fd_table = current_process ? current_process->fds
                                                       : NULL;
    if (fd_table && fd >= 0 && fd < MAX_FD_COUNT && fd_table[fd].is_used &&
        fd_table[fd].node && chan_is_node(fd_table[fd].node))
        return (uint32_t)chan_poll(fd_table[fd].node, (int)events);

    return (uint32_t)vfs_poll(fd, (int)events);
}

static int64_t ep_read(struct vfs_node *node, uint64_t offset, size_t size,
                       void *buf) {
    (void)node; (void)offset; (void)size; (void)buf;
    return -22; /* -EINVAL: an epoll instance is not readable */
}

static int64_t ep_write(struct vfs_node *node, uint64_t offset, size_t size,
                        const void *buf) {
    (void)node; (void)offset; (void)size; (void)buf;
    return -22; /* -EINVAL */
}

static int ep_close(struct vfs_node *node) {
    /* Same reasoning as shmem: dup/fork mean two fds can name one instance,
     * so freeing here risks a use-after-free. The table is small and the
     * leak is bounded by how many instances a program creates. */
    (void)node;
    return 0;
}

int64_t sys_epoll_create1(int flags) {
    (void)flags; /* EPOLL_CLOEXEC: the fd table has no exec-time sweep */

    struct epoll_inst *e = (struct epoll_inst *)kzalloc(sizeof(*e));
    if (!e) return -12; /* -ENOMEM */

    e->node.type     = VFS_FILE;
    e->node.device   = e;
    e->node.read     = ep_read;
    e->node.write    = ep_write;
    e->node.close    = ep_close;

    int fd = vfs_install_node(&e->node);
    if (fd < 0) {
        kfree(e);
        return fd;
    }
    return fd;
}

int64_t sys_epoll_ctl(int epfd, int op, int fd,
                      const struct epoll_event_k *ev) {
    struct file_descriptor *fd_table = current_process ? current_process->fds
                                                       : NULL;
    /* Resolve the instance through the fd table, so epfd may be an alias. */
    struct vfs_node *node = NULL;
    if (fd_table && epfd >= 0 && epfd < MAX_FD_COUNT &&
        fd_table[epfd].is_used)
        node = fd_table[epfd].node;
    if (!node || node->read != ep_read) return -9; /* -EBADF */

    struct epoll_inst *e = (struct epoll_inst *)node->device;
    if (!e) return -9;

    int slot = -1;
    for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
        if (e->watch[i].used && e->watch[i].fd == fd) { slot = i; break; }
    }

    if (op == EPOLL_CTL_DEL) {
        if (slot < 0) return -2; /* -ENOENT */
        e->watch[slot].used = 0;
        return 0;
    }

    if (op == EPOLL_CTL_MOD) {
        if (slot < 0) return -2; /* -ENOENT */
        if (ev) {
            e->watch[slot].events = ev->events;
            e->watch[slot].data   = ev->data;
        }
        return 0;
    }

    if (op != EPOLL_CTL_ADD) return -22; /* -EINVAL */
    if (slot >= 0) return -17;           /* -EEXIST */
    for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
        if (!e->watch[i].used) {
            e->watch[i].used   = 1;
            e->watch[i].fd     = fd;
            e->watch[i].events = ev ? ev->events : 0;
            e->watch[i].data   = ev ? ev->data : 0;
            e->nwatch++;
            return 0;
        }
    }
    return -28; /* -ENOSPC */
}

int64_t sys_epoll_wait(int epfd, struct epoll_event_k *events,
                       int maxevents, int timeout_ms) {
    if (!events || maxevents <= 0) return -22; /* -EINVAL */

    struct file_descriptor *fd_table = current_process ? current_process->fds
                                                       : NULL;
    struct vfs_node *node = NULL;
    if (fd_table && epfd >= 0 && epfd < MAX_FD_COUNT &&
        fd_table[epfd].is_used)
        node = fd_table[epfd].node;
    if (!node || node->read != ep_read) return -9; /* -EBADF */

    struct epoll_inst *e = (struct epoll_inst *)node->device;
    if (!e) return -9;

    uint64_t start = timer_get_ticks();
    for (;;) {
        int ready = 0;
        for (int i = 0; i < EPOLL_MAX_WATCH && ready < maxevents; i++) {
            if (!e->watch[i].used) continue;
            uint32_t rev = ep_ready(e->watch[i].fd, e->watch[i].events);
            if (!rev) continue;
            events[ready].events = rev;
            events[ready].data   = e->watch[i].data;
            ready++;
        }
        if (ready) return ready;

        if (has_unblocked_pending()) return -4; /* -EINTR */

        if (timeout_ms == 0) return 0;
        if (timeout_ms > 0) {
            if ((int64_t)(timer_get_ticks() - start) >= timeout_ms / 10)
                return 0;
            timer_sleep_ms(1);
        } else {
            /* Infinite: park until an IRQ, but keep the network drained so a
             * socket becoming ready is noticed without a timer tick. */
            net_poll();
            __asm__ volatile("hlt");
        }
    }
}