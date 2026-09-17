/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * screens/log.c - kernel log stream body.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "screens/screens.h"
#include "ui/ui.h"

/* ── LOG: stream widget, pause + filter ───────────────────────────────── */

#define LOG_WIN (32 * 1024)

void draw_log_body(struct app *a) {
    int w = a->body_w;
    int h = a->body_h;
    int y = 0;
    char head[128];
    /* snapshot the retained window (cap) */
    static char *win = NULL;
    size_t kept;
    size_t scan;
    uint64_t base;
    /* line index over the window */
    static uint64_t *starts = NULL;
    static int cap_idx = 4096;
    int nlines = 0;
    int i;
    int top;
    int shown = 0;
    if (!win) {
        win = malloc(LOG_WIN);
        starts = malloc(sizeof(uint64_t) * (size_t)cap_idx);
        if (!win || !starts)
            return;
    }
    kept = lat_stream_retained(&a->data.kmsg);
    scan = kept > LOG_WIN ? LOG_WIN : kept;
    base = a->data.kmsg.head - scan;
    if (scan > 0)
        lat_stream_read(&a->data.kmsg, base, scan, win);
    /* index */
    if (scan > 0) {
        size_t k = 0;
        starts[nlines++] = 0;
        while (k < scan && nlines < cap_idx) {
            if (win[k] == '\n' && k + 1 < scan)
                starts[nlines++] = k + 1;
            k++;
        }
    }
    if (a->log.paused)
        snprintf(head, sizeof head, "LOG [paused]%s%s", a->log.filter[0] ? " [filter: " : "", a->log.filter[0] ? a->log.filter : "");
    else
        snprintf(head, sizeof head, "LOG [live tail]%s%s", a->log.filter[0] ? " [filter: " : "", a->log.filter[0] ? a->log.filter : "");
    if (a->log.filter[0]) {
        size_t hl = strlen(head);
        if (hl + 1 < sizeof head) {
            head[hl] = ']';
            head[hl + 1] = '\0';
        }
    }
    ui_text(a, a->lay_body, 0, y++, head, TH_FG, TH_BG, LAT_ATTR_BOLD, w);
    /* top line: live tail or paused anchor (line index) */
    if (!a->log.paused || a->log.anchor >= (uint64_t)nlines) {
        top = nlines - (h - 1);
        if (top < 0)
            top = 0;
    } else {
        top = (int)a->log.anchor;
    }
    for (i = top; i < nlines && shown < h - 1; i++) {
        size_t s = (size_t)starts[i];
        size_t e = (i + 1 < nlines) ? (size_t)starts[i + 1] : scan;
        char line[256];
        size_t ll = e - s;
        int sev;
        if (ll > 0 && win[e - 1] == '\n')
            ll--;
        if (a->log.filter[0]) {
            /* substring match on the raw line */
            size_t fl = strlen(a->log.filter);
            size_t k;
            int hit = 0;
            if (fl <= ll) {
                for (k = 0; k + fl <= ll; k++) {
                    if (memcmp(win + s + k, a->log.filter, fl) == 0) {
                        hit = 1;
                        break;
                    }
                }
            }
            if (!hit)
                continue;
        }
        if (ll > sizeof line - 5)
            ll = sizeof line - 5;
        memcpy(line, win + s, ll);
        line[ll] = '\0';
        sev = kmsg_severity(line, ll);
        {
            char out[256];
            snprintf(out, sizeof out, "%c %s", sev == 2 ? '!' : sev == 1 ? '*' : '.', line);
            ui_text(a, a->lay_body, 0, y + shown, out,
                    sev == 2 ? TH_HOT : sev == 1 ? TH_WARN : TH_FG, TH_BG,
                    0, w);
        }
        shown++;
    }
    if (a->log.paused && shown < h - 1)
        ui_text(a, a->lay_body, 0, y + shown, "-- paused, END resumes live --",
                TH_FG, TH_BG, LAT_ATTR_DIM, w);
}

int log_key(struct app *a, struct lat_keyev ev) {
    if (ev.key == KEY_CHAR && ev.ch == ' ') {
        a->log.paused = !a->log.paused;
        if (!a->log.paused)
            a->log.anchor = 0;
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_UP) {
        if (!a->log.paused) {
            a->log.paused = 1;
            a->log.anchor = 0;
        } else if (a->log.anchor > 0) {
            a->log.anchor--;
        }
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_DOWN) {
        if (a->log.paused) {
            a->log.anchor++;
            a->dirty = 1;
        }
        return 1;
    }
    if (ev.key == KEY_PGUP) {
        if (!a->log.paused) {
            a->log.paused = 1;
            a->log.anchor = 0;
        } else if (a->log.anchor > 10) {
            a->log.anchor -= 10;
        } else {
            a->log.anchor = 0;
        }
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_PGDN) {
        if (a->log.paused) {
            a->log.anchor += 10;
            a->dirty = 1;
        }
        return 1;
    }
    if (ev.key == KEY_END) {
        a->log.paused = 0;
        a->log.anchor = 0;
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_HOME) {
        a->log.paused = 1;
        a->log.anchor = 0;
        a->dirty = 1;
        return 1;
    }
    return 0;
}

