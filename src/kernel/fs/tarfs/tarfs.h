/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * tarfs.h - USTAR In-Memory Initramfs driver.
 */

#ifndef KERNEL_TARFS_H
#define KERNEL_TARFS_H

#include <stdint.h>
#include <stddef.h>
#include "fs/vfs/vfs.h"

struct vfs_node *tarfs_init(void *tar_archive, size_t archive_size);

#endif /* KERNEL_TARFS_H */
