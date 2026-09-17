/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * lattice_surface.c - L0 cell-surface implementation. Alloc-free, no libc.
 */

#include "l0/surface.h"

static struct lat_cell make_cell(uint8_t ch, uint8_t fg, uint8_t bg,
                                 uint8_t attr) {
    struct lat_cell c;
    c.ch = ch;
    c.fg = fg & 15;
    c.bg = bg & 15;
    c.attr = attr;
    return c;
}

void lat_surface_init(struct lat_surface *s, uint32_t cols, uint32_t rows,
                      struct lat_cell *front, struct lat_cell *back,
                      const struct lat_backend *be) {
    uint32_t n;
    uint32_t i;

    s->cols = cols;
    s->rows = rows;
    s->front = front;
    s->back = back;
    s->be = be;
    s->flushes = 0;
    s->cells_flushed = 0;
    n = cols * rows;
    for (i = 0; i < n; i++) {
        front[i] = make_cell(' ', 7, 0, 0);
        back[i] = make_cell(' ', 7, 0, 0);
    }
}

void lat_surface_put(struct lat_surface *s, uint32_t x, uint32_t y,
                     uint8_t ch, uint8_t fg, uint8_t bg, uint8_t attr) {
    if (!s || x >= s->cols || y >= s->rows)
        return;
    s->back[y * s->cols + x] = make_cell(ch, fg, bg, attr);
}

void lat_surface_text(struct lat_surface *s, uint32_t x, uint32_t y,
                      const char *str, uint8_t fg, uint8_t bg, uint8_t attr,
                      uint32_t maxlen) {
    uint32_t i;

    if (!s || !str || y >= s->rows)
        return;
    for (i = 0; i < maxlen && str[i]; i++) {
        if (x + i >= s->cols)
            break;
        s->back[y * s->cols + x + i] = make_cell((uint8_t)str[i], fg, bg, attr);
    }
}

void lat_surface_fill(struct lat_surface *s, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h,
                      uint8_t ch, uint8_t fg, uint8_t bg, uint8_t attr) {
    uint32_t dy;
    uint32_t dx;

    if (!s)
        return;
    for (dy = 0; dy < h; dy++) {
        if (y + dy >= s->rows)
            break;
        for (dx = 0; dx < w; dx++) {
            if (x + dx >= s->cols)
                break;
            s->back[(y + dy) * s->cols + x + dx] =
                make_cell(ch, fg, bg, attr);
        }
    }
}

void lat_surface_clear(struct lat_surface *s,
                       uint8_t ch, uint8_t fg, uint8_t bg, uint8_t attr) {
    if (!s)
        return;
    lat_surface_fill(s, 0, 0, s->cols, s->rows, ch, fg, bg, attr);
}

static int cell_same(const struct lat_cell *a, const struct lat_cell *b) {
    return a->ch == b->ch && a->fg == b->fg && a->bg == b->bg &&
           a->attr == b->attr;
}

uint32_t lat_surface_flush(struct lat_surface *s) {
    uint32_t changed = 0;
    uint32_t y;
    uint32_t x;

    if (!s)
        return 0;
    for (y = 0; y < s->rows; y++) {
        for (x = 0; x < s->cols; x++) {
            struct lat_cell *f = &s->front[y * s->cols + x];
            struct lat_cell *b = &s->back[y * s->cols + x];
            if (cell_same(f, b))
                continue;
            /* Transparent staging cells are compositor-internal: they
             * mean "nothing here", so the backend keeps showing front. */
            if (b->ch == 0)
                continue;
            *f = *b;
            if (s->be && s->be->present)
                s->be->present(s->be->ctx, x, y, b);
            changed++;
        }
    }
    s->flushes++;
    s->cells_flushed += changed;
    if (s->be && s->be->flush)
        s->be->flush(s->be->ctx);
    return changed;
}

void lat_surface_dump_text(const struct lat_surface *s,
                           void (*putc)(void *ctx, char c), void *ctx) {
    uint32_t y;
    uint32_t x;

    if (!s || !putc)
        return;
    for (y = 0; y < s->rows; y++) {
        int last = -1;
        for (x = 0; x < s->cols; x++) {
            uint8_t ch = s->back[y * s->cols + x].ch;
            if (ch != 0 && ch != ' ')
                last = (int)x;
        }
        for (x = 0; x < s->cols && (int)x <= last; x++) {
            uint8_t ch = s->back[y * s->cols + x].ch;
            putc(ctx, ch == 0 ? ' ' : (char)ch);
        }
        putc(ctx, '\n');
    }
}
