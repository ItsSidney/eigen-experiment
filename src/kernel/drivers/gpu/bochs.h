/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * bochs.h - QEMU stdvga / bochs-display driver.
 */

#ifndef KERNEL_DRIVERS_GPU_BOCHS_H
#define KERNEL_DRIVERS_GPU_BOCHS_H

/* Probe for a Bochs-compatible display adapter. Returns 1 when claimed, 0 when
 * the hardware is not one of ours, -1 on failure. */
int bochs_probe(void);

#endif /* KERNEL_DRIVERS_GPU_BOCHS_H */