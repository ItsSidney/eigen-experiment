/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * shmem.c - anonymous shared-memory objects behind memfd_create.
 *
 * Layout: one kmalloc'd array of physical page addresses plus a size. Reads
 * and writes copy through the HHDM; the mmap hook maps the frames directly
 * into the caller, which is what makes the mapping *shared* - the client and
 * the compositor touch the same memory.
 */

#include <stdint.h>
#include <stddef.h>
#include "shmem.h"
#include "fs/vfs/vfs.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"
#include "proc/process.h"

#define SHMEM_MAX_PAGES  4096             /* 16 MB per object */
#define SHMEM_PAGE_SIZE  4096ULL

struct shmem_obj {
    struct vfs_node node;
    uint64_t        npages;
    uint64_t       *pages;   /* physical frame addresses */
};

static uint8_t *sh_page_virt(uint64_t phys) {
    return (uint8_t *)(vmm_get_hhdm_offset() + phys);
}

/* Make sure the object owns at least `want` pages; new pages read as zero. */
static int sh_reserve(struct shmem_obj *o, uint64_t want) {
    if (want <= o->npages) return 0;
    if (want > SHMEM_MAX_PAGES) return -27; /* -EFBIG */

    uint64_t *np = (uint64_t *)kmalloc(sizeof(uint64_t) * want);
    if (!np) return -12; /* -ENOMEM */
    for (uint64_t i = 0; i < o->npages; i++) np[i] = o->pages[i];

    for (uint64_t i = o->npages; i < want; i++) {
        void *p = pmm_alloc_page();
        if (!p) {
            /* Roll back the frames we did get this round. */
            for (uint64_t k = o->npages; k < i; k++)
                pmm_free_page((void *)np[k]);
            kfree(np);
            return -12;
        }
        uint8_t *v = sh_page_virt((uint64_t)p);
        for (int b = 0; b < 4096; b++) v[b] = 0;
        np[i] = (uint64_t)p;
    }

    if (o->pages) kfree(o->pages);
    o->pages = np;
    o->npages = want;
    return 0;
}
static int64_t sh_read(struct vfs_node *node, uint64_t offset, size_t size,
                       void *buf) {
    struct shmem_obj *o = (struct shmem_obj *)node->device;
    if (!o || !buf) return -14;                 /* -EFAULT */
    if (offset >= node->size) return 0;
    if (offset + size > node->size) size = (size_t)(node->size - offset);

    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0;
    while (done < size) {
        uint64_t off = offset + done;
        uint64_t page = off / SHMEM_PAGE_SIZE;
        size_t in_page = (size_t)(off % SHMEM_PAGE_SIZE);
        size_t chunk = size - done;
        if (chunk > SHMEM_PAGE_SIZE - in_page) chunk = SHMEM_PAGE_SIZE - in_page;
        if (page >= o->npages) break;
        const uint8_t *src = sh_page_virt(o->pages[page]) + in_page;
        for (size_t i = 0; i < chunk; i++) dst[done + i] = src[i];
        done += chunk;
    }
    return (int64_t)done;
}

static int64_t sh_write(struct vfs_node *node, uint64_t offset, size_t size,
                        const void *buf) {
    struct shmem_obj *o = (struct shmem_obj *)node->device;
    if (!o || !buf) return -14;                 /* -EFAULT */

    uint64_t end = offset + size;
    uint64_t need = (end + SHMEM_PAGE_SIZE - 1) / SHMEM_PAGE_SIZE;
    int r = sh_reserve(o, need);
    if (r != 0) return r;
    if (end > node->size) node->size = end;

    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;
    while (done < size) {
        uint64_t off = offset + done;
        uint64_t page = off / SHMEM_PAGE_SIZE;
        size_t in_page = (size_t)(off % SHMEM_PAGE_SIZE);
        size_t chunk = size - done;
        if (chunk > SHMEM_PAGE_SIZE - in_page) chunk = SHMEM_PAGE_SIZE - in_page;
        uint8_t *dst = sh_page_virt(o->pages[page]) + in_page;
        for (size_t i = 0; i < chunk; i++) dst[i] = src[done + i];
        done += chunk;
    }
    return (int64_t)size;
}

/* ftruncate: the size Wayland asks for when it sets up a buffer pool. */
static int sh_truncate(struct vfs_node *node, int64_t length) {
    struct shmem_obj *o = (struct shmem_obj *)node->device;
    if (!o || length < 0) return -22;           /* -EINVAL */
    uint64_t want = ((uint64_t)length + SHMEM_PAGE_SIZE - 1) / SHMEM_PAGE_SIZE;
    int r = sh_reserve(o, want);
    if (r != 0) return r;
    node->size = (uint64_t)length;
    return 0;
}

/* mmap: map the object's own frames, so every mapper shares them. This is
 * the property tmpfs cannot offer. */
static int64_t sh_mmap(struct vfs_node *node, uint64_t vaddr, uint64_t size,
                       uint64_t offset, uint64_t pte_flags) {
    struct shmem_obj *o = (struct shmem_obj *)node->device;
    if (!o || size == 0) return -22;            /* -EINVAL */
    if (offset % SHMEM_PAGE_SIZE) return -22;   /* must be page-aligned */

    uint64_t pte = pte_flags | PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE;
    uint64_t page = offset / SHMEM_PAGE_SIZE;

    pml4_t *pml4 = current_process ? current_process->pml4
                                   : vmm_get_kernel_address_space();
    for (uint64_t done = 0; done < size;
         done += SHMEM_PAGE_SIZE, page++, vaddr += SHMEM_PAGE_SIZE) {
        int r = sh_reserve(o, page + 1);
        if (r != 0) return r;
        vmm_map_page(pml4, vaddr, o->pages[page], pte);
    }
    return 0;
}

static int sh_close(struct vfs_node *node) {
    /* The object is deliberately NOT freed here. dup(2) and fork() copy the
     * fd table, so two fds can name the same object and a second close would
     * free memory the first one still references. Reclaiming it needs a
     * dup-aware reference count; until that exists the pages are released at
     * reboot. Correct beats tidy: the alternative is a use-after-free.
     *
     * A client that creates and destroys pools in a loop will grow the
     * kernel's shmem footprint - noted as a known limitation. */
    (void)node;
    return 0;
}

int shmem_create(const char *name, uint64_t size) {
    (void)name; /* anonymous: the name is only for /proc-style reporting */

    struct shmem_obj *o = (struct shmem_obj *)kzalloc(sizeof(*o));
    if (!o) return -12; /* -ENOMEM */

    o->node.type        = VFS_FILE;
    o->node.permissions = 0600;
    o->node.inode       = 0;   /* anonymous: no on-disk identity */
    o->node.device      = o;
    o->node.size        = 0;
    o->node.read        = sh_read;
    o->node.write       = sh_write;
    o->node.truncate    = sh_truncate;
    o->node.mmap        = sh_mmap;
    o->node.close       = sh_close;

    int fd = vfs_install_node(&o->node);
    if (fd < 0) {
        kfree(o);
        return fd;
    }

    if (size) {
        int r = sh_truncate(&o->node, (int64_t)size);
        if (r != 0) {
            vfs_close(fd);
            return r;
        }
    }
    return fd;
}
