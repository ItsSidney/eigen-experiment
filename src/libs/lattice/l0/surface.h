/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * lattice_surface.h - L0 cell-surface abstraction + backend vtable.
 *
 * One grid of cells, three interchangeable output backends:
 *   1. framebuffer (kernel): present() -> console_put_cell()
 *   2. terminal (userspace): present() buffers, flush() emits ANSI
 *   3. headless/serial:      null present(); lat_surface_dump_text()
 *      streams rows through a putc callback (serial log, tests).
 *
 * The core owns no memory: front/back cell grids are caller-provided
 * (arena slice or heap block, cols*rows cells each). Flushing diffs back
 * against front so only changed cells reach the backend (L1 relies on
 * this for its dirty-rect behaviour).
 *
 * Colours are 0-15 (VGA/SGR indices, shared with the kernel console
 * palette). ch is a CP437 glyph code; ch==0 is transparent and is never
 * presented (the compositor uses it for see-through layer cells).
 *
 * Freestanding: only <stdint.h>/<stddef.h>/<stdbool.h>.
 */

#ifndef LATTICE_SURFACE_H
#define LATTICE_SURFACE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Cell attribute bits. */
#define LAT_ATTR_REVERSE 0x01
#define LAT_ATTR_DIM     0x02
#define LAT_ATTR_BOLD    0x04

struct lat_cell {
    uint8_t ch;
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;
};

struct lat_backend {
    void *ctx;
    void (*present)(void *ctx, uint32_t x, uint32_t y,
                    const struct lat_cell *cell);
    void (*flush)(void *ctx);
    void (*cursor)(void *ctx, uint32_t x, uint32_t y, int visible);
};

struct lat_surface {
    uint32_t cols;
    uint32_t rows;
    struct lat_cell *front; /* last flushed state */
    struct lat_cell *back;  /* staging state */
    const struct lat_backend *be;
    uint64_t flushes;
    uint64_t cells_flushed; /* lifetime changed-cell count */
};

void lat_surface_init(struct lat_surface *s, uint32_t cols, uint32_t rows,
                      struct lat_cell *front, struct lat_cell *back,
                      const struct lat_backend *be);
void lat_surface_put(struct lat_surface *s, uint32_t x, uint32_t y,
                     uint8_t ch, uint8_t fg, uint8_t bg, uint8_t attr);
void lat_surface_text(struct lat_surface *s, uint32_t x, uint32_t y,
                      const char *str, uint8_t fg, uint8_t bg, uint8_t attr,
                      uint32_t maxlen);
void lat_surface_fill(struct lat_surface *s, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h,
                      uint8_t ch, uint8_t fg, uint8_t bg, uint8_t attr);
void lat_surface_clear(struct lat_surface *s,
                       uint8_t ch, uint8_t fg, uint8_t bg, uint8_t attr);

/* Diff back against front, present changed cells, swap state.
 * Returns the number of changed cells. Always calls be->flush. */
uint32_t lat_surface_flush(struct lat_surface *s);

/* Write the staged (back) grid as plain text rows via putc, one '\n'
 * per row, trailing blanks trimmed. The serial/headless backend. */
void lat_surface_dump_text(const struct lat_surface *s,
                           void (*putc)(void *ctx, char c), void *ctx);

#endif /* LATTICE_SURFACE_H */
