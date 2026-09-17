/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * keyboard.h - PS/2 Keyboard Driver definitions.
 */

#ifndef KERNEL_KEYBOARD_H
#define KERNEL_KEYBOARD_H

#include <stdint.h>

void keyboard_init(void);
char keyboard_getchar(void);
int  keyboard_has_char(void);
int  keyboard_trygetchar(void);
void keyboard_diag(void);   /* print i8042 status to serial + console */
/* Drain one pending input byte if present (buffering it, raising ISIG
 * signals); lets blocking non-tty waits honor Ctrl-C/Ctrl-\. */
void tty_poll_signal(void);

#endif /* KERNEL_KEYBOARD_H */
