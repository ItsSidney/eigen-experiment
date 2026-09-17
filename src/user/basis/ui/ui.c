/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * ui.c - basisui chrome. Userspace (musl OK).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "data.h"

int state_fg(const char *state) {
    if (!state)
        return TH_FG;
    if (strcmp(state, "online") == 0)
        return TH_OK;
    if (strcmp(state, "starting") == 0 || strcmp(state, "restarting") == 0)
        return 4;
    if (strcmp(state, "offline") == 0)
        return TH_HOT;
    if (strcmp(state, "maintenance") == 0)
        return TH_WARN;
    return TH_FG; /* disabled / uninit: plain, dimmed by caller */
}

void ui_text(struct app *a, int layer, int lx, int ly, const char *str,
             int fg, int bg, int attr, int maxlen) {
    lat_layer_text(&a->comp, layer, (uint32_t)(lx < 0 ? 0 : lx),
                   (uint32_t)(ly < 0 ? 0 : ly), str, (uint8_t)fg,
                   (uint8_t)bg, (uint8_t)attr, (uint32_t)maxlen);
}

void ui_put(struct app *a, int layer, int lx, int ly, char ch, int fg,
            int bg, int attr) {
    if (lx < 0 || ly < 0)
        return;
    lat_layer_put(&a->comp, layer, (uint32_t)lx, (uint32_t)ly,
                  (uint8_t)ch, (uint8_t)fg, (uint8_t)bg, (uint8_t)attr);
}

void ui_fill(struct app *a, int layer, int lx, int ly, int w, int h, char ch,
             int fg, int bg, int attr) {
    struct lat_layer *l;
    int x;
    int y;
    if (layer < 0 || layer >= LAT_MAX_LAYERS)
        return;
    l = &a->comp.layers[layer];
    if (!l->used)
        return;
    for (y = ly; y < ly + h; y++) {
        for (x = lx; x < lx + w; x++) {
            if (x < 0 || y < 0 || x >= (int)l->w || y >= (int)l->h)
                continue;
            ui_put(a, layer, x, y, ch, fg, bg, attr);
        }
    }
}

static void box_geom(struct app *a, int *x0, int *y0, int *bw, int *bh) {
    int cols = a->term.cols;
    int rows = a->term.rows;
    int w = cols - 4;
    int h = rows - 2;
    if (w > 78)
        w = 78;
    if (h > 30)
        h = 30;
    if (w < 60)
        w = cols - 2;
    if (h < 18)
        h = rows - 1;
    *x0 = (cols - w) / 2;
    *y0 = (rows - h) / 2;
    if (*x0 < 0)
        *x0 = 0;
    if (*y0 < 0)
        *y0 = 0;
    *bw = w;
    *bh = h;
}

int ui_setup(struct app *a) {
    int cols = a->term.cols;
    int rows = a->term.rows;
    size_t n = (size_t)cols * (size_t)rows;
    int x0, y0, bw, bh;
    int bw2, bh2;
    if (cols < 80)
        cols = 80;
    if (rows < 24)
        rows = 24;
    a->front = malloc(sizeof(struct lat_cell) * n);
    a->back = malloc(sizeof(struct lat_cell) * n);
    a->bgcells = malloc(sizeof(struct lat_cell) * n);
    a->modalcells = malloc(sizeof(struct lat_cell) * n);
    box_geom(a, &x0, &y0, &bw, &bh);
    /* content = white interior below the breadcrumb row */
    a->body_x = x0 + 2;
    a->body_y = y0 + 5;
    a->body_w = bw - 4;
    a->body_h = bh - 8;
    if (a->body_w < 20 || a->body_h < 8)
        return -1;
    bw2 = a->body_w;
    bh2 = a->body_h;
    a->bodycells =
        malloc(sizeof(struct lat_cell) * (size_t)bw2 * (size_t)bh2);
    if (!a->front || !a->back || !a->bgcells || !a->modalcells ||
        !a->bodycells) {
        free(a->front);
        free(a->back);
        free(a->bgcells);
        free(a->modalcells);
        free(a->bodycells);
        return -1;
    }
    lat_surface_init(&a->surf, (uint32_t)cols, (uint32_t)rows, a->front,
                     a->back, &a->term.be);
    lat_comp_init(&a->comp, &a->surf);
    a->lay_bg = lat_layer_create(&a->comp, 0, 0, (uint32_t)cols,
                                 (uint32_t)rows, a->bgcells, 0);
    a->lay_body = lat_layer_create(&a->comp, (uint32_t)a->body_x,
                                   (uint32_t)a->body_y, (uint32_t)bw2,
                                   (uint32_t)bh2, a->bodycells, 5);
    a->lay_modal = lat_layer_create(&a->comp, 0, 0, (uint32_t)cols,
                                    (uint32_t)rows, a->modalcells, 20);
    if (a->lay_bg < 0 || a->lay_body < 0 || a->lay_modal < 0)
        return -1;
    lat_layer_set_dim_behind(&a->comp, a->lay_modal, 1);
    lat_layer_set_visible(&a->comp, a->lay_modal, 0);
    lat_table_init(&a->table, a->rows, SVC_MAX, a->subs, 4);
    return 0;
}

void ui_teardown(struct app *a) {
    free(a->front);
    free(a->back);
    free(a->bgcells);
    free(a->bodycells);
    free(a->modalcells);
}

/* panel rows: 0 top pad, 1 title, 2 telemetry, 3 breadcrumb, 4 rule,
 * content, double rule, footer strip. content_h = bh - 8. */
static void draw_chrome(struct app *a) {
    int cols = a->term.cols;
    int rows = a->term.rows;
    int x0, y0, bw, bh;
    int x;
    int i;
    int fy;
    const char *hint = "";
    int online = 0;
    int r;
    for (r = 0; r < a->table.maxrows; r++) {
        if (!a->table.rows[r].used)
            continue;
        {
            char st[16];
            unpack_svc(&a->table.rows[r], st, NULL, NULL, NULL, NULL,
                       0);
            if (strcmp(st, "online") == 0)
                online++;
        }
    }
    box_geom(a, &x0, &y0, &bw, &bh);
    /* blue screen, floating white panel, dithered shadow */
    ui_fill(a, a->lay_bg, 0, 0, cols, rows, ' ', 7, 4, 0);
    ui_fill(a, a->lay_bg, x0, y0, bw, bh, ' ', TH_FG, TH_BG, 0);
    for (i = 1; i <= bh; i++)
        ui_put(a, a->lay_bg, x0 + bw, y0 + i, 0xB1, 0, 4, 0);
    for (x = 1; x <= bw; x++)
        ui_put(a, a->lay_bg, x0 + x, y0 + bh, 0xB1, 0, 4, 0);
    /* title + online/total tag */
    ui_text(a, a->lay_bg, x0 + 2, y0 + 1, "Basis Services", TH_FG, TH_BG,
            LAT_ATTR_BOLD, bw - 20);
    {
        char tag[32];
        int tlen2;
        snprintf(tag, sizeof tag, "%d/%d online", online, a->nsvc);
        tlen2 = (int)strlen(tag);
        for (i = 0; i < tlen2; i++)
            ui_put(a, a->lay_bg, x0 + bw - 2 - tlen2 + i, y0 + 1, tag[i],
                   online == a->nsvc ? TH_OK : TH_WARN, TH_BG,
                   LAT_ATTR_BOLD);
    }
    /* hairline rule */
    for (x = 0; x < bw; x++)
        ui_put(a, a->lay_bg, x0 + x, y0 + 2, 0xC4, 4, TH_BG, 0);
    /* breadcrumb row */
    ui_text(a, a->lay_bg, x0 + 2, y0 + 3, "SERVICES", 4, TH_BG,
            LAT_ATTR_BOLD, bw - 4);
    /* footer: blue action strip, panel last row */
    fy = y0 + bh - 1;
    if (a->modal == BM_TEXT)
        hint = "\x18\x19 Scroll  PgUp/PgDn  Esc Close";
    else if (a->modal == BM_CONFIRM)
        hint = "Enter Confirm  Esc Cancel  Tab Switch";
    else if (a->modal == BM_HELP)
        hint = "Enter/Esc Close";
    else
        hint = "\x18\x19 Sel  Enter Detail  L Log  D Deps  E En  X Dis  "
               "C Clr  V Val  ? Help  Q Quit";
    for (x = 0; x < bw; x++)
        ui_put(a, a->lay_bg, x0 + x, fy, ' ', 7, 4, 0);
    ui_text(a, a->lay_bg, x0 + 2, fy, hint, 7, 4, 0, bw - 4);
}

/* ── modal frame ────────────────────────────────────────────────────── */

void ui_modal_frame(struct app *a, const char *title, int w, int h,
                    int *ix, int *iy, int *iw, int *ih) {
    int cols = a->term.cols;
    int rows = a->term.rows;
    int x0 = (cols - w) / 2;
    int y0 = (rows - h) / 2;
    int x;
    int y;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    lat_layer_clear(&a->comp, a->lay_modal);
    ui_fill(a, a->lay_modal, x0, y0, w, h, ' ', 0, 7, 0);
    for (y = 1; y <= h; y++)
        ui_put(a, a->lay_modal, x0 + w, y0 + y, 0xB1, 0, 0, 0);
    for (x = 1; x <= w; x++)
        ui_put(a, a->lay_modal, x0 + x, y0 + h, 0xB1, 0, 0, 0);
    ui_put(a, a->lay_modal, x0, y0, 0xDA, 0, 7, 0);
    ui_put(a, a->lay_modal, x0 + w - 1, y0, 0xBF, 0, 7, 0);
    ui_put(a, a->lay_modal, x0, y0 + h - 1, 0xC0, 0, 7, 0);
    ui_put(a, a->lay_modal, x0 + w - 1, y0 + h - 1, 0xD9, 0, 7, 0);
    for (x = 1; x < w - 1; x++) {
        ui_put(a, a->lay_modal, x0 + x, y0, 0xC4, 0, 7, 0);
        ui_put(a, a->lay_modal, x0 + x, y0 + h - 1, 0xC4, 0, 7, 0);
    }
    for (y = 1; y < h - 1; y++) {
        ui_put(a, a->lay_modal, x0, y0 + y, 0xB3, 0, 7, 0);
        ui_put(a, a->lay_modal, x0 + w - 1, y0 + y, 0xB3, 0, 7, 0);
    }
    if (title)
        ui_text(a, a->lay_modal, x0 + 2, y0, title, 0, 7, LAT_ATTR_BOLD,
                w - 4);
    *ix = x0 + 2;
    *iy = y0 + 1;
    *iw = w - 4;
    *ih = h - 2;
}

void ui_button(struct app *a, int layer, int cx, int ly, const char *label,
               int focused) {
    char buf[48];
    int len;
    int x;
    int i;
    snprintf(buf, sizeof buf, "<%s>", label);
    len = (int)strlen(buf);
    x = cx - len / 2;
    for (i = 0; i < len; i++) {
        if (focused)
            ui_put(a, layer, x + i, ly, buf[i], 7, 4, LAT_ATTR_BOLD);
        else
            ui_put(a, layer, x + i, ly, buf[i], 0, 7, 0);
    }
}

/* ── frame ──────────────────────────────────────────────────────────── */

void ui_draw(struct app *a) {
    draw_chrome(a);
    ui_fill(a, a->lay_body, 0, 0, a->body_w, a->body_h, ' ', TH_FG, TH_BG,
            0);
    draw_screen(a);
    if (a->modal != BM_NONE)
        draw_modal(a);
    else
        lat_layer_clear(&a->comp, a->lay_modal);
}

void ui_present(struct app *a) {
    lat_layer_set_visible(&a->comp, a->lay_modal,
                          a->modal != BM_NONE ? 1 : 0);
    ui_draw(a);
    lat_comp_present(&a->comp);
}
