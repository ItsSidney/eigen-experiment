/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * gdt.h - Global Descriptor Table definitions.
 */

#ifndef KERNEL_GDT_H
#define KERNEL_GDT_H

#include <stdint.h>

/* Segment selectors (index * 8) */
#define GDT_SEG_NULL     0x00
#define GDT_SEG_KCODE    0x08
#define GDT_SEG_KDATA    0x10
#define GDT_SEG_UDATA    0x18
#define GDT_SEG_UCODE    0x20
#define GDT_SEG_TSS      0x28

void gdt_init(void);

#endif /* KERNEL_GDT_H */
