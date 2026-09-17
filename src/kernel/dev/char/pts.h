/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * pts.c - ptsfs (PTY slave filesystem) for /dev/pts/N nodes.
 */

#ifndef KERNEL_PTS_H
#define KERNEL_PTS_H

#include "fs/vfs/vfs.h"

/*
 * Mount the PTY slave filesystem. Call this after devfs is mounted
 * at /dev, so /dev/pts/ is accessible.
 */
struct vfs_node *ptsfs_mount(void);

#endif
