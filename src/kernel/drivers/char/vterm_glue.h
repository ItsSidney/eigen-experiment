/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * vterm_glue.h - libvterm driven terminal emulation for the framebuffer
 * console. libvterm owns the screen model and the escape-sequence parser;
 * the console supplies pixels, glyphs and the palette.
 */
#ifndef KERNEL_VTERM_GLUE_H
#define KERNEL_VTERM_GLUE_H

#include <stdint.h>
#include <stddef.h>

/* Create the VTerm instance, sized to the current console geometry.
 * Safe to call more than once (subsequent calls are no-ops). */
void vterm_console_init(void);

/* Feed raw terminal output (what a userspace program wrote to the tty).
 * Creates the instance on first use. */
void vterm_console_write(const char *buf, size_t len);

/* Wipe the screen and reset the emulator to power-on state. */
void vterm_console_reset(void);

/* Resize the emulator to the console's current rows/cols. */
void vterm_console_resize(void);

/* True once the emulator is initialised and usable. */
int vterm_console_ready(void);

/* ── keyboard → host path ────────────────────────────────────────────────
 * The i8042 driver decodes scancodes into bytes; libvterm only understands
 * key *events*. Feed each decoded byte here: plain text is forwarded as a
 * Unicode event, ESC-led arrow/function sequences are looked up so the tty
 * always receives the cursor-mode-correct spelling. The tty callback is set
 * once at boot; vterm_keyboard_feed() before that is dropped. */
void vterm_keyboard_feed(char c);
void vterm_keyboard_code(uint32_t ucs, uint32_t mod);

#endif /* KERNEL_VTERM_GLUE_H */
