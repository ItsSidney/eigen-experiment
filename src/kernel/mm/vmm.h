/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * vmm.h - Virtual Memory Manager (4-Level Paging for x86_64).
 */

#ifndef KERNEL_VMM_H
#define KERNEL_VMM_H

#include <stdint.h>
#include <stddef.h>
#include "pmm.h"
#include "include/limine.h"

/* Paging Flags */
#define PAGE_PRESENT    (1ULL << 0)
#define PAGE_WRITABLE   (1ULL << 1)
#define PAGE_USER       (1ULL << 2)
#define PAGE_PWT        (1ULL << 3)
#define PAGE_PCD        (1ULL << 4)
#define PAGE_ACCESSED   (1ULL << 5)
#define PAGE_DIRTY      (1ULL << 6)
#define PAGE_HUGE       (1ULL << 7)
#define PAGE_GLOBAL     (1ULL << 8)
#define PAGE_NX         (1ULL << 63)

#define PAGE_ADDR_MASK  0x000FFFFFFFFFF000ULL

typedef uint64_t pml4_t;

void vmm_init(struct limine_memmap_response *mm, struct limine_kernel_address_response *kaddr, uint64_t hhdm_offset);
pml4_t *vmm_create_address_space(void);
pml4_t *vmm_clone_address_space(pml4_t *src_pml4);
void vmm_switch_address_space(pml4_t *pml4);
void vmm_map_page(pml4_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap_page(pml4_t *pml4, uint64_t virt);
uint64_t vmm_virt_to_phys(pml4_t *pml4, uint64_t virt);

pml4_t *vmm_get_kernel_address_space(void);
uint64_t vmm_get_hhdm_offset(void);

#endif /* KERNEL_VMM_H */
