/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * kheap.c - Kernel heap allocator with page-backed expansion and block splitting/coalescing.
 */

#include <stdint.h>
#include <stddef.h>
#include "kheap.h"
#include "pmm.h"
#include "vmm.h"
#include "include/serial.h"

#define KHEAP_START   0xFFFFFFFF90000000ULL
#define KHEAP_HEADER_MAGIC 0xEA910515ULL

struct heap_block {
    uint32_t magic;
    uint32_t is_free;
    size_t size; /* size of usable memory excluding header */
    struct heap_block *next;
    struct heap_block *prev;
};

#define BLOCK_HEADER_SIZE sizeof(struct heap_block)

static uint64_t heap_top = KHEAP_START;
static struct heap_block *head_block = NULL;

static void *expand_heap(size_t bytes) {
    size_t pages_needed = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t old_top = heap_top;

    for (size_t i = 0; i < pages_needed; i++) {
        void *paddr = pmm_alloc_page();
        if (!paddr) {
            serial_puts("[KHEAP] Failed to allocate physical page for heap expansion!\n");
            return NULL;
        }
        vmm_map_page(vmm_get_kernel_address_space(), heap_top, (uint64_t)paddr, PAGE_PRESENT | PAGE_WRITABLE);
        heap_top += PAGE_SIZE;
    }

    return (void *)old_top;
}

void kheap_init(void) {
    /* Pre-allocate initial 64 KB for early boot heap */
    size_t initial_bytes = 64 * 1024;
    void *base = expand_heap(initial_bytes);
    if (!base) {
        serial_puts("[KHEAP] FATAL: Heap initialization failed!\n");
        return;
    }

    head_block = (struct heap_block *)base;
    head_block->magic = KHEAP_HEADER_MAGIC;
    head_block->is_free = 1;
    head_block->size = initial_bytes - BLOCK_HEADER_SIZE;
    head_block->next = NULL;
    head_block->prev = NULL;

    serial_puts("[KHEAP] Initialized at 0x");
    serial_puthex(KHEAP_START);
    serial_puts(" (64 KB initial)\n");
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    /* Align size to 16 bytes for alignment */
    size = (size + 15) & ~15ULL;

    struct heap_block *curr = head_block;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            /* Check if we can split this block */
            if (curr->size >= size + BLOCK_HEADER_SIZE + 32) {
                struct heap_block *new_block = (struct heap_block *)((uint8_t *)curr + BLOCK_HEADER_SIZE + size);
                new_block->magic = KHEAP_HEADER_MAGIC;
                new_block->is_free = 1;
                new_block->size = curr->size - size - BLOCK_HEADER_SIZE;
                new_block->next = curr->next;
                new_block->prev = curr;

                if (curr->next) curr->next->prev = new_block;
                curr->next = new_block;
                curr->size = size;
            }

            curr->is_free = 0;
            return (void *)((uint8_t *)curr + BLOCK_HEADER_SIZE);
        }
        if (!curr->next) break;
        curr = curr->next;
    }

    /* Need to expand heap */
    size_t alloc_bytes = size + BLOCK_HEADER_SIZE;
    if (alloc_bytes < 32 * 1024) alloc_bytes = 32 * 1024; /* Expand by at least 32KB */

    void *new_mem = expand_heap(alloc_bytes);
    if (!new_mem) return NULL;

    struct heap_block *expanded = (struct heap_block *)new_mem;
    expanded->magic = KHEAP_HEADER_MAGIC;
    expanded->is_free = 0;
    expanded->size = size;
    expanded->prev = curr;
    expanded->next = NULL;
    if (curr) curr->next = expanded;

    /* Remaining chunk from expansion becomes a free block */
    size_t leftover = alloc_bytes - size - BLOCK_HEADER_SIZE;
    if (leftover > BLOCK_HEADER_SIZE + 16) {
        struct heap_block *free_chunk = (struct heap_block *)((uint8_t *)expanded + BLOCK_HEADER_SIZE + size);
        free_chunk->magic = KHEAP_HEADER_MAGIC;
        free_chunk->is_free = 1;
        free_chunk->size = leftover - BLOCK_HEADER_SIZE;
        free_chunk->prev = expanded;
        free_chunk->next = NULL;
        expanded->next = free_chunk;
    }

    return (void *)((uint8_t *)expanded + BLOCK_HEADER_SIZE);
}

void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr) {
        uint8_t *b = (uint8_t *)ptr;
        for (size_t i = 0; i < size; i++) b[i] = 0;
    }
    return ptr;
}

void kfree(void *ptr) {
    if (!ptr) return;

    struct heap_block *block = (struct heap_block *)((uint8_t *)ptr - BLOCK_HEADER_SIZE);
    if (block->magic != KHEAP_HEADER_MAGIC) {
        serial_puts("[KHEAP] Warning: Invalid kfree pointer magic!\n");
        return;
    }

    block->is_free = 1;

    /* Coalesce with next block if free */
    if (block->next && block->next->is_free) {
        block->size += BLOCK_HEADER_SIZE + block->next->size;
        block->next = block->next->next;
        if (block->next) block->next->prev = block;
    }

    /* Coalesce with previous block if free */
    if (block->prev && block->prev->is_free) {
        block->prev->size += BLOCK_HEADER_SIZE + block->size;
        block->prev->next = block->next;
        if (block->next) block->next->prev = block->prev;
    }
}
