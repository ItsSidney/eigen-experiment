/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * screens/shellview.c - pty shell body.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "screens/screens.h"
#include "ui/ui.h"

/* ── SHELL body ─────────────────────────────────────────────────────── */

void draw_shell_body(struct app *a) {
    int w = a->body_w;
    int h = a->body_h;
    size_t kept;
    size_t scan;
    uint64_t base;
    static char *win = NULL;
    if (!win) {
        win = malloc(8192);
        if (!win)
            return;
    }
    /* black terminal: fill first so the white panel never shows through */
    ui_fill(a, a->lay_body, 0, 0, w, h, ' ', 7, 0, 0);
    if (!a->shell.running) {
        ui_text(a, a->lay_body, 0, 1, "[shell closed: ENTER reopens]",
                7, 0, LAT_ATTR_DIM, w);
        return;
    }
    kept = lat_stream_retained(&a->shell.out);
    scan = kept > 8000 ? 8000 : kept;
    base = a->shell.out.head - scan;
    if (scan > 0)
        lat_stream_read(&a->shell.out, base, scan, win);
    {
        /* last h-1 lines */
        int total = 0;
        size_t k;
        int start = 0;
        int i;
        int y = 1;
        for (k = 0; k < scan; k++) {
            if (win[k] == '\n')
                total++;
        }
        {
            int skip = total - (h - 1);
            int seen = 0;
            size_t pos = 0;
            if (skip < 0)
                skip = 0;
            while (pos < scan && seen < skip) {
                if (win[pos++] == '\n')
                    seen++;
            }
            start = (int)pos;
        }
        for (i = start; i < (int)scan && y < h; i++) {
            static char line[256];
            int ll = 0;
            while (i < (int)scan && win[i] != '\n' && ll < 250)
                line[ll++] = win[i++];
            line[ll] = '\0';
            ui_text(a, a->lay_body, 0, y++, line, 7, 0, 0, w);
        }
    }
}

