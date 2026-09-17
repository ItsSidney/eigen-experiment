/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * fb.h - /dev/fb0 linear framebuffer device (fbdev contract, no modeset).
 */

#ifndef KERNEL_DEV_FB_H
#define KERNEL_DEV_FB_H

#include <stdint.h>
#include "include/limine.h"

void fb_init(struct limine_framebuffer *fb);
int  fb_ioctl(unsigned long cmd, uint64_t arg);

/* Re-anchor an already-registered fb0 on a newly-activated display. Used when
 * the probe chain claims its framebuffer after fb_init() already published
 * the bootloader one: the node and its /dev entry survive, only the source
 * of geometry/ioctl answers changes. */
void fb_repoint(struct limine_framebuffer *fb);

#endif /* KERNEL_DEV_FB_H */
