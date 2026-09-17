/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * lattice_compositor.c - L1 blend + present. Alloc-free, no libc.
 */

#include "l1/compositor.h"

void lat_comp_init(struct lat_comp *c, struct lat_surface *surf) {
    int i;

    c->surf = surf;
    c->frames = 0;
    for (i = 0; i < LAT_MAX_LAYERS; i++) {
        c->layers[i].used = 0;
        c->layers[i].visible = 0;
        c->layers[i].z = 0;
        c->layers[i].x = 0;
        c->layers[i].y = 0;
        c->layers[i].w = 0;
        c->layers[i].h = 0;
        c->layers[i].cells = 0;
        c->layers[i].dim_behind = 0;
    }
}

int lat_layer_create(struct lat_comp *c, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h, struct lat_cell *cells, int z) {
    int i;
    uint32_t n;
    uint32_t k;

    if (!c || !cells || w == 0 || h == 0)
        return -1;
    for (i = 0; i < LAT_MAX_LAYERS; i++) {
        if (!c->layers[i].used) {
            c->layers[i].used = 1;
            c->layers[i].visible = 1;
            c->layers[i].z = z;
            c->layers[i].x = x;
            c->layers[i].y = y;
            c->layers[i].w = w;
            c->layers[i].h = h;
            c->layers[i].cells = cells;
            c->layers[i].dim_behind = 0;
            n = w * h;
            for (k = 0; k < n; k++) {
                cells[k].ch = 0;
                cells[k].fg = 7;
                cells[k].bg = 0;
                cells[k].attr = 0;
            }
            return i;
        }
    }
    return -1;
}

void lat_layer_destroy(struct lat_comp *c, int id) {
    if (!c || id < 0 || id >= LAT_MAX_LAYERS)
        return;
    c->layers[id].used = 0;
    c->layers[id].visible = 0;
    c->layers[id].cells = 0;
}

static struct lat_layer *get_layer(struct lat_comp *c, int id) {
    if (!c || id < 0 || id >= LAT_MAX_LAYERS)
        return 0;
    if (!c->layers[id].used)
        return 0;
    return &c->layers[id];
}

void lat_layer_set_visible(struct lat_comp *c, int id, int visible) {
    struct lat_layer *l = get_layer(c, id);

    if (l)
        l->visible = visible ? 1 : 0;
}

void lat_layer_set_z(struct lat_comp *c, int id, int z) {
    struct lat_layer *l = get_layer(c, id);

    if (l)
        l->z = z;
}

void lat_layer_set_dim_behind(struct lat_comp *c, int id, int dim) {
    struct lat_layer *l = get_layer(c, id);

    if (l)
        l->dim_behind = dim ? 1 : 0;
}

void lat_layer_put(struct lat_comp *c, int id, uint32_t lx, uint32_t ly,
                   uint8_t ch, uint8_t fg, uint8_t bg, uint8_t attr) {
    struct lat_layer *l = get_layer(c, id);
    struct lat_cell *cell;

    if (!l || lx >= l->w || ly >= l->h)
        return;
    cell = &l->cells[ly * l->w + lx];
    cell->ch = ch;
    cell->fg = fg & 15;
    cell->bg = bg & 15;
    cell->attr = attr;
}

void lat_layer_text(struct lat_comp *c, int id, uint32_t lx, uint32_t ly,
                    const char *str, uint8_t fg, uint8_t bg, uint8_t attr,
                    uint32_t maxlen) {
    uint32_t i;

    if (!str)
        return;
    for (i = 0; i < maxlen && str[i]; i++)
        lat_layer_put(c, id, lx + i, ly, (uint8_t)str[i], fg, bg, attr);
}

void lat_layer_fill(struct lat_comp *c, int id, uint32_t lx, uint32_t ly,
                    uint32_t w, uint32_t h,
                    uint8_t ch, uint8_t fg, uint8_t bg, uint8_t attr) {
    uint32_t dy;
    uint32_t dx;

    for (dy = 0; dy < h; dy++) {
        for (dx = 0; dx < w; dx++)
            lat_layer_put(c, id, lx + dx, ly + dy, ch, fg, bg, attr);
    }
}

void lat_layer_clear(struct lat_comp *c, int id) {
    struct lat_layer *l = get_layer(c, id);
    uint32_t n;
    uint32_t k;

    if (!l)
        return;
    n = l->w * l->h;
    for (k = 0; k < n; k++) {
        l->cells[k].ch = 0;
        l->cells[k].fg = 7;
        l->cells[k].bg = 0;
        l->cells[k].attr = 0;
    }
}

/* Blend one surface cell: topmost opaque layer cell wins; any higher
 * dim_behind layer covering the cell dims what shows through. */
static struct lat_cell blend_cell(struct lat_comp *c, uint32_t sx,
                                  uint32_t sy) {
    struct lat_cell out;
    int best = -1;
    int best_z = -0x7fffffff;
    int i;
    int dim = 0;

    out.ch = ' ';
    out.fg = 7;
    out.bg = 0;
    out.attr = 0;

    for (i = 0; i < LAT_MAX_LAYERS; i++) {
        struct lat_layer *l = &c->layers[i];
        uint32_t lx;
        uint32_t ly;
        struct lat_cell *cell;

        if (!l->used || !l->visible)
            continue;
        if (sx < l->x || sy < l->y)
            continue;
        lx = sx - l->x;
        ly = sy - l->y;
        if (lx >= l->w || ly >= l->h)
            continue;
        cell = &l->cells[ly * l->w + lx];
        if (cell->ch == 0)
            continue;
        if (l->z > best_z) {
            best_z = l->z;
            best = i;
        }
    }
    if (best >= 0)
        out = c->layers[best].cells[(sy - c->layers[best].y) *
                                        c->layers[best].w +
                                    (sx - c->layers[best].x)];
    /* A dim_behind layer above the winner dims the region behind it. */
    for (i = 0; i < LAT_MAX_LAYERS; i++) {
        struct lat_layer *l = &c->layers[i];

        if (!l->used || !l->visible || !l->dim_behind)
            continue;
        if (i == best)
            continue;
        if (l->z <= best_z)
            continue;
        if (sx < l->x || sy < l->y)
            continue;
        if (sx - l->x >= l->w || sy - l->y >= l->h)
            continue;
        dim = 1;
        break;
    }
    if (dim)
        out.attr |= LAT_ATTR_DIM;
    return out;
}

uint32_t lat_comp_present(struct lat_comp *c) {
    uint32_t y;
    uint32_t x;

    if (!c || !c->surf)
        return 0;
    for (y = 0; y < c->surf->rows; y++) {
        for (x = 0; x < c->surf->cols; x++) {
            struct lat_cell cell = blend_cell(c, x, y);
            c->surf->back[y * c->surf->cols + x] = cell;
        }
    }
    c->frames++;
    return lat_surface_flush(c->surf);
}
