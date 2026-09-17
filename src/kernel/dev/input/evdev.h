/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * evdev.h - /dev/input/eventX: Linux evdev protocol, PS/2 sources.
 */

#ifndef KERNEL_DEV_INPUT_EVDEV_H
#define KERNEL_DEV_INPUT_EVDEV_H

#include <stdint.h>
#include <stddef.h>

/* VFS hooks take the opaque node; the full type comes from vfs.h. */
struct vfs_node;

void evdev_init(void);

/* PS/2 sources call these with decoded motion/button state. Safe to call
 * from an IRQ: the ring is event-slot atomic by construction. */
void evdev_post_rel(int dx, int dy, int wheel);
void evdev_post_key(uint16_t code, int32_t value);
void evdev_post_btn(uint16_t code, int32_t value);
void evdev_post_abs(int slot, int value);
void evdev_post_syn(void);

/* Poll layer: nonzero while unread events sit in any device queue. */
int  evdev_any_ready(void);
/* Per-device readiness for poll/select on an event node. */
int  evdev_poll(struct vfs_node *node, int events);

/* VFS hooks for the device nodes. */
struct vfs_node;
int64_t evdev_read(struct vfs_node *node, uint64_t offset, size_t size,
                   void *buf);

#endif /* KERNEL_DEV_INPUT_EVDEV_H */
