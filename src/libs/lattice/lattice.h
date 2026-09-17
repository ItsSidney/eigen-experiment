/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * lattice.h - Lattice TUI toolkit, layers L0-L2 (this step).
 *
 * Layout (one folder per layer; L3-L6 get their own as they land):
 *   core/        arena bump allocator (no-heap mode primitive)
 *   l0/          surface: cell grid + backend vtable (framebuffer /
 *                terminal / headless-serial). Caller-owned cell storage.
 *   l1/          compositor: layer stack with z-order, transparency
 *                (ch==0) and dim-behind modals. Blend + diff-flush.
 *   l2/          reactive: Signal / Stream / Table pub/sub. Widgets hold
 *                handles plus generation counters, never copies. Same
 *                code runs heap-backed and arena-only (core never
 *                allocates).
 *   backends/    platform terminal glue (userspace; kernel framebuffer
 *                glue lives in src/kernel/tui/). Reserved for L3 work.
 *   tests/       host proof harness (stdio/assert, never in kernel build).
 *
 * Include as <lattice/lattice.h> with -Isrc/libs on the command line.
 * The library itself is freestanding (stdint/stddef/stdbool only) and
 * builds for both the kernel (-ffreestanding -fno-builtin) and the
 * host test harness. Platform adapters live outside this directory:
 * kernel framebuffer glue in src/kernel/tui/lattice_demo.c,
 * userspace terminal glue in future L3 work (the harness carries a
 * minimal ANSI emitter for proof).
 */

#ifndef LATTICE_H
#define LATTICE_H

#include "core/arena.h"
#include "l0/surface.h"
#include "l2/reactive.h"
#include "l1/compositor.h"

#endif /* LATTICE_H */
