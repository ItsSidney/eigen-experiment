/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * kheap.h - Dynamic kernel heap memory allocator (kmalloc/kzalloc/kfree).
 */

#ifndef KERNEL_KHEAP_H
#define KERNEL_KHEAP_H

#include <stdint.h>
#include <stddef.h>

void kheap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *ptr);

#endif /* KERNEL_KHEAP_H */
