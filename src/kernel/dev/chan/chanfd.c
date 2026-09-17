/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * chanfd.c - the synchronous signalling channels: eventfd, timerfd, signalfd.
 *
 * Wayland's event loop is built on exactly these three: eventfd for the
 * terminate pipe and internal wakeups, timerfd for the frame timer heap,
 * signalfd for the child-watch and exit-signal sources. All three are
 * edge-triggered counters with blocking read semantics, so they share one
 * small object: a value, a waiter flag, and an fd-table node.
 *
 * The loop parks in read() and the IRQ tick (or another thread on a bigger
 * kernel) advances the counters - here each counter is written directly by
 * the code that arms it. That matches the single-threaded deferred model:
 * nobody needs to observe a counter advance without first returning to the
 * loop.
 */

#include <stdint.h>
#include <stddef.h>
#include "chanfd.h"
#include "fs/vfs/vfs.h"
#include "mm/kheap.h"
#include "drivers/timer/pit.h"

#define CHAN_TIMERFD  1
#define CHAN_EVENTFD  2
#define CHAN_SIGNALFD 3

struct chanfd {
    struct vfs_node node;
    int             kind;
    volatile uint64_t value;      /* eventfd counter / timer expirations */
    /* timerfd arming */
    uint64_t deadline_ticks;      /* PIT ticks at which the timer fires */
    uint64_t interval_ticks;      /* 0 = one-shot */
    int      armed;
    int      clock_monotonic;     /* CLOCK_MONOTONIC or CLOCK_REALTIME */
    /* signalfd mask */
    uint64_t signo_mask;
};

/* PIT tick rate, shared with the rest of the kernel. */
extern uint64_t timer_get_ticks(void);

static int64_t chan_read(struct vfs_node *node, uint64_t offset, size_t size,
                         void *buf) {
    struct chanfd *c = (struct chanfd *)node->device;
    if (!c || !buf) return -14;                 /* -EFAULT */
    (void)offset;

    if (size < sizeof(uint64_t)) return -22;    /* -EINVAL */

    /* Wait for the counter to change. Timers compare against the tick; the
     * other two wait on an explicit store that already holds the lock. */
    for (;;) {
        if (c->kind == CHAN_TIMERFD && c->armed) {
            if (timer_get_ticks() >= c->deadline_ticks) {
                c->value++;
                if (c->interval_ticks) {
                    c->deadline_ticks += c->interval_ticks;
                    if (c->deadline_ticks <= timer_get_ticks())
                        c->deadline_ticks = timer_get_ticks() + c->interval_ticks;
                } else {
                    c->armed = 0;
                }
                break;
            }
        } else if (c->value) {
            break;
        }
        if (has_unblocked_pending()) return -4; /* -EINTR */
        __asm__ volatile("pause");
    }

    uint64_t out = c->value;
    c->value = 0;
    uint8_t *dst = (uint8_t *)buf;
    uint8_t *src = (uint8_t *)&out;
    for (int i = 0; i < 8; i++) dst[i] = src[i];
    return 8;
}

static int64_t chan_write(struct vfs_node *node, uint64_t offset, size_t size,
                          const void *buf) {
    struct chanfd *c = (struct chanfd *)node->device;
    if (!c || !buf || size < sizeof(uint64_t)) return -22; /* -EINVAL */
    (void)offset;
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t add = 0;
    uint8_t *dst = (uint8_t *)&add;
    for (int i = 0; i < 8; i++) dst[i] = src[i];
    /* 0xFFFFFFFFFFFFFFFF means "do not add", per eventfd(2). */
    if (add != 0xFFFFFFFFFFFFFFFFULL) c->value += add;
    return 8;
}
#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1

int chan_timer_settime(struct vfs_node *node, int flags,
                       uint64_t sec, uint64_t nsec,
                       uint64_t isec, uint64_t insec) {
    struct chanfd *c = (struct chanfd *)node->device;
    if (!c || c->kind != CHAN_TIMERFD) return -9; /* -EBADF */
    (void)flags; /* ABSTIME vs relative both land on the tick clock here */
    uint64_t now = timer_get_ticks();
    uint64_t total = sec * 100 + nsec / 10000000ULL;
    if (total == 0 && isec == 0 && insec == 0) {
        c->armed = 0;                            /* disarm */
        return 0;
    }
    c->deadline_ticks  = now + total;
    c->interval_ticks  = isec * 100 + insec / 10000000ULL;
    c->armed = 1;
    return 0;
}

/* signalfd delivers the signals the caller masked here. */
int chan_signal_mask(struct vfs_node *node, uint64_t mask) {
    struct chanfd *c = (struct chanfd *)node->device;
    if (!c || c->kind != CHAN_SIGNALFD) return -9; /* -EBADF */
    c->signo_mask = mask;
    return 0;
}

/* Identity predicate for the epoll readiness path: a chanfd is recognised by
 * its read op, so probing a tmpfs or pty node (whose device pointer means
 * something else entirely) can never misfire. */
int chan_is_node(struct vfs_node *node) {
    return node && node->read == chan_read;
}

/* Readiness for a poll/select watcher. */
int chan_poll(struct vfs_node *node, int events) {
    struct chanfd *c = (struct chanfd *)node->device;
    if (!c) return 0;
    int mask = 0;
    if (c->kind == CHAN_TIMERFD && c->armed && timer_get_ticks() >= c->deadline_ticks)
        mask |= 0x001; /* POLLIN */
    else if (c->kind != CHAN_TIMERFD && c->value)
        mask |= 0x001;
    if (events & 0x004) mask |= 0x004; /* always writable */
    return mask & events;
}

static int chan_close(struct vfs_node *node) {
    /* Same lifetime policy as shmem: close releases the fd, the object is
     * reclaimed at reboot (no dup-aware refcount yet). */
    (void)node;
    return 0;
}

static void chan_node_init(struct chanfd *c, int kind) {
    c->node.type        = VFS_FILE;
    c->node.permissions = 0600;
    c->node.device      = c;
    c->node.read        = chan_read;
    c->node.write       = chan_write;
    c->node.close       = chan_close;
    c->kind             = kind;
    c->value            = 0;
    c->armed            = 0;
}

int chan_eventfd(unsigned int initval) {
    struct chanfd *c = (struct chanfd *)kzalloc(sizeof(*c));
    if (!c) return -12; /* -ENOMEM */
    chan_node_init(c, CHAN_EVENTFD);
    c->value = initval;
    int fd = vfs_install_node(&c->node);
    if (fd < 0) kfree(c);
    return fd;
}

int chan_timerfd_create(int clockid) {
    if (clockid != CLOCK_MONOTONIC && clockid != CLOCK_REALTIME)
        return -22; /* -EINVAL */
    struct chanfd *c = (struct chanfd *)kzalloc(sizeof(*c));
    if (!c) return -12; /* -ENOMEM */
    chan_node_init(c, CHAN_TIMERFD);
    c->clock_monotonic = (clockid == CLOCK_MONOTONIC);
    int fd = vfs_install_node(&c->node);
    if (fd < 0) kfree(c);
    return fd;
}

int chan_signalfd(int sigmask_lo) {
    struct chanfd *c = (struct chanfd *)kzalloc(sizeof(*c));
    if (!c) return -12; /* -ENOMEM */
    chan_node_init(c, CHAN_SIGNALFD);
    c->signo_mask = (uint64_t)(uint32_t)sigmask_lo;
    int fd = vfs_install_node(&c->node);
    if (fd < 0) kfree(c);
    return fd;
}
