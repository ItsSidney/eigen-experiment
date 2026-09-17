/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * dialogs.c - dialog implementations on L0/L1/L2. Userspace (musl OK).
 */

#define _DEFAULT_SOURCE /* strdup */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dialogs.h"

/* Palette roles. */
#define C_SCREEN_BG 4
#define C_SCREEN_FG 7
#define C_DLG_BG 7
#define C_DLG_FG 0
#define C_HILITE_BG 4
#define C_HILITE_FG 7

/* ── screen: surface + background + centered dialog layer ───────────── */

struct screen {
    struct lat_term *t;
    struct lat_surface surf;
    struct lat_comp comp;
    struct lat_cell *front;
    struct lat_cell *back;
    struct lat_cell *bgcells;
    struct lat_cell *dlgcells;
    int bg;
    int dlg;
    int dw;
    int dh;
};

static void screen_bg_draw(struct screen *s, const char *backtitle) {
    struct lat_comp *c = &s->comp;
    lat_layer_fill(c, s->bg, 0, 0, (uint32_t)s->t->cols,
                   (uint32_t)s->t->rows, ' ', C_SCREEN_FG, C_SCREEN_BG, 0);
    if (backtitle && backtitle[0])
        lat_layer_text(c, s->bg, 1, 0, backtitle, C_SCREEN_FG, C_SCREEN_BG,
                       LAT_ATTR_BOLD, (uint32_t)(s->t->cols - 2));
}

/* Center a dw*dh dialog; NULL cells when dw/dh invalid. */
static int screen_setup(struct screen *s, struct lat_term *t,
                        const char *backtitle, int dw, int dh) {
    int cols = t->cols;
    int rows = t->rows;
    int dx;
    int dy;
    if (dw > cols - 2)
        dw = cols - 2;
    if (dh > rows - 2)
        dh = rows - 2;
    if (dw < 10 || dh < 5)
        return -1;
    memset(s, 0, sizeof *s);
    s->t = t;
    s->dw = dw;
    s->dh = dh;
    dx = (cols - dw) / 2;
    dy = (rows - dh) / 2;
    s->front = malloc(sizeof(struct lat_cell) * (size_t)cols * (size_t)rows);
    s->back = malloc(sizeof(struct lat_cell) * (size_t)cols * (size_t)rows);
    s->bgcells = malloc(sizeof(struct lat_cell) * (size_t)cols * (size_t)rows);
    s->dlgcells = malloc(sizeof(struct lat_cell) * (size_t)dw * (size_t)dh);
    if (!s->front || !s->back || !s->bgcells || !s->dlgcells) {
        free(s->front);
        free(s->back);
        free(s->bgcells);
        free(s->dlgcells);
        return -1;
    }
    lat_surface_init(&s->surf, (uint32_t)cols, (uint32_t)rows, s->front,
                     s->back, &t->be);
    lat_comp_init(&s->comp, &s->surf);
    s->bg = lat_layer_create(&s->comp, 0, 0, (uint32_t)cols, (uint32_t)rows,
                             s->bgcells, 0);
    s->dlg = lat_layer_create(&s->comp, (uint32_t)dx, (uint32_t)dy,
                              (uint32_t)dw, (uint32_t)dh, s->dlgcells, 10);
    if (s->bg < 0 || s->dlg < 0) {
        free(s->front);
        free(s->back);
        free(s->bgcells);
        free(s->dlgcells);
        return -1;
    }
    lat_layer_set_dim_behind(&s->comp, s->dlg, 1);
    screen_bg_draw(s, backtitle);
    return 0;
}

static void screen_teardown(struct screen *s) {
    free(s->front);
    free(s->back);
    free(s->bgcells);
    free(s->dlgcells);
}

static void screen_present(struct screen *s) {
    lat_comp_present(&s->comp);
}

/* ── box + text helpers (dialog-local coordinates) ──────────────────── */

static void dlg_fill(struct screen *s) {
    lat_layer_fill(&s->comp, s->dlg, 0, 0, (uint32_t)s->dw, (uint32_t)s->dh,
                   ' ', C_DLG_FG, C_DLG_BG, 0);
}

static void dlg_box(struct screen *s, const char *title) {
    struct lat_comp *c = &s->comp;
    int w = s->dw;
    int h = s->dh;
    int x;
    int y;
    dlg_fill(s);
    lat_layer_put(c, s->dlg, 0, 0, '+', C_DLG_FG, C_DLG_BG, 0);
    lat_layer_put(c, s->dlg, (uint32_t)(w - 1), 0, '+', C_DLG_FG, C_DLG_BG, 0);
    lat_layer_put(c, s->dlg, 0, (uint32_t)(h - 1), '+', C_DLG_FG, C_DLG_BG, 0);
    lat_layer_put(c, s->dlg, (uint32_t)(w - 1), (uint32_t)(h - 1), '+',
                  C_DLG_FG, C_DLG_BG, 0);
    for (x = 1; x < w - 1; x++) {
        lat_layer_put(c, s->dlg, (uint32_t)x, 0, '-', C_DLG_FG, C_DLG_BG, 0);
        lat_layer_put(c, s->dlg, (uint32_t)x, (uint32_t)(h - 1), '-',
                      C_DLG_FG, C_DLG_BG, 0);
    }
    for (y = 1; y < h - 1; y++) {
        lat_layer_put(c, s->dlg, 0, (uint32_t)y, '|', C_DLG_FG, C_DLG_BG, 0);
        lat_layer_put(c, s->dlg, (uint32_t)(w - 1), (uint32_t)y, '|',
                      C_DLG_FG, C_DLG_BG, 0);
    }
    if (title && title[0]) {
        char tbuf[128];
        size_t n = strlen(title);
        if (n > sizeof tbuf - 5)
            n = sizeof tbuf - 5;
        tbuf[0] = ' ';
        memcpy(tbuf + 1, title, n);
        tbuf[n + 1] = ' ';
        tbuf[n + 2] = '\0';
        lat_layer_text(c, s->dlg, 2, 0, tbuf, C_DLG_FG, C_DLG_BG,
                       LAT_ATTR_BOLD, (uint32_t)(w - 4));
    }
}

static void dlg_text(struct screen *s, int x, int y, const char *str,
                     int maxlen) {
    lat_layer_text(&s->comp, s->dlg, (uint32_t)x, (uint32_t)y, str, C_DLG_FG,
                   C_DLG_BG, 0, (uint32_t)maxlen);
}

/* Greedy word wrap; honours \n. Returns malloc'd lines, *nlines set. */
static char **wrap_text(const char *text, int width, int *nlines) {
    int cap = 16;
    int n = 0;
    char **lines = malloc(sizeof(char *) * (size_t)cap);
    const char *p = text;
    if (!lines) {
        *nlines = 0;
        return NULL;
    }
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t plen = eol ? (size_t)(eol - p) : strlen(p);
        size_t off = 0;
        if (plen == 0) {
            /* blank line */
            if (n >= cap) {
                cap *= 2;
                lines = realloc(lines, sizeof(char *) * (size_t)cap);
                if (!lines) {
                    *nlines = 0;
                    return NULL;
                }
            }
            lines[n++] = strdup("");
        }
        while (off < plen) {
            size_t chunk = plen - off;
            size_t cut;
            if ((int)chunk > width) {
                /* look back for a space */
                cut = (size_t)width;
                while (cut > 0 && p[off + cut] != ' ' && p[off + cut] != '\t')
                    cut--;
                if (cut == 0)
                    cut = (size_t)width; /* hard-cut long word */
            } else {
                cut = chunk;
            }
            if (n >= cap) {
                cap *= 2;
                lines = realloc(lines, sizeof(char *) * (size_t)cap);
                if (!lines) {
                    *nlines = 0;
                    return NULL;
                }
            }
            lines[n] = malloc(cut + 1);
            if (!lines[n]) {
                *nlines = 0;
                return NULL;
            }
            memcpy(lines[n], p + off, cut);
            lines[n][cut] = '\0';
            n++;
            off += cut;
            while (off < plen && (p[off] == ' ' || p[off] == '\t'))
                off++;
        }
        if (!eol)
            break;
        p = eol + 1;
    }
    *nlines = n;
    return lines;
}

static void free_lines(char **lines, int n) {
    int i;
    if (!lines)
        return;
    for (i = 0; i < n; i++)
        free(lines[i]);
    free(lines);
}

/* widest line length, capped for measuring */
static int max_line_len(char **lines, int n) {
    int m = 0;
    int i;
    for (i = 0; i < n; i++) {
        int l = (int)strlen(lines[i]);
        if (l > m)
            m = l;
    }
    return m;
}

static void draw_button(struct screen *s, int cx, int y, const char *label,
                        int focused) {
    char buf[64];
    int len;
    int x;
    snprintf(buf, sizeof buf, "[ %s ]", label);
    len = (int)strlen(buf);
    x = cx - len / 2;
    if (focused) {
        /* focused: paint the whole button reversed */
        int i;
        for (i = 0; i < len; i++)
            lat_layer_put(&s->comp, s->dlg, (uint32_t)(x + i), (uint32_t)y,
                          (uint8_t)buf[i], C_HILITE_FG, C_HILITE_BG,
                          LAT_ATTR_BOLD);
    } else {
        lat_layer_text(&s->comp, s->dlg, (uint32_t)x, (uint32_t)y, buf,
                       C_DLG_FG, C_DLG_BG, 0, (uint32_t)(len + 1));
    }
}

static void emit_result(int out_fd, const char *str) {
    if (!str)
        return;
    (void)write(out_fd, str, strlen(str));
    (void)write(out_fd, "\n", 1);
}

/* ── msgbox / infobox ───────────────────────────────────────────────── */

int dlg_msgbox(struct lat_term *t, const char *title, const char *backtitle,
               const char *text, int h, int w, int infobox) {
    struct screen s;
    char **lines;
    int nlines;
    int dw;
    int dh;
    int i;
    struct lat_keyev ev;
    lines = wrap_text(text, t->cols - 10 > 20 ? t->cols - 10 : 20, &nlines);
    if (!lines)
        return DLG_ERROR;
    dw = (w > 0) ? w : max_line_len(lines, nlines) + 6;
    dh = (h > 0) ? h : nlines + 6;
    if (dw > t->cols - 2)
        dw = t->cols - 2;
    if (dh > t->rows - 2)
        dh = t->rows - 2;
    /* re-wrap to the final width */
    free_lines(lines, nlines);
    lines = wrap_text(text, dw - 4, &nlines);
    if (!lines)
        return DLG_ERROR;
    if (screen_setup(&s, t, backtitle, dw, dh) != 0) {
        free_lines(lines, nlines);
        return DLG_ERROR;
    }
    dlg_box(&s, title);
    for (i = 0; i < nlines && i + 2 < dh - 2; i++)
        dlg_text(&s, 2, 2 + i, lines[i], dw - 4);
    if (!infobox)
        draw_button(&s, dw / 2, dh - 2, "OK", 1);
    screen_present(&s);
    free_lines(lines, nlines);
    if (infobox) {
        screen_teardown(&s);
        return DLG_OK;
    }
    for (;;) {
        ev = lat_term_read_key(t);
        if (ev.key == KEY_ENTER || ev.key == KEY_ESC || ev.key == KEY_CHAR ||
            ev.key == KEY_NONE) {
            screen_teardown(&s);
            return (ev.key == KEY_CTRL_C) ? DLG_CANCEL : DLG_OK;
        }
        if (ev.key == KEY_CTRL_C) {
            screen_teardown(&s);
            return DLG_CANCEL;
        }
    }
}

/* ── yesno ──────────────────────────────────────────────────────────── */

int dlg_yesno(struct lat_term *t, const char *title, const char *backtitle,
              const char *text, int h, int w) {
    struct screen s;
    char **lines;
    int nlines;
    int dw;
    int dh;
    int i;
    int focus = 0; /* 0 = Yes, 1 = No */
    lines = wrap_text(text, t->cols - 10 > 20 ? t->cols - 10 : 20, &nlines);
    if (!lines)
        return DLG_ERROR;
    dw = (w > 0) ? w : max_line_len(lines, nlines) + 6;
    dh = (h > 0) ? h : nlines + 7;
    if (dw > t->cols - 2)
        dw = t->cols - 2;
    if (dh > t->rows - 2)
        dh = t->rows - 2;
    free_lines(lines, nlines);
    lines = wrap_text(text, dw - 4, &nlines);
    if (!lines)
        return DLG_ERROR;
    if (screen_setup(&s, t, backtitle, dw, dh) != 0) {
        free_lines(lines, nlines);
        return DLG_ERROR;
    }
    for (;;) {
        struct lat_keyev ev;
        dlg_box(&s, title);
        for (i = 0; i < nlines && i + 2 < dh - 3; i++)
            dlg_text(&s, 2, 2 + i, lines[i], dw - 4);
        draw_button(&s, dw / 2 - 8, dh - 2, "Yes", focus == 0);
        draw_button(&s, dw / 2 + 8, dh - 2, "No", focus == 1);
        screen_present(&s);
        ev = lat_term_read_key(t);
        if (ev.key == KEY_LEFT || ev.key == KEY_RIGHT || ev.key == KEY_TAB)
            focus = 1 - focus;
        else if (ev.key == KEY_ENTER) {
            free_lines(lines, nlines);
            screen_teardown(&s);
            return focus == 0 ? DLG_OK : DLG_CANCEL;
        } else if (ev.key == KEY_CHAR) {
            if (ev.ch == 'y' || ev.ch == 'Y') {
                free_lines(lines, nlines);
                screen_teardown(&s);
                return DLG_OK;
            }
            if (ev.ch == 'n' || ev.ch == 'N') {
                free_lines(lines, nlines);
                screen_teardown(&s);
                return DLG_CANCEL;
            }
        } else if (ev.key == KEY_ESC || ev.key == KEY_CTRL_C ||
                   ev.key == KEY_NONE) {
            free_lines(lines, nlines);
            screen_teardown(&s);
            return DLG_CANCEL;
        }
    }
}

/* ── menu (rows live in an L2 Table) ────────────────────────────────── */

int dlg_menu(struct lat_term *t, const char *title, const char *backtitle,
             const char *text, int h, int w, int menu_h,
             char **tags, char **items, int n, int out_fd) {
    struct screen s;
    char **prompt;
    int nprompt;
    int dw;
    int dh;
    int mh;
    int i;
    int sel = 0;
    int top = 0;
    int m = 0; /* used rows (duplicate tags collapse, so m <= n) */
    struct lat_table table;
    struct lat_row *rows;
    struct lat_sub *subs;
    int list_y;
    int list_h;
    if (n <= 0)
        return DLG_ERROR;
    rows = malloc(sizeof(struct lat_row) * (size_t)n);
    subs = malloc(sizeof(struct lat_sub) * 4);
    if (!rows || !subs) {
        free(rows);
        free(subs);
        return DLG_ERROR;
    }
    lat_table_init(&table, rows, n, subs, 4);
    for (i = 0; i < n; i++) {
        if (lat_table_upsert(&table, tags[i], items[i],
                             strlen(items[i])) != 0) {
            /* tag too long for a key: fall back to positional tag */
            char fb[32];
            snprintf(fb, sizeof fb, "%d", i + 1);
            lat_table_upsert(&table, fb, items[i], strlen(items[i]));
        }
    }
    prompt = wrap_text(text, t->cols - 10 > 20 ? t->cols - 10 : 20, &nprompt);
    if (!prompt) {
        free(rows);
        free(subs);
        return DLG_ERROR;
    }
    for (i = 0; i < table.maxrows; i++) {
        if (table.rows[i].used)
            m++;
    }
    if (m <= 0) {
        free_lines(prompt, nprompt);
        free(rows);
        free(subs);
        return DLG_ERROR;
    }
    {
        int widest = max_line_len(prompt, nprompt);
        for (i = 0; i < table.maxrows; i++) {
            if (!table.rows[i].used)
                continue;
            int l = (int)strlen(table.rows[i].key) + 2 +
                    (int)table.rows[i].vlen;
            if (l > widest)
                widest = l;
        }
        dw = (w > 0) ? w : widest + 6;
        if (dw > t->cols - 2)
            dw = t->cols - 2;
    }
    free_lines(prompt, nprompt);
    prompt = wrap_text(text, dw - 4, &nprompt);
    if (!prompt) {
        free(rows);
        free(subs);
        return DLG_ERROR;
    }
    mh = (menu_h > 0) ? menu_h : n;
    if (mh > t->rows - 10)
        mh = t->rows - 10;
    if (mh < 1)
        mh = 1;
    if (mh > n)
        mh = n;
    dh = (h > 0) ? h : nprompt + mh + 7;
    if (dh > t->rows - 2)
        dh = t->rows - 2;
    if (screen_setup(&s, t, backtitle, dw, dh) != 0) {
        free_lines(prompt, nprompt);
        free(rows);
        free(subs);
        return DLG_ERROR;
    }
    list_y = 2 + nprompt + 1;
    list_h = dh - list_y - 3;
    if (list_h > mh)
        list_h = mh;
    if (list_h < 1)
        list_h = 1;
    for (;;) {
        struct lat_keyev ev;
        int r;
        int idx;
        int shown;
        dlg_box(&s, title);
        for (i = 0; i < nprompt && i + 2 < list_y; i++)
            dlg_text(&s, 2, 2 + i, prompt[i], dw - 4);
        /* keep selection visible */
        if (sel < top)
            top = sel;
        if (sel >= top + list_h)
            top = sel - list_h + 1;
        idx = 0;
        shown = 0;
        for (r = 0; r < table.maxrows && shown < list_h; r++) {
            struct lat_row *row;
            char line[256];
            int y;
            if (!table.rows[r].used)
                continue;
            if (idx < top || idx >= top + list_h) {
                idx++;
                continue;
            }
            y = list_y + shown;
            row = &table.rows[r];
            snprintf(line, sizeof line, "%s  %.*s", row->key,
                     (int)row->vlen, row->val);
            if (idx == sel) {
                int k;
                int len = (int)strlen(line);
                if (len > dw - 4)
                    len = dw - 4;
                for (k = 0; k < dw - 4; k++) {
                    char ch = k < len ? line[k] : ' ';
                    lat_layer_put(&s.comp, s.dlg, (uint32_t)(2 + k),
                                  (uint32_t)y, (uint8_t)ch, C_HILITE_FG,
                                  C_HILITE_BG, LAT_ATTR_BOLD);
                }
            } else {
                dlg_text(&s, 2, y, line, dw - 4);
            }
            idx++;
            shown++;
        }
        draw_button(&s, dw / 2 - 8, dh - 2, "Select", 1);
        draw_button(&s, dw / 2 + 8, dh - 2, "Cancel", 0);
        screen_present(&s);
        ev = lat_term_read_key(t);
        if (ev.key == KEY_UP || (ev.key == KEY_CHAR && ev.ch == 'k'))
            sel = (sel + m - 1) % m;
        else if (ev.key == KEY_DOWN || (ev.key == KEY_CHAR && ev.ch == 'j'))
            sel = (sel + 1) % m;
        else if (ev.key == KEY_HOME)
            sel = 0;
        else if (ev.key == KEY_END)
            sel = m - 1;
        else if (ev.key == KEY_ENTER) {
            /* sel-th used row, in table order */
            int seen = 0;
            const char *tag = "";
            for (r = 0; r < table.maxrows; r++) {
                if (!table.rows[r].used)
                    continue;
                if (seen == sel) {
                    tag = table.rows[r].key;
                    break;
                }
                seen++;
            }
            emit_result(out_fd, tag);
            free_lines(prompt, nprompt);
            free(rows);
            free(subs);
            screen_teardown(&s);
            return DLG_OK;
        } else if (ev.key == KEY_ESC || ev.key == KEY_CTRL_C ||
                   ev.key == KEY_NONE) {
            free_lines(prompt, nprompt);
            free(rows);
            free(subs);
            screen_teardown(&s);
            return DLG_CANCEL;
        }
    }
}

/* ── inputbox / passwordbox ─────────────────────────────────────────── */

int dlg_inputbox(struct lat_term *t, const char *title,
                 const char *backtitle, const char *text, int h, int w,
                 const char *init, int password, int out_fd) {
    struct screen s;
    char **lines;
    int nlines;
    int dw;
    int dh;
    int i;
    char *buf;
    size_t cap = 512;
    size_t len = 0;
    size_t cursor = 0;
    int field_y;
    int field_w;
    buf = malloc(cap);
    if (!buf)
        return DLG_ERROR;
    if (init) {
        len = strlen(init);
        if (len >= cap)
            len = cap - 1;
        memcpy(buf, init, len);
    }
    buf[len] = '\0';
    cursor = len;
    lines = wrap_text(text, t->cols - 10 > 20 ? t->cols - 10 : 20, &nlines);
    if (!lines) {
        free(buf);
        return DLG_ERROR;
    }
    dw = (w > 0) ? w : max_line_len(lines, nlines) + 6;
    if (dw < 30)
        dw = 30;
    if (dw > t->cols - 2)
        dw = t->cols - 2;
    dh = (h > 0) ? h : nlines + 7;
    if (dh > t->rows - 2)
        dh = t->rows - 2;
    free_lines(lines, nlines);
    lines = wrap_text(text, dw - 4, &nlines);
    if (!lines) {
        free(buf);
        return DLG_ERROR;
    }
    if (screen_setup(&s, t, backtitle, dw, dh) != 0) {
        free_lines(lines, nlines);
        free(buf);
        return DLG_ERROR;
    }
    field_y = 2 + nlines + 1;
    if (field_y > dh - 3)
        field_y = dh - 3;
    field_w = dw - 4;
    for (;;) {
        struct lat_keyev ev;
        size_t vlen = len < (size_t)(field_w - 1) ? len : (size_t)(field_w - 1);
        size_t voff = 0;
        dlg_box(&s, title);
        for (i = 0; i < nlines && i + 2 < field_y; i++)
            dlg_text(&s, 2, 2 + i, lines[i], dw - 4);
        if (len > (size_t)(field_w - 1))
            voff = len - (size_t)(field_w - 1);
        if (cursor < voff)
            voff = cursor;
        for (i = 0; (size_t)i < vlen + 1 && i < field_w; i++) {
            char ch = ' ';
            if ((size_t)i < vlen)
                ch = password ? '*' : buf[voff + (size_t)i];
            if (voff + (size_t)i == cursor)
                lat_layer_put(&s.comp, s.dlg, (uint32_t)(2 + i),
                              (uint32_t)field_y, (uint8_t)(ch == ' ' ? '_' : ch),
                              C_HILITE_FG, C_HILITE_BG, 0);
            else
                lat_layer_put(&s.comp, s.dlg, (uint32_t)(2 + i),
                              (uint32_t)field_y, (uint8_t)ch, C_DLG_FG,
                              C_DLG_BG, LAT_ATTR_REVERSE);
        }
        draw_button(&s, dw / 2 - 8, dh - 2, "OK", 1);
        draw_button(&s, dw / 2 + 8, dh - 2, "Cancel", 0);
        screen_present(&s);
        ev = lat_term_read_key(t);
        if (ev.key == KEY_ENTER) {
            buf[len] = '\0';
            emit_result(out_fd, buf);
            free_lines(lines, nlines);
            free(buf);
            screen_teardown(&s);
            return DLG_OK;
        } else if (ev.key == KEY_ESC || ev.key == KEY_CTRL_C ||
                   ev.key == KEY_NONE) {
            free_lines(lines, nlines);
            free(buf);
            screen_teardown(&s);
            return DLG_CANCEL;
        } else if (ev.key == KEY_BACKSPACE) {
            if (cursor > 0) {
                memmove(buf + cursor - 1, buf + cursor, len - cursor + 1);
                cursor--;
                len--;
            }
        } else if (ev.key == KEY_LEFT) {
            if (cursor > 0)
                cursor--;
        } else if (ev.key == KEY_RIGHT) {
            if (cursor < len)
                cursor++;
        } else if (ev.key == KEY_HOME) {
            cursor = 0;
        } else if (ev.key == KEY_END) {
            cursor = len;
        } else if (ev.key == KEY_CHAR) {
            if (len + 1 < cap) {
                memmove(buf + cursor + 1, buf + cursor, len - cursor + 1);
                buf[cursor] = ev.ch;
                cursor++;
                len++;
            }
        }
    }
}

/* ── textbox (content pages through an L2 Stream) ───────────────────── */

static int read_file_bytes(const char *path, uint8_t **out, size_t *outlen) {
    /* cap: 512KB keeps the pager bounded on small machines */
    static const size_t cap = 512 * 1024;
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    size_t n = 0;
    size_t r;
    if (!f)
        return -1;
    buf = malloc(cap + 64);
    if (!buf) {
        fclose(f);
        return -1;
    }
    while (n < cap) {
        r = fread(buf + n, 1, cap - n, f);
        if (r == 0)
            break;
        n += r;
    }
    fclose(f);
    *out = buf;
    *outlen = n;
    return 0;
}

int dlg_textbox(struct lat_term *t, const char *title,
                const char *backtitle, const char *path, int h, int w) {
    struct screen s;
    uint8_t *data = NULL;
    size_t datalen = 0;
    struct lat_stream stream;
    struct lat_sub *subs;
    int dw;
    int dh;
    int view_h;
    int view_w;
    uint64_t total_lines = 0;
    uint64_t top = 0;
    /* index line starts (cap 8192 lines; beyond that, tail window) */
    static const int max_idx = 8192;
    uint64_t *starts;
    int nidx = 0;
    if (read_file_bytes(path, &data, &datalen) != 0)
        return DLG_ERROR;
    subs = malloc(sizeof(struct lat_sub) * 4);
    starts = malloc(sizeof(uint64_t) * (size_t)max_idx);
    if (!subs || !starts) {
        free(data);
        free(subs);
        free(starts);
        return DLG_ERROR;
    }
    lat_stream_init(&stream, data, datalen ? datalen : 1, subs, 4);
    /* Stream rings overwrite; textbox needs full retention, so push once
     * into a same-size ring (no wrap) and page via absolute offsets. */
    if (datalen > 0)
        lat_stream_push(&stream, data, datalen);
    {
        /* build the line index from the retained bytes */
        uint64_t base = lat_stream_base(&stream);
        size_t kept = lat_stream_retained(&stream);
        size_t i = 0;
        nidx = 0;
        starts[nidx++] = base;
        while (i < kept && nidx < max_idx) {
            if (data[(base + i) % (datalen ? datalen : 1)] == '\n') {
                if (base + i + 1 < stream.head)
                    starts[nidx++] = base + i + 1;
            }
            i++;
        }
        total_lines = (uint64_t)nidx;
    }
    dw = (w > 0) ? w : t->cols - 8;
    dh = (h > 0) ? h : t->rows - 4;
    if (dw > t->cols - 2)
        dw = t->cols - 2;
    if (dh > t->rows - 2)
        dh = t->rows - 2;
    if (screen_setup(&s, t, backtitle, dw, dh) != 0) {
        free(data);
        free(subs);
        free(starts);
        return DLG_ERROR;
    }
    view_h = dh - 4;
    view_w = dw - 4;
    for (;;) {
        struct lat_keyev ev;
        uint64_t r;
        dlg_box(&s, title);
        if (top + (uint64_t)view_h > total_lines && total_lines > 0)
            top = total_lines > (uint64_t)view_h
                      ? total_lines - (uint64_t)view_h
                      : 0;
        for (r = 0; r < (uint64_t)view_h; r++) {
            char line[512];
            uint64_t li = top + r;
            if (li < total_lines) {
                uint64_t ls = starts[li];
                uint64_t le = (li + 1 < total_lines) ? starts[li + 1]
                                                    : stream.head;
                size_t ll = (size_t)(le - ls);
                char *dst = line;
                size_t k;
                if (ll > (size_t)(view_w))
                    ll = (size_t)view_w;
                for (k = 0; k < ll; k++) {
                    uint8_t b = 0;
                    lat_stream_read(&stream, ls + k, 1, &b);
                    if (b == '\n' || b == '\r' || b == '\t')
                        b = ' ';
                    if (b < 0x20 || b == 0x7F)
                        b = '.';
                    dst[k] = (char)b;
                }
                dst[ll] = '\0';
                dlg_text(&s, 2, 2 + (int)r, line, view_w);
            }
        }
        {
            char pos[64];
            snprintf(pos, sizeof pos, "[%llu/%llu]", (unsigned long long)(top + 1),
                     (unsigned long long)total_lines);
            dlg_text(&s, dw - (int)strlen(pos) - 3, dh - 2, pos, 20);
        }
        draw_button(&s, 8, dh - 2, "Exit", 1);
        screen_present(&s);
        ev = lat_term_read_key(t);
        if (ev.key == KEY_DOWN || (ev.key == KEY_CHAR && ev.ch == 'j')) {
            if (top + (uint64_t)view_h < total_lines)
                top++;
        } else if (ev.key == KEY_UP || (ev.key == KEY_CHAR && ev.ch == 'k')) {
            if (top > 0)
                top--;
        } else if (ev.key == KEY_PGDN ||
                   (ev.key == KEY_CHAR && (ev.ch == ' ' || ev.ch == 'f'))) {
            top += (uint64_t)view_h;
            if (top + (uint64_t)view_h > total_lines && total_lines > 0)
                top = total_lines > (uint64_t)view_h
                          ? total_lines - (uint64_t)view_h
                          : 0;
        } else if (ev.key == KEY_PGUP) {
            top = top > (uint64_t)view_h ? top - (uint64_t)view_h : 0;
        } else if (ev.key == KEY_HOME) {
            top = 0;
        } else if (ev.key == KEY_END) {
            top = total_lines > (uint64_t)view_h
                      ? total_lines - (uint64_t)view_h
                      : 0;
        } else if (ev.key == KEY_ENTER || ev.key == KEY_ESC ||
                   ev.key == KEY_CTRL_C || ev.key == KEY_NONE ||
                   (ev.key == KEY_CHAR && ev.ch == 'q')) {
            free(data);
            free(subs);
            free(starts);
            screen_teardown(&s);
            return DLG_OK;
        }
    }
}

/* ── gauge (percent is an L2 Signal; stdin lines bump its version) ──── */

static int gauge_parse_line(const char *line, int *pct, char *msg,
                            size_t msgcap) {
    /* plain "NNN" percent */
    char *end = NULL;
    long v;
    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '\0')
        return 0;
    v = strtol(line, &end, 10);
    if (end != line) {
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
            end++;
        if (*end == '\0' || *end == '%') {
            if (v < 0)
                v = 0;
            if (v > 100)
                v = 100;
            *pct = (int)v;
            return 1;
        }
    }
    /* "XXX" message block handled by the caller */
    strncpy(msg, line, msgcap - 1);
    msg[msgcap - 1] = '\0';
    return 2;
}

int dlg_gauge(struct lat_term *t, const char *title, const char *backtitle,
              const char *text, int h, int w, int pct, int in_fd) {
    struct screen s;
    char **lines;
    int nlines;
    int dw;
    int dh;
    int i;
    int use_stdin;
    struct lat_signal sig;
    uint8_t sigbuf[16];
    struct lat_sub subs[4];
    uint64_t seen = 0;
    char msg[256] = "";
    int bar_w;
    int bar_y;
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    lines = wrap_text(text, t->cols - 10 > 20 ? t->cols - 10 : 20, &nlines);
    if (!lines)
        return DLG_ERROR;
    dw = (w > 0) ? w : max_line_len(lines, nlines) + 6;
    if (dw < 40)
        dw = 40;
    if (dw > t->cols - 2)
        dw = t->cols - 2;
    dh = (h > 0) ? h : nlines + 8;
    if (dh > t->rows - 2)
        dh = t->rows - 2;
    free_lines(lines, nlines);
    lines = wrap_text(text, dw - 4, &nlines);
    if (!lines)
        return DLG_ERROR;
    if (screen_setup(&s, t, backtitle, dw, dh) != 0) {
        free_lines(lines, nlines);
        return DLG_ERROR;
    }
    lat_signal_init(&sig, sigbuf, sizeof sigbuf, subs, 4);
    lat_signal_set(&sig, &pct, sizeof pct);
    use_stdin = !isatty(in_fd);
    bar_w = dw - 6;
    bar_y = 2 + nlines + 2;
    if (bar_y > dh - 3)
        bar_y = dh - 3;
    if (use_stdin) {
        /* whiptail protocol: "NNN" percent lines, "XXX" message blocks. */
        char lbuf[512];
        size_t llen = 0;
        int in_block = 0;
        char block[1024] = "";
        for (;;) {
            char c;
            ssize_t n = read(in_fd, &c, 1);
            if (n <= 0)
                break;
            if (c == '\n') {
                lbuf[llen] = '\0';
                if (strcmp(lbuf, "XXX") == 0) {
                    if (!in_block) {
                        in_block = 1;
                        block[0] = '\0';
                    } else {
                        in_block = 0;
                        strncpy(msg, block, sizeof msg - 1);
                        msg[sizeof msg - 1] = '\0';
                    }
                } else if (in_block) {
                    if (block[0])
                        strncat(block, "\n", sizeof block - strlen(block) - 1);
                    strncat(block, lbuf, sizeof block - strlen(block) - 1);
                } else {
                    char m2[256] = "";
                    int r = gauge_parse_line(lbuf, &pct, m2, sizeof m2);
                    if (r == 1)
                        lat_signal_set(&sig, &pct, sizeof pct);
                    else if (r == 2) {
                        strncpy(msg, m2, sizeof msg - 1);
                        msg[sizeof msg - 1] = '\0';
                    }
                }
                /* re-render on generation change (or message change) */
                if (sig.version != seen) {
                    seen = sig.version;
                    lat_signal_get(&sig, &pct, sizeof pct, NULL);
                }
                dlg_box(&s, title);
                for (i = 0; i < nlines && i + 2 < bar_y; i++)
                    dlg_text(&s, 2, 2 + i, lines[i], dw - 4);
                if (msg[0])
                    dlg_text(&s, 2, 2 + nlines, msg, dw - 4);
                {
                    int fill = (pct * bar_w) / 100;
                    int k;
                    char pcts[16];
                    for (k = 0; k < bar_w; k++)
                        lat_layer_put(&s.comp, s.dlg, (uint32_t)(3 + k),
                                      (uint32_t)bar_y,
                                      (uint8_t)(k < fill ? '#' : '-'),
                                      C_HILITE_FG, C_HILITE_BG, 0);
                    snprintf(pcts, sizeof pcts, "%d%%", pct);
                    dlg_text(&s, 3, bar_y + 1, pcts, 8);
                }
                screen_present(&s);
                llen = 0;
            } else if (llen + 1 < sizeof lbuf) {
                lbuf[llen++] = c;
            }
        }
        free_lines(lines, nlines);
        screen_teardown(&s);
        return DLG_OK;
    }
    /* tty: show once, wait for a key */
    dlg_box(&s, title);
    for (i = 0; i < nlines && i + 2 < bar_y; i++)
        dlg_text(&s, 2, 2 + i, lines[i], dw - 4);
    {
        int fill = (pct * bar_w) / 100;
        int k;
        for (k = 0; k < bar_w; k++)
            lat_layer_put(&s.comp, s.dlg, (uint32_t)(3 + k), (uint32_t)bar_y,
                          (uint8_t)(k < fill ? '#' : '-'), C_HILITE_FG,
                          C_HILITE_BG, 0);
    }
    screen_present(&s);
    free_lines(lines, nlines);
    for (;;) {
        struct lat_keyev ev = lat_term_read_key(t);
        if (ev.key == KEY_ENTER || ev.key == KEY_NONE) {
            screen_teardown(&s);
            return DLG_OK;
        }
        if (ev.key == KEY_ESC || ev.key == KEY_CTRL_C) {
            screen_teardown(&s);
            return DLG_CANCEL;
        }
    }
}
