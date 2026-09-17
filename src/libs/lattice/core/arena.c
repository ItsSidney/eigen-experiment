/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * lattice_arena.c - bump allocator implementation. No libc calls; the
 * alignment loop is plain arithmetic so gcc cannot rewrite it into a
 * memset/memcpy idiom (kernel objects build with -fno-builtin anyway).
 */

#include "core/arena.h"

void lat_arena_init(struct lat_arena *a, void *buf, size_t cap) {
    a->base = (uint8_t *)buf;
    a->cap = cap;
    a->off = 0;
}

void *lat_arena_alloc(struct lat_arena *a, size_t size, size_t align) {
    size_t off = a->off;
    size_t mask;

    if (!a || !a->base)
        return 0;
    if (align == 0)
        align = 1;
    /* Align up without division: callers pass 1/2/4/8. */
    mask = align - 1;
    off = (off + mask) & ~mask;
    if (size > a->cap || off > a->cap - size)
        return 0;
    a->off = off + size;
    return a->base + off;
}

void lat_arena_reset(struct lat_arena *a) {
    if (a)
        a->off = 0;
}

size_t lat_arena_used(const struct lat_arena *a) {
    return a ? a->off : 0;
}

size_t lat_arena_free(const struct lat_arena *a) {
    if (!a || a->off >= a->cap)
        return 0;
    return a->cap - a->off;
}
