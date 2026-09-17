/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * chanfd.h - eventfd(2), timerfd(2) and signalfd(2) for the event loop.
 */

#ifndef KERNEL_DEV_CHAN_H
#define KERNEL_DEV_CHAN_H

#include <stdint.h>

struct vfs_node;

/* Create an event counter fd with an initial value. */
int chan_eventfd(unsigned int initval);

/* Create a timerfd on CLOCK_REALTIME or CLOCK_MONOTONIC. */
int chan_timerfd_create(int clockid);

/* Arm or disarm a timerfd. Times are seconds + nanoseconds, interval of
 * zero means one-shot; all-zero means disarm. */
int chan_timer_settime(struct vfs_node *node, int flags,
                       uint64_t sec, uint64_t nsec,
                       uint64_t isec, uint64_t insec);

/* Create a signalfd watching the given 32-bit signal mask. */
int chan_signalfd(int sigmask_lo);

/* Restrict a signalfd's mask after creation. */
int chan_signal_mask(struct vfs_node *node, uint64_t mask);

/* Readiness probe used by the poll/select path. */
int chan_poll(struct vfs_node *node, int events);

/* True when the node is one of ours (recognised by its read op). */
int chan_is_node(struct vfs_node *node);

#endif /* KERNEL_DEV_CHAN_H */
