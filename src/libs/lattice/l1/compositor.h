/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * lattice_compositor.h - L1 layer-aware compositor.
 *
 * Panes are layers with a z-order stacked over one L0 surface. Each
 * layer owns a caller-provided cell grid (w*h); ch==0 cells are
 * transparent and let lower layers show through. Presenting blends
 * top-down into the surface staging buffer and flushes, so only the
 * diff reaches the backend (dirty-rect behaviour at cell granularity).
 *
 * Modal signature: a layer with dim_behind set dims the region behind
 * it instead of blanking it. Cells the modal leaves transparent show
 * the layers below with LAT_ATTR_DIM added, so a confirm dialog floats
 * over a live dashboard that keeps updating underneath (structurally
 * impossible in whiptail's full-screen takeover).
 *
 * Alloc-free like the rest of the core: layer grids come from the
 * caller (arena or heap). Fixed cap LAT_MAX_LAYERS keeps the struct
 * static-sized for bare-metal use.
 *
 * Freestanding: only <stdint.h>/<stddef.h>.
 */

#ifndef LATTICE_COMPOSITOR_H
#define LATTICE_COMPOSITOR_H

#include <stdint.h>
#include <stddef.h>

#include "l0/surface.h"

#define LAT_MAX_LAYERS 8

struct lat_layer {
    int used;
    int visible;
    int z;
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
    struct lat_cell *cells;
    int dim_behind;
};

struct lat_comp {
    struct lat_surface *surf;
    struct lat_layer layers[LAT_MAX_LAYERS];
    uint64_t frames;
};

void lat_comp_init(struct lat_comp *c, struct lat_surface *surf);
/* Returns layer id (0..LAT_MAX_LAYERS-1) or -1 when full. */
int lat_layer_create(struct lat_comp *c, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h, struct lat_cell *cells, int z);
void lat_layer_destroy(struct lat_comp *c, int id);
void lat_layer_set_visible(struct lat_comp *c, int id, int visible);
void lat_layer_set_z(struct lat_comp *c, int id, int z);
void lat_layer_set_dim_behind(struct lat_comp *c, int id, int dim);

void lat_layer_put(struct lat_comp *c, int id, uint32_t lx, uint32_t ly,
                   uint8_t ch, uint8_t fg, uint8_t bg, uint8_t attr);
void lat_layer_text(struct lat_comp *c, int id, uint32_t lx, uint32_t ly,
                    const char *str, uint8_t fg, uint8_t bg, uint8_t attr,
                    uint32_t maxlen);
void lat_layer_fill(struct lat_comp *c, int id, uint32_t lx, uint32_t ly,
                    uint32_t w, uint32_t h,
                    uint8_t ch, uint8_t fg, uint8_t bg, uint8_t attr);
/* Transparent clear (ch==0): see-through until drawn. */
void lat_layer_clear(struct lat_comp *c, int id);

/* Blend all visible layers into the surface and flush.
 * Returns changed cells reported by the surface flush. */
uint32_t lat_comp_present(struct lat_comp *c);

#endif /* LATTICE_COMPOSITOR_H */
