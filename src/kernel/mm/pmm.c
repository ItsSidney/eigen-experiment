/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * pmm.c - Physical Memory Manager implementation.
 * Uses a bitmap tracking 4096-byte frames.
 */

#include <stdint.h>
#include <stddef.h>
#include "pmm.h"
#include "include/serial.h"

static uint64_t *bitmap = NULL;
static uint64_t total_pages = 0;
static uint64_t free_pages = 0;
static uint64_t hhdm = 0;
static uint64_t last_searched_page = 0;

static inline void bitmap_set(uint64_t page) {
    bitmap[page / 64] |= (1ULL << (page % 64));
}

static inline void bitmap_clear(uint64_t page) {
    bitmap[page / 64] &= ~(1ULL << (page % 64));
}

static inline int bitmap_test(uint64_t page) {
    return (bitmap[page / 64] & (1ULL << (page % 64))) != 0;
}

void pmm_init(struct limine_memmap_response *mm, uint64_t hhdm_offset) {
    hhdm = hhdm_offset;
    uint64_t highest_addr = 0;

    /* 1. Calculate highest usable physical address to size the bitmap */
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *entry = mm->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t top = entry->base + entry->length;
            if (top > highest_addr) {
                highest_addr = top;
            }
        }
    }

    total_pages = highest_addr / PAGE_SIZE;
    uint64_t bitmap_size = ((total_pages + 63) / 64) * sizeof(uint64_t);
    uint64_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;

    /* 2. Find a usable memory region large enough to store the bitmap */
    uint64_t bitmap_phys = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *entry = mm->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= bitmap_size) {
            bitmap_phys = entry->base;
            break;
        }
    }

    if (!bitmap_phys) {
        serial_puts("[PMM] FATAL: Could not find space for memory bitmap!\n");
        for (;;) __asm__ volatile("hlt");
    }

    /* Access the bitmap buffer using the Higher Half Direct Map */
    bitmap = (uint64_t *)(bitmap_phys + hhdm);

    /* 3. Initially mark all memory as used (1) */
    for (uint64_t i = 0; i < (bitmap_size / sizeof(uint64_t)); i++) {
        bitmap[i] = ~0ULL;
    }
    free_pages = 0;

    /* 4. Mark all Limine USABLE regions as free (0) */
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *entry = mm->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t start_page = entry->base / PAGE_SIZE;
            uint64_t count = entry->length / PAGE_SIZE;
            for (uint64_t p = 0; p < count; p++) {
                bitmap_clear(start_page + p);
                free_pages++;
            }
        }
    }

    /* 5. Mark first page (0x0 - 0x1000) as used so NULL is never allocated */
    bitmap_set(0);
    if (free_pages > 0) free_pages--;

    /* 6. Mark the pages occupied by the bitmap itself as used */
    uint64_t bitmap_start_page = bitmap_phys / PAGE_SIZE;
    for (uint64_t p = 0; p < bitmap_pages; p++) {
        bitmap_set(bitmap_start_page + p);
        if (free_pages > 0) free_pages--;
    }

    serial_puts("[PMM] Initialized: ");
    serial_puthex(free_pages * PAGE_SIZE / (1024 * 1024));
    serial_puts(" MB free of ");
    serial_puthex(total_pages * PAGE_SIZE / (1024 * 1024));
    serial_puts(" MB total\n");
}

void *pmm_alloc_page(void) {
    uint64_t num_words = (total_pages + 63) / 64;
    uint64_t start_word = last_searched_page / 64;

    for (uint64_t i = 0; i < num_words; i++) {
        uint64_t idx = (start_word + i) % num_words;
        if (bitmap[idx] != ~0ULL) {
            /* Found a word with at least one free page (0 bit) */
            int bit = __builtin_ctzll(~bitmap[idx]);
            uint64_t page = idx * 64 + bit;
            if (page < total_pages) {
                bitmap_set(page);
                free_pages--;
                last_searched_page = page + 1;
                return (void *)(page * PAGE_SIZE);
            }
        }
    }

    serial_puts("[PMM] Out of physical memory!\n");
    return NULL;
}

void *pmm_alloc_pages(size_t count) {
    if (count == 0) return NULL;
    if (count == 1) return pmm_alloc_page();

    uint64_t consecutive = 0;
    uint64_t start_page = 0;

    for (uint64_t p = 0; p < total_pages; p++) {
        if (!bitmap_test(p)) {
            if (consecutive == 0) start_page = p;
            consecutive++;
            if (consecutive == count) {
                for (uint64_t i = 0; i < count; i++) {
                    bitmap_set(start_page + i);
                    free_pages--;
                }
                return (void *)(start_page * PAGE_SIZE);
            }
        } else {
            consecutive = 0;
        }
    }

    serial_puts("[PMM] Failed to allocate contiguous pages!\n");
    return NULL;
}

uint64_t pmm_get_free_pages(void)  { return free_pages; }
uint64_t pmm_get_total_pages(void) { return total_pages; }

void pmm_free_page(void *paddr) {
    uint64_t page = (uint64_t)paddr / PAGE_SIZE;
    if (page < total_pages && bitmap_test(page)) {
        bitmap_clear(page);
        free_pages++;
    }
}

void pmm_free_pages(void *paddr, size_t count) {
    uint64_t start_page = (uint64_t)paddr / PAGE_SIZE;
    for (size_t i = 0; i < count; i++) {
        uint64_t page = start_page + i;
        if (page < total_pages && bitmap_test(page)) {
            bitmap_clear(page);
            free_pages++;
        }
    }
}

uint64_t pmm_get_total_memory(void) {
    return total_pages * PAGE_SIZE;
}

uint64_t pmm_get_free_memory(void) {
    return free_pages * PAGE_SIZE;
}
