/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * pmm.h - Physical Memory Manager (Bitmap Page Allocator).
 */

#ifndef KERNEL_PMM_H
#define KERNEL_PMM_H

#include <stdint.h>
#include <stddef.h>
#include "include/limine.h"

#define PAGE_SIZE 4096

void pmm_init(struct limine_memmap_response *mm, uint64_t hhdm_offset);
void *pmm_alloc_page(void);
void *pmm_alloc_pages(size_t count);
void pmm_free_page(void *paddr);
void pmm_free_pages(void *paddr, size_t count);
uint64_t pmm_get_free_pages(void);
uint64_t pmm_get_total_pages(void);

uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_free_memory(void);

#endif /* KERNEL_PMM_H */
