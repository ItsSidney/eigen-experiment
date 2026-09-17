/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * shmem.h - anonymous shared-memory objects behind memfd_create.
 */

#ifndef KERNEL_SHMEM_H
#define KERNEL_SHMEM_H

#include <stdint.h>

/* Create an anonymous shmem object of `size` bytes, return its fd
 * (negative errno on failure). */
int shmem_create(const char *name, uint64_t size);

#endif /* KERNEL_SHMEM_H */
