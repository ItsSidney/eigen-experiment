/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * mouse.h - PS/2 auxiliary port: IMPS/2 wheel mouse on IRQ12.
 */

#ifndef KERNEL_DRIVERS_CHAR_MOUSE_H
#define KERNEL_DRIVERS_CHAR_MOUSE_H

void mouse_init(void);
/* Userspace-visible cursor for a compositor-less test console. */
void mouse_cursor_pos(int *x, int *y);
int  mouse_buttons(void);
/* True once the aux-port handshake completed and IRQ12 is ours. The keyboard
 * handler uses this to decide whether an AUX byte must be dropped (a stale
 * one would otherwise block every keystroke forever). */
int  mouse_active(void);

#endif /* KERNEL_DRIVERS_CHAR_MOUSE_H */
