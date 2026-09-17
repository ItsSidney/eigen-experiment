/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * vmm.c - Virtual Memory Manager (x86_64 4-Level Paging).
 */

#include <stdint.h>
#include <stddef.h>
#include "vmm.h"
#include "pmm.h"
#include "include/serial.h"

static pml4_t *kernel_pml4 = NULL;
static uint64_t kernel_pml4_phys = 0;
static uint64_t hhdm = 0;

static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm);
}

static inline void invlpg(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

static void *alloc_zeroed_page(void) {
    void *page_phys = pmm_alloc_page();
    if (!page_phys) return NULL;
    uint64_t *page_virt = (uint64_t *)phys_to_virt((uint64_t)page_phys);
    for (int i = 0; i < 512; i++) {
        page_virt[i] = 0;
    }
    return page_phys;
}

void vmm_map_page(pml4_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pml4_virt = (uint64_t *)phys_to_virt((uint64_t)pml4);

    /* Level 4: PML4 -> PDPT */
    if (!(pml4_virt[pml4_idx] & PAGE_PRESENT)) {
        void *pdpt_phys = alloc_zeroed_page();
        pml4_virt[pml4_idx] = (uint64_t)pdpt_phys | PAGE_PRESENT | PAGE_WRITABLE | (flags & PAGE_USER);
    } else if (flags & PAGE_USER) {
        pml4_virt[pml4_idx] |= PAGE_USER;
    }

    uint64_t *pdpt_virt = (uint64_t *)phys_to_virt(pml4_virt[pml4_idx] & PAGE_ADDR_MASK);

    /* Level 3: PDPT -> PD */
    if (!(pdpt_virt[pdpt_idx] & PAGE_PRESENT)) {
        void *pd_phys = alloc_zeroed_page();
        pdpt_virt[pdpt_idx] = (uint64_t)pd_phys | PAGE_PRESENT | PAGE_WRITABLE | (flags & PAGE_USER);
    } else if (flags & PAGE_USER) {
        pdpt_virt[pdpt_idx] |= PAGE_USER;
    }

    uint64_t *pd_virt = (uint64_t *)phys_to_virt(pdpt_virt[pdpt_idx] & PAGE_ADDR_MASK);

    /* Level 2: PD -> PT */
    if (!(pd_virt[pd_idx] & PAGE_PRESENT)) {
        void *pt_phys = alloc_zeroed_page();
        pd_virt[pd_idx] = (uint64_t)pt_phys | PAGE_PRESENT | PAGE_WRITABLE | (flags & PAGE_USER);
    } else if (flags & PAGE_USER) {
        pd_virt[pd_idx] |= PAGE_USER;
    }

    uint64_t *pt_virt = (uint64_t *)phys_to_virt(pd_virt[pd_idx] & PAGE_ADDR_MASK);

    /* Level 1: PT entry */
    pt_virt[pt_idx] = (phys & PAGE_ADDR_MASK) | flags;
    invlpg(virt);
}

void vmm_unmap_page(pml4_t *pml4, uint64_t virt) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pml4_virt = (uint64_t *)phys_to_virt((uint64_t)pml4);
    if (!(pml4_virt[pml4_idx] & PAGE_PRESENT)) return;

    uint64_t *pdpt_virt = (uint64_t *)phys_to_virt(pml4_virt[pml4_idx] & PAGE_ADDR_MASK);
    if (!(pdpt_virt[pdpt_idx] & PAGE_PRESENT)) return;

    uint64_t *pd_virt = (uint64_t *)phys_to_virt(pdpt_virt[pdpt_idx] & PAGE_ADDR_MASK);
    if (!(pd_virt[pd_idx] & PAGE_PRESENT)) return;

    uint64_t *pt_virt = (uint64_t *)phys_to_virt(pd_virt[pd_idx] & PAGE_ADDR_MASK);
    pt_virt[pt_idx] = 0;
    invlpg(virt);
}

uint64_t vmm_virt_to_phys(pml4_t *pml4, uint64_t virt) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pml4_virt = (uint64_t *)phys_to_virt((uint64_t)pml4);
    if (!(pml4_virt[pml4_idx] & PAGE_PRESENT)) return 0;

    uint64_t *pdpt_virt = (uint64_t *)phys_to_virt(pml4_virt[pml4_idx] & PAGE_ADDR_MASK);
    if (!(pdpt_virt[pdpt_idx] & PAGE_PRESENT)) return 0;

    uint64_t *pd_virt = (uint64_t *)phys_to_virt(pdpt_virt[pdpt_idx] & PAGE_ADDR_MASK);
    if (!(pd_virt[pd_idx] & PAGE_PRESENT)) return 0;

    uint64_t *pt_virt = (uint64_t *)phys_to_virt(pd_virt[pd_idx] & PAGE_ADDR_MASK);
    if (!(pt_virt[pt_idx] & PAGE_PRESENT)) return 0;

    return (pt_virt[pt_idx] & PAGE_ADDR_MASK) | (virt & 0xFFF);
}

pml4_t *vmm_create_address_space(void) {
    pml4_t *new_pml4 = (pml4_t *)alloc_zeroed_page();
    if (!new_pml4) return NULL;

    uint64_t *new_virt = (uint64_t *)phys_to_virt((uint64_t)new_pml4);
    uint64_t *kernel_virt = (uint64_t *)phys_to_virt((uint64_t)kernel_pml4);

    /* Copy higher half entries (indices 256 to 511) from kernel address space */
    for (int i = 256; i < 512; i++) {
        new_virt[i] = kernel_virt[i];
    }

    return new_pml4;
}

pml4_t *vmm_clone_address_space(pml4_t *src_pml4) {
    if (!src_pml4) return NULL;
    pml4_t *new_pml4 = vmm_create_address_space();
    if (!new_pml4) return NULL;

    uint64_t *src_pml4_virt = (uint64_t *)phys_to_virt((uint64_t)src_pml4);

    /* Lower half only (user space entries 0..255) */
    for (int pml4_i = 0; pml4_i < 256; pml4_i++) {
        if (!(src_pml4_virt[pml4_i] & PAGE_PRESENT)) continue;

        uint64_t *src_pdpt_virt = (uint64_t *)phys_to_virt(src_pml4_virt[pml4_i] & PAGE_ADDR_MASK);
        for (int pdpt_i = 0; pdpt_i < 512; pdpt_i++) {
            if (!(src_pdpt_virt[pdpt_i] & PAGE_PRESENT)) continue;

            uint64_t *src_pd_virt = (uint64_t *)phys_to_virt(src_pdpt_virt[pdpt_i] & PAGE_ADDR_MASK);
            for (int pd_i = 0; pd_i < 512; pd_i++) {
                if (!(src_pd_virt[pd_i] & PAGE_PRESENT)) continue;

                uint64_t *src_pt_virt = (uint64_t *)phys_to_virt(src_pd_virt[pd_i] & PAGE_ADDR_MASK);
                for (int pt_i = 0; pt_i < 512; pt_i++) {
                    uint64_t pte = src_pt_virt[pt_i];
                    if (!(pte & PAGE_PRESENT)) continue;

                    /* Reconstruct virtual address */
                    uint64_t vaddr = ((uint64_t)pml4_i << 39) |
                                     ((uint64_t)pdpt_i << 30) |
                                     ((uint64_t)pd_i << 21)   |
                                     ((uint64_t)pt_i << 12);

                    void *new_phys = pmm_alloc_page();
                    if (!new_phys) continue;

                    uint8_t *src_page = (uint8_t *)phys_to_virt(pte & PAGE_ADDR_MASK);
                    uint8_t *dst_page = (uint8_t *)phys_to_virt((uint64_t)new_phys);
                    for (int b = 0; b < 4096; b++) {
                        dst_page[b] = src_page[b];
                    }

                    uint64_t flags = pte & 0xFFF;
                    vmm_map_page(new_pml4, vaddr, (uint64_t)new_phys, flags);
                }
            }
        }
    }

    return new_pml4;
}

void vmm_switch_address_space(pml4_t *pml4) {
    __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)pml4) : "memory");
}

pml4_t *vmm_get_kernel_address_space(void) {
    return kernel_pml4;
}

void vmm_init(struct limine_memmap_response *mm, struct limine_kernel_address_response *kaddr, uint64_t hhdm_offset) {
    hhdm = hhdm_offset;

    /* 1. Allocate kernel PML4 root */
    kernel_pml4_phys = (uint64_t)alloc_zeroed_page();
    kernel_pml4 = (pml4_t *)kernel_pml4_phys;

    /* 2. Identity map all physical memory to HHDM region (virt = hhdm + phys) */
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *entry = mm->entries[i];

        /* Skip bad memory and high-memory reserved ranges (e.g. 1TB PCI holes) */
        if (entry->type == LIMINE_MEMMAP_BAD_MEMORY) continue;
        if (entry->type == LIMINE_MEMMAP_RESERVED && entry->base >= 0x100000000ULL) continue;

        uint64_t base = entry->base & ~(PAGE_SIZE - 1);
        uint64_t top = (entry->base + entry->length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (uint64_t phys = base; phys < top; phys += PAGE_SIZE) {
            vmm_map_page(kernel_pml4, hhdm + phys, phys, PAGE_PRESENT | PAGE_WRITABLE);
        }
    }

    /* 3. Map kernel ELF image (e.g. 16MB region at virtual_base) */
    uint64_t kphys_base = kaddr ? kaddr->physical_base : 0;
    uint64_t kvirt_base = kaddr ? kaddr->virtual_base : 0xFFFFFFFF80200000ULL;
    uint64_t ksize = 16 * 1024 * 1024; /* 16MB */

    for (uint64_t offset = 0; offset < ksize; offset += PAGE_SIZE) {
        vmm_map_page(kernel_pml4, kvirt_base + offset, kphys_base + offset, PAGE_PRESENT | PAGE_WRITABLE);
    }

    /* 4. Switch to our new kernel page table */
    vmm_switch_address_space(kernel_pml4);
    serial_puts("[VMM] 4-level paging initialized and active (CR3 switched).\n");
}
/* Returns the HHDM offset: physical_address + hhdm_offset = kernel_virtual_address */
uint64_t vmm_get_hhdm_offset(void) {
    return hhdm;
}
