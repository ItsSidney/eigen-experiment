/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * lattice_arena.h - bump allocator for arena-only (no-heap) mode.
 *
 * The Lattice core never allocates by itself: every init function takes
 * caller-owned storage. This arena turns one static buffer into that
 * storage, so the recovery console (bare metal, heap not yet up) can use
 * the exact same L0/L1/L2 code as heap-backed userspace tools.
 *
 * Freestanding: only <stdint.h>/<stddef.h>. No libc, no heap.
 */

#ifndef LATTICE_ARENA_H
#define LATTICE_ARENA_H

#include <stdint.h>
#include <stddef.h>

struct lat_arena {
    uint8_t *base;
    size_t cap;
    size_t off;
};

void lat_arena_init(struct lat_arena *a, void *buf, size_t cap);
void *lat_arena_alloc(struct lat_arena *a, size_t size, size_t align);
void lat_arena_reset(struct lat_arena *a);
size_t lat_arena_used(const struct lat_arena *a);
size_t lat_arena_free(const struct lat_arena *a);

#endif /* LATTICE_ARENA_H */
