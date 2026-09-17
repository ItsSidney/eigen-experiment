/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * vdso.h - Virtual dynamic shared object mapped into every process.
 */

#ifndef KERNEL_VDSO_H
#define KERNEL_VDSO_H

#include <stdint.h>
#include <stddef.h>

/* Fixed user mapping address (above mmap area, below stack/trampoline). */
#define VDSO_BASE 0x00007FFFF7FFD000ULL

void vdso_init(void);
int  vdso_map(void *pml4);

#endif /* KERNEL_VDSO_H */
