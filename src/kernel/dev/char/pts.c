/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * pts.c - ptsfs (PTY slave filesystem) for /dev/pts/N nodes.
 */

#include <stdint.h>
#include <stddef.h>
#include "fs/vfs/vfs.h"
#include "dev/char/pty.h"

extern struct pty_pair pty_pairs[PTY_MAX_PAIRS];

static struct vfs_node ptsfs_root;

static void kstrncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
}

static int parse_num(const char *name) {
    int num = 0;
    if (!name || !*name) return -1;
    for (int i = 0; name[i]; i++) {
        if (name[i] < '0' || name[i] > '9') return -1;
        num = num * 10 + (name[i] - '0');
    }
    return num;
}

static struct vfs_node *ptsfs_finddir(struct vfs_node *node, const char *name) {
    (void)node;
    int num = parse_num(name);
    if (num < 0) return NULL;
    struct pty_pair *p = pty_get((uint32_t)num);
    if (!p) return NULL;
    return &p->slave_node;
}

static struct vfs_dirent *ptsfs_readdir(struct vfs_node *node, uint32_t index) {
    (void)node;
    static struct vfs_dirent dent;
    uint32_t found = 0;
    for (int i = 0; i < PTY_MAX_NUM; i++) {
        if (pty_pairs[i].used) {
            if (found == index) {
                kstrncpy(dent.name, pty_pairs[i].slave_node.name + 4, sizeof(dent.name));
                dent.ino = pty_pairs[i].slave_node.inode;
                dent.type = VFS_CHARDEVICE;
                dent.size = 0;
                return &dent;
            }
            found++;
        }
    }
    return NULL;
}

struct vfs_node *ptsfs_mount(void) {
    kstrncpy(ptsfs_root.name, "pts", sizeof(ptsfs_root.name));
    ptsfs_root.type = VFS_DIRECTORY;
    ptsfs_root.permissions = 0755;
    ptsfs_root.inode = 3;
    ptsfs_root.finddir = ptsfs_finddir;
    ptsfs_root.readdir = ptsfs_readdir;
    return &ptsfs_root;
}
