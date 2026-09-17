/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * ui.c - unified single-box chrome + modal frame. Userspace (musl OK).
 *
 * Every screen shares one centered double-line box: title/health,
 * reason + telemetry, breadcrumb, content region, footer. Detail
 * bodies draw into the body layer (= box interior); the menu rail
 * is gone, digits 1-9 jump from anywhere.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/ui.h"

const struct menu_item menu_items[MENU_N] = {
    { '1', "Kernel Log", SCR_LOG, ACT_NONE },
    { '2', "Registers", SCR_REGS, ACT_NONE },
    { '3', "Exceptions", SCR_EXC, ACT_NONE },
    { '4', "Storage", SCR_STORAGE, ACT_NONE },
    { '5', "PCI / ACPI", SCR_PCI, ACT_NONE },
    { '6', "Hex Viewer", SCR_HEX, ACT_NONE },
    { '7', "Shell", SCR_SHELL, ACT_NONE },
    { '8', "Export Report", NSCR, ACT_EXPORT },
    { '9', "Reboot", NSCR, ACT_REBOOT },
};

const char *screen_name(enum screen s) {
    switch (s) {
    case SCR_HOME: return "HOME";
    case SCR_LOG: return "LOG";
    case SCR_REGS: return "REGISTERS";
    case SCR_EXC: return "EXCEPTIONS";
    case SCR_STORAGE: return "STORAGE";
    case SCR_PCI: return "PCI";
    case SCR_HEX: return "HEX";
    case SCR_SHELL: return "SHELL";
    default: return "?";
    }
    /* NSCR has no name */
}

const char *health_name(int level) {
    if (level >= HEALTH_CRITICAL)
        return "CRITICAL";
    if (level == HEALTH_DEGRADED)
        return "DEGRADED";
    return "OK";
}

int health_fg(int level) {
    if (level >= HEALTH_CRITICAL)
        return 1; /* red */
    if (level == HEALTH_DEGRADED)
        return 3; /* yellow */
    return 2; /* green */
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

/* Centered box geometry shared by setup (content region) and chrome. */
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
    if (h < 20)
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
    /* content = white interior between crumb and footer strip */
    a->body_x = x0 + 2;
    a->body_y = y0 + 6;
    a->body_w = bw - 4;
    a->body_h = bh - 10;
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
    lat_signal_init(&a->health, a->health_buf, sizeof a->health_buf,
                    a->health_subs, 4);
    return 0;
}

void ui_teardown(struct app *a) {
    free(a->front);
    free(a->back);
    free(a->bgcells);
    free(a->bodycells);
    free(a->modalcells);
}

/* ── health: reason at start, live escalation on new exceptions ─────── */

static int reason_level(const char *reason) {
    const char *p;
    if (!reason || !reason[0])
        return HEALTH_OK;
    /* "panic" anywhere (any case) -> critical */
    for (p = reason; *p; p++) {
        if ((p[0] == 'p' || p[0] == 'P') && (p[1] == 'a' || p[1] == 'A') &&
            (p[2] == 'n' || p[2] == 'N') && (p[3] == 'i' || p[3] == 'I') &&
            (p[4] == 'c' || p[4] == 'C'))
            return HEALTH_CRITICAL;
    }
    return HEALTH_DEGRADED;
}

void health_poll(struct app *a) {
    int level = reason_level(a->reason);
    if (a->data.exc_count > a->exc_at_start)
        level = HEALTH_CRITICAL;
    if (level != a->health_level) {
        uint8_t v = (uint8_t)level;
        a->health_level = level;
        lat_signal_set(&a->health, &v, 1);
        a->dirty = 1;
    }
}

/* ── box chrome ─────────────────────────────────────────────────────── */

static void box_hdiv(struct app *a, int x0, int y, int w, int dbl) {
    int i;
    uint8_t l = dbl ? 0xCC : 0xC3;
    uint8_t h = dbl ? 0xCD : 0xC4;
    uint8_t r = dbl ? 0xB9 : 0xB4;
    ui_put(a, a->lay_bg, x0, y, l, 4, TH_BG, 0);
    for (i = 1; i < w - 1; i++)
        ui_put(a, a->lay_bg, x0 + i, y, h, 4, TH_BG, 0);
    ui_put(a, a->lay_bg, x0 + w - 1, y, r, 4, TH_BG, 0);
}

/* streaming indicator for the telemetry row */
static const char *live_state(struct app *a) {
    int held = (a->screen == SCR_LOG && a->log.paused) ||
               (a->screen == SCR_REGS && a->regs.frozen);
    int live_screen = a->screen == SCR_HOME || a->screen == SCR_LOG ||
                      a->screen == SCR_REGS || a->screen == SCR_SHELL;
    if (held || !live_screen)
        return "  HELD";
    return "  LIVE";
}

static void draw_chrome(struct app *a) {
    int cols = a->term.cols;
    int rows = a->term.rows;
    int x0, y0, bw, bh;
    int ch;
    int x, y;
    int i;
    char tag[32];
    char line[160];
    char mems[16];
    char irqs[16];
    char cr[96];
    const char *hint = "";
    int tlen;
    int fy;
    box_geom(a, &x0, &y0, &bw, &bh);
    ch = bh - 10;
    /* blue screen, floating white panel, dithered shadow */
    ui_fill(a, a->lay_bg, 0, 0, cols, rows, ' ', 7, 4, 0);
    ui_fill(a, a->lay_bg, x0, y0, bw, bh, ' ', TH_FG, TH_BG, 0);
    for (y = 1; y <= bh; y++)
        ui_put(a, a->lay_bg, x0 + bw, y0 + y, 0xB1, 0, 4, 0);
    for (x = 1; x <= bw; x++)
        ui_put(a, a->lay_bg, x0 + x, y0 + bh, 0xB1, 0, 4, 0);
    /* title + health tag */
    ui_text(a, a->lay_bg, x0 + 2, y0 + 1,
            "Eigen Recovery", TH_FG, TH_BG, LAT_ATTR_BOLD,
            bw - 20);
    snprintf(tag, sizeof tag, "%c %s", 0x0A,
             health_name(a->health_level));
    tlen = (int)strlen(tag);
    {
        /* dot carries the severity color, the word stays black */
        int dot = a->health_level >= HEALTH_CRITICAL ? 1 :
                  a->health_level == HEALTH_DEGRADED ? 5 : 2;
        ui_put(a, a->lay_bg, x0 + bw - 2 - tlen, y0 + 1, tag[0], dot,
               TH_BG, LAT_ATTR_BOLD);
        ui_text(a, a->lay_bg, x0 + bw - 1 - tlen, y0 + 1, tag + 1, TH_FG,
                TH_BG, LAT_ATTR_BOLD, tlen);
    }
    box_hdiv(a, x0, y0 + 2, bw, 0);
    /* reason + telemetry rows */
    snprintf(line, sizeof line, "REASON  %s",
             a->reason[0] ? a->reason : "manual invocation");
    ui_text(a, a->lay_bg, x0 + 2, y0 + 3, line, TH_FG, TH_BG, 0, bw - 4);
    if (a->data.mem_pct < 0)
        snprintf(mems, sizeof mems, "?");
    else
        snprintf(mems, sizeof mems, "%d%%", a->data.mem_pct);
    if ((long)a->data.irq_rate < 0)
        snprintf(irqs, sizeof irqs, "?/s");
    else
        snprintf(irqs, sizeof irqs, "%lu/s", a->data.irq_rate);
    snprintf(line, sizeof line, "UPTIME  %ss  BUILD  %-18.18s  MEM %s  "
                                "IRQ %s  EXC %lu%s",
             a->data.uptime[0] ? a->data.uptime : "?", a->data.build,
             mems, irqs, a->data.exc_count,
             live_state(a));
    ui_text(a, a->lay_bg, x0 + 2, y0 + 4, line, TH_FG, TH_BG,
            LAT_ATTR_DIM, bw - 4);
    /* breadcrumb row */
    if (a->screen == SCR_HOME)
        snprintf(cr, sizeof cr, "HOME");
    else
        snprintf(cr, sizeof cr, "HOME > %s%s%s", screen_name(a->screen),
                 a->crumb_extra[0] ? " > " : "", a->crumb_extra);
    ui_text(a, a->lay_bg, x0 + 2, y0 + 5, cr, 4, TH_BG, LAT_ATTR_BOLD,
            bw - 4);
    /* double break before the footer */
    box_hdiv(a, x0, y0 + 6 + ch, bw, 1);
    /* footer: blue action strip, panel last row */
    fy = y0 + 7 + ch;
    if (a->modal != M_NONE) {
        switch (a->modal) {
        case M_CONFIRM:
            hint = "Enter Confirm  Esc Cancel  Tab Switch";
            break;
        case M_MESSAGE:
            hint = "Enter/Esc Close";
            break;
        case M_PALETTE:
            hint = "Type Filter  Enter Run  Esc Close";
            break;
        case M_FILTER:
            hint = "Enter Apply  Esc Close (keeps filter)";
            break;
        case M_GOTO:
            hint = "Enter Go  Esc Close";
            break;
        default:
            break;
        }
    } else {
        switch (a->screen) {
        case SCR_HOME:
            hint = "\x18\x19/1-9 Open  Enter Run  / Palette  "
                   "? Help  Esc Quit";
            break;
        case SCR_LOG:
            hint = "\x18\x19 Scroll  Space Pause  / Filter  "
                   "1-9 Jump  Esc Home";
            break;
        case SCR_REGS:
            hint = "\x18\x19 Row  P Pin  F Freeze  1-9 Jump  "
                   "Esc Home";
            break;
        case SCR_EXC:
            hint = "\x18\x19 Select  Enter Detail  1-9 Jump  "
                   "Esc Home";
            break;
        case SCR_STORAGE:
            hint = "\x18\x19 Select  Enter Inspect  F Mount  "
                   "1-9 Jump  Esc Home";
            break;
        case SCR_PCI:
            hint = "\x18\x19 Select  Enter Detail  1-9 Jump  "
                   "Esc Home";
            break;
        case SCR_HEX:
            hint = "PgUp/PgDn Page  G Goto  A Annotate  1-9 Jump  "
                   "Esc Home";
            break;
        case SCR_SHELL:
            hint = "Type away (Tab/Esc Home)  1-9 Jump";
            break;
        default:
            break;
        }
    }
    for (x = 1; x < bw - 1; x++)
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
    /* drop shadow, modern card feel */
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
            ui_put(a, layer, x + i, ly, buf[i], 7, 1, LAT_ATTR_BOLD);
        else
            ui_put(a, layer, x + i, ly, buf[i], 0, 7, 0);
    }
}

/* ── frame ──────────────────────────────────────────────────────────── */

void app_goto(struct app *a, enum screen s) {
    if (a->screen == SCR_SHELL && s != SCR_SHELL)
        shell_free(&a->shell);
    a->screen = s;
    a->crumb_extra[0] = '\0';
    if (s == SCR_SHELL && !a->shell.running)
        shell_spawn(&a->shell);
    a->dirty = 1;
}

void ui_draw(struct app *a) {
    draw_chrome(a);
    ui_fill(a, a->lay_body, 0, 0, a->body_w, a->body_h, ' ', TH_FG, TH_BG,
            0);
    draw_body(a);
    if (a->modal != M_NONE)
        draw_modal(a);
    else
        lat_layer_clear(&a->comp, a->lay_modal);
}

void ui_present(struct app *a) {
    lat_layer_set_visible(&a->comp, a->lay_modal,
                          a->modal != M_NONE ? 1 : 0);
    ui_draw(a);
    lat_comp_present(&a->comp);
}
