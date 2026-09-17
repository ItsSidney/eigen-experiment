/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * tarfs.c - USTAR in-memory initramfs with copy-on-write write layer.
 *
 * Read-only tar data is never touched. The first write to any file copies its
 * content into a heap buffer; subsequent reads/writes use that buffer. New
 * files and directories are created entirely in heap memory.
 */

#include <stdint.h>
#include <stddef.h>
#include "tarfs.h"
#include "fs/vfs/vfs.h"
#include "mm/kheap.h"
#include "include/serial.h"

struct tar_header {
    char filename[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag[1];
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} __attribute__((packed));

/*
 * Per-file RAM backing. Used for COW copies of archive files and for brand-new
 * files created at runtime. Identified by VFS_NODE_FLAG_COW (bit 15) in
 * node->flags.
 */
#define TARFS_FLAG_COW  0x8000

struct tarfs_file {
    uint8_t *data;
    size_t   size;
    size_t   cap;
};

struct tar_dir_data {
    struct vfs_node **children;
    uint32_t count;
    uint32_t capacity;
};

static uint64_t tarfs_inode_next = 5000;

/* ── String helpers ──────────────────────────────────────────────────── */

static uint64_t parse_octal(const char *str, int len) {
    uint64_t val = 0;
    for (int i = 0; i < len && str[i]; i++) {
        if (str[i] >= '0' && str[i] <= '7')
            val = (val << 3) | (str[i] - '0');
    }
    return val;
}

static int kstrcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static void kstrncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
}

/* ── COW promotion: move archive bytes into heap on first write ──────── */

static struct tarfs_file *cow_promote(struct vfs_node *node) {
    if (node->flags & TARFS_FLAG_COW)
        return (struct tarfs_file *)node->device;

    /* node->device is currently a raw pointer into the archive image. */
    const uint8_t *src = (const uint8_t *)node->device;

    struct tarfs_file *f = (struct tarfs_file *)kzalloc(sizeof(struct tarfs_file));
    if (!f) return NULL;

    size_t cap = node->size ? node->size : 64;
    f->data = (uint8_t *)kmalloc(cap);
    if (!f->data) { kfree(f); return NULL; }
    f->cap  = cap;
    f->size = node->size;

    for (size_t i = 0; i < node->size; i++) f->data[i] = src[i];

    node->device = f;
    node->flags |= TARFS_FLAG_COW;
    return f;
}

/* ── File read (handles both raw-archive and COW-heap nodes) ─────────── */

static int64_t tarfs_file_read(struct vfs_node *node, uint64_t offset,
                                size_t size, void *buf) {
    if (offset >= node->size) return 0;
    if (offset + size > node->size) size = node->size - offset;

    const uint8_t *src;
    if (node->flags & TARFS_FLAG_COW) {
        struct tarfs_file *f = (struct tarfs_file *)node->device;
        src = f->data + offset;
    } else {
        src = (const uint8_t *)node->device + offset;
    }

    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < size; i++) dst[i] = src[i];
    return (int64_t)size;
}

/* ── File write (COW on first access) ───────────────────────────────── */

static int64_t tarfs_file_write(struct vfs_node *node, uint64_t offset,
                                 size_t size, const void *buf) {
    if (!node || !buf) return -14;

    struct tarfs_file *f = cow_promote(node);
    if (!f) return -12; /* -ENOMEM */

    uint64_t end = offset + (uint64_t)size;
    if (end > f->cap) {
        size_t ncap = f->cap ? f->cap : 64;
        while (ncap < end) ncap *= 2;
        uint8_t *nd = (uint8_t *)kmalloc(ncap);
        if (!nd) return -12;
        for (size_t i = 0; i < f->size; i++) nd[i] = f->data[i];
        kfree(f->data);
        f->data = nd;
        f->cap  = ncap;
    }

    /* Zero any gap between EOF and write offset. */
    if (offset > f->size) {
        for (uint64_t i = f->size; i < offset; i++) f->data[i] = 0;
    }

    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < size; i++) f->data[offset + i] = src[i];

    if (end > f->size) f->size = (size_t)end;
    node->size = f->size;
    return (int64_t)size;
}

/* ── File truncate ───────────────────────────────────────────────────── */

static int tarfs_file_truncate(struct vfs_node *node, int64_t length) {
    if (!node || length < 0) return -22;
    struct tarfs_file *f = cow_promote(node);
    if (!f) return -12;

    if ((uint64_t)length > f->cap) {
        size_t ncap = f->cap ? f->cap : 64;
        while (ncap < (uint64_t)length) ncap *= 2;
        uint8_t *nd = (uint8_t *)kmalloc(ncap);
        if (!nd) return -12;
        for (size_t i = 0; i < f->size; i++) nd[i] = f->data[i];
        kfree(f->data);
        f->data = nd;
        f->cap  = ncap;
    }
    /* Zero-fill any gap when growing. */
    for (uint64_t i = f->size; i < (uint64_t)length; i++) f->data[i] = 0;
    f->size    = (size_t)length;
    node->size = (uint64_t)length;
    return 0;
}

/* ── Directory operations ────────────────────────────────────────────── */

static struct vfs_node *tarfs_dir_finddir(struct vfs_node *node,
                                           const char *name) {
    struct tar_dir_data *dir = (struct tar_dir_data *)node->device;
    if (!dir) return NULL;
    for (uint32_t i = 0; i < dir->count; i++) {
        if (kstrcmp(dir->children[i]->name, name) == 0)
            return dir->children[i];
    }
    return NULL;
}

static struct vfs_dirent *tarfs_dir_readdir(struct vfs_node *node,
                                             uint32_t index) {
    static struct vfs_dirent dirent;
    struct tar_dir_data *dir = (struct tar_dir_data *)node->device;
    if (!dir || index >= dir->count) return NULL;

    struct vfs_node *child = dir->children[index];
    kstrncpy(dirent.name, child->name, sizeof(dirent.name));
    dirent.ino  = child->inode;
    dirent.type = child->type;
    dirent.size = child->size;
    return &dirent;
}

static void dir_add_child(struct vfs_node *parent, struct vfs_node *child) {
    struct tar_dir_data *dir = (struct tar_dir_data *)parent->device;
    if (!dir) return;
    if (dir->count >= dir->capacity) {
        uint32_t ncap = dir->capacity == 0 ? 8 : dir->capacity * 2;
        struct vfs_node **nc = (struct vfs_node **)kmalloc(ncap * sizeof(struct vfs_node *));
        for (uint32_t i = 0; i < dir->count; i++) nc[i] = dir->children[i];
        if (dir->children) kfree(dir->children);
        dir->children = nc;
        dir->capacity = ncap;
    }
    dir->children[dir->count++] = child;
}

static int dir_remove_child(struct vfs_node *parent, const char *name) {
    struct tar_dir_data *dir = (struct tar_dir_data *)parent->device;
    if (!dir) return -2;
    for (uint32_t i = 0; i < dir->count; i++) {
        if (kstrcmp(dir->children[i]->name, name) == 0) {
            dir->children[i] = dir->children[--dir->count];
            return 0;
        }
    }
    return -2;
}

/* ── Forward declarations for create_dir_node ───────────────────────── */
static int tarfs_create(struct vfs_node *parent, const char *name, uint16_t mode);
static int tarfs_mkdir(struct vfs_node *parent, const char *name, uint16_t mode);
static int tarfs_unlink(struct vfs_node *parent, const char *name);
static int tarfs_rmdir(struct vfs_node *parent, const char *name);

/* ── create_dir_node: allocate writable directory ───────────────────── */

static struct vfs_node *create_dir_node(const char *name) {
    struct vfs_node *node = (struct vfs_node *)kzalloc(sizeof(struct vfs_node));
    kstrncpy(node->name, name, sizeof(node->name));
    node->type        = VFS_DIRECTORY;
    node->permissions = 0755;
    node->finddir     = tarfs_dir_finddir;
    node->readdir     = tarfs_dir_readdir;
    node->create      = tarfs_create;
    node->mkdir       = tarfs_mkdir;
    node->unlink      = tarfs_unlink;
    node->rmdir       = tarfs_rmdir;

    struct tar_dir_data *dir = (struct tar_dir_data *)kzalloc(sizeof(struct tar_dir_data));
    node->device = dir;
    return node;
}

/* ── Runtime create/mkdir/unlink/rmdir ───────────────────────────────── */

static int tarfs_create(struct vfs_node *parent, const char *name,
                         uint16_t mode) {
    if (!parent || !name) return -22;
    if (tarfs_dir_finddir(parent, name)) return -17; /* -EEXIST */

    struct tarfs_file *tf = (struct tarfs_file *)kzalloc(sizeof(struct tarfs_file));
    if (!tf) return -12;
    tf->data = (uint8_t *)kmalloc(64);
    if (!tf->data) { kfree(tf); return -12; }
    tf->cap  = 64;
    tf->size = 0;

    struct vfs_node *f = (struct vfs_node *)kzalloc(sizeof(struct vfs_node));
    if (!f) { kfree(tf->data); kfree(tf); return -12; }
    kstrncpy(f->name, name, sizeof(f->name));
    f->type        = VFS_FILE;
    f->permissions = mode ? (uint32_t)mode : 0644;
    f->inode       = tarfs_inode_next++;
    f->size        = 0;
    f->device      = tf;
    f->flags       = TARFS_FLAG_COW;
    f->read        = tarfs_file_read;
    f->write       = tarfs_file_write;
    f->truncate    = tarfs_file_truncate;

    dir_add_child(parent, f);
    return 0;
}

static int tarfs_mkdir(struct vfs_node *parent, const char *name,
                        uint16_t mode) {
    if (!parent || !name) return -22;
    if (tarfs_dir_finddir(parent, name)) return -17;

    struct vfs_node *d = create_dir_node(name);
    if (!d) return -12;
    d->inode       = tarfs_inode_next++;
    d->permissions = mode ? (uint32_t)mode : 0755;
    dir_add_child(parent, d);
    return 0;
}

static int tarfs_unlink(struct vfs_node *parent, const char *name) {
    if (!parent || !name) return -22;
    struct vfs_node *child = tarfs_dir_finddir(parent, name);
    if (!child) return -2;
    if (child->type == VFS_DIRECTORY) return -21; /* -EISDIR: cannot unlink directory */

    if (child->flags & TARFS_FLAG_COW) {
        struct tarfs_file *tf = (struct tarfs_file *)child->device;
        if (tf) {
            if (tf->data) kfree(tf->data);
            kfree(tf);
        }
    }
    dir_remove_child(parent, name);
    kfree(child);
    return 0;
}

static int tarfs_rmdir(struct vfs_node *parent, const char *name) {
    if (!parent || !name) return -22;
    struct vfs_node *child = tarfs_dir_finddir(parent, name);
    if (!child) return -2;
    if (child->type != VFS_DIRECTORY) return -20; /* -ENOTDIR: cannot rmdir non-directory */

    struct tar_dir_data *dir = (struct tar_dir_data *)child->device;
    if (dir && dir->count > 0) return -39; /* -ENOTEMPTY */
    if (dir) {
        if (dir->children) kfree(dir->children);
        kfree(dir);
    }
    dir_remove_child(parent, name);
    kfree(child);
    return 0;
}

/* ── Archive parser ──────────────────────────────────────────────────── */

struct vfs_node *tarfs_init(void *tar_archive, size_t archive_size) {
    if (!tar_archive || archive_size < 512) return NULL;

    struct vfs_node *root = create_dir_node("/");

    uint8_t *ptr = (uint8_t *)tar_archive;
    uint8_t *end = ptr + archive_size;
    uint64_t inode_counter = 100;

    while (ptr + 512 <= end) {
        struct tar_header *hdr = (struct tar_header *)ptr;
        if (hdr->filename[0] == '\0') break;

        uint64_t size = parse_octal(hdr->size, 11);
        uint32_t mode = (uint32_t)parse_octal(hdr->mode, 7);

        char path[128];
        kstrncpy(path, hdr->filename, sizeof(path));

        size_t plen = 0;
        while (path[plen]) plen++;
        if (plen > 1 && path[plen - 1] == '/') path[plen - 1] = '\0';

        struct vfs_node *curr = root;
        char *p = path;
        if (*p == '.' && *(p + 1) == '/') p += 2;
        if (*p == '/') p++;

        char part[64];
        while (*p) {
            int i = 0;
            while (*p && *p != '/' && i < 63) part[i++] = *p++;
            part[i] = '\0';

            if (*p == '/') {
                p++;
                struct vfs_node *next = curr->finddir(curr, part);
                if (!next) {
                    next = create_dir_node(part);
                    next->inode = inode_counter++;
                    dir_add_child(curr, next);
                }
                curr = next;
            } else {
                if (part[0] == '\0' || (part[0] == '.' && part[1] == '\0'))
                    break;

                if (hdr->typeflag[0] == '5') {
                    if (!curr->finddir(curr, part)) {
                        struct vfs_node *d = create_dir_node(part);
                        d->inode       = inode_counter++;
                        d->permissions = mode ? mode : 0755;
                        dir_add_child(curr, d);
                    }
                } else if (hdr->typeflag[0] == '2') {
                    if (!curr->finddir(curr, part)) {
                        struct vfs_node *l = (struct vfs_node *)kzalloc(sizeof(struct vfs_node));
                        kstrncpy(l->name, part, sizeof(l->name));
                        l->type        = VFS_SYMLINK;
                        l->permissions = 0777;
                        l->inode       = inode_counter++;
                        kstrncpy(l->linktarget, hdr->linkname, sizeof(l->linktarget));
                        dir_add_child(curr, l);
                    }
                } else {
                    /* Regular file — COW on first write, raw pointer until then. */
                    struct vfs_node *f = (struct vfs_node *)kzalloc(sizeof(struct vfs_node));
                    kstrncpy(f->name, part, sizeof(f->name));
                    f->type        = VFS_FILE;
                    f->size        = size;
                    f->permissions = mode ? mode : 0644;
                    f->inode       = inode_counter++;
                    f->device      = (void *)(ptr + 512);
                    f->flags       = 0; /* not yet promoted */
                    f->read        = tarfs_file_read;
                    f->write       = tarfs_file_write;
                    f->truncate    = tarfs_file_truncate;
                    dir_add_child(curr, f);
                }
            }
        }

        ptr += 512 + ((size + 511) & ~511ULL);
    }

    serial_puts("[TARFS] In-memory archive mounted (COW write layer active).\n");
    return root;
}
