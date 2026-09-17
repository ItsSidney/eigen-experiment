/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * console.h - Framebuffer text console with scrolling and standard ASCII rendering.
 */

#ifndef KERNEL_CONSOLE_H
#define KERNEL_CONSOLE_H

#include <stdint.h>
#include "include/limine.h"

void console_init(struct limine_framebuffer *fb, uint32_t fg_color, uint32_t bg_color);
void console_putc(char c);
void console_puts(const char *str);
void console_clear(void);
void console_get_size(uint16_t *rows, uint16_t *cols);

/* ── Cell-level API (used by the libvterm glue and full-screen TUIs) ────── */
uint32_t console_cols(void);
uint32_t console_rows(void);
uint8_t  console_color_index(int n);
uint8_t  console_color_rgb(uint32_t r, uint32_t g, uint32_t b);
void     console_put_cell(uint32_t x, uint32_t y, uint8_t ch,
                          uint8_t fg, uint8_t bg, int reverse);
void     console_set_cursor_pos(uint32_t x, uint32_t y);
void     console_set_cursor_visible(int on);
void     console_cursor_repaint(void);
void     console_repaint(void);

#endif /* KERNEL_CONSOLE_H */
