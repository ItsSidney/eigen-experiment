/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * home.c - HOME content: menu with live status column + shell peek.
 * Draws into the body layer (box interior, 0-based). Userspace (musl).
 */

#include <stdio.h>
#include <string.h>

#include "ui/ui.h"
#include "screens/screens.h"

/* Per-item right-column status: why you might open it. */
static void menu_status(struct app *a, int idx, char *out, size_t cap,
                        int *hot) {
    struct sysdata *d = &a->data;
    *hot = 0;
    switch (idx) {
    case 0: {
        size_t kept = lat_stream_retained(&d->kmsg);
        if (kept > 1024)
            snprintf(out, cap, "%lu KB", kept / 1024);
        else
            snprintf(out, cap, "%lu B", kept);
        break;
    }
    case 1:
        if (d->regs_valid)
            snprintf(out, cap, "#%s %s", d->regs_vec, d->regs_mnem);
        else
            snprintf(out, cap, "no fault");
        break;
    case 2:
        if (d->exc_count > 0) {
            snprintf(out, cap, "%lu recorded", d->exc_count);
            *hot = 1;
        } else {
            snprintf(out, cap, "none");
        }
        break;
    case 3: {
        int bad = 0;
        int i;
        for (i = 0; i < d->nrisk; i++) {
            if (d->risks[i].bad)
                bad++;
        }
        if (bad > 0) {
            snprintf(out, cap, "! %d at risk", bad);
            *hot = 1;
        } else {
            snprintf(out, cap, "%d mounts", d->nmnt);
        }
        break;
    }
    case 4:
        snprintf(out, cap, "%d devs", d->npci);
        break;
    case 5:
        snprintf(out, cap, "@0x%lX", a->hex.addr);
        break;
    case 6:
        snprintf(out, cap, "%s", a->shell.running ? "running" : "idle");
        break;
    default:
        snprintf(out, cap, "-");
        break;
    }
}

static void draw_home_content(struct app *a) {
    int w = a->body_w;
    int h = a->body_h;
    int i;
    int y = 0;
    int nmenu = MENU_N < h - 3 ? MENU_N : h - 3;
    if (nmenu < 0)
        nmenu = 0;
    for (i = 0; i < nmenu; i++) {
        char label[64];
        char status[48];
        char row[128];
        int hot = 0;
        int sel = (a->menu_sel == i);
        menu_status(a, i, status, sizeof status, &hot);
        snprintf(label, sizeof label, "%c %d  %s", sel ? 0x10 : ' ',
                 i + 1, menu_items[i].label);
        snprintf(row, sizeof row, "%-32s %s", label, status);
        if (sel) {
            int k;
            for (k = 0; k < w; k++) {
                char ch = k < (int)strlen(row) ? row[k] : ' ';
                /* arrow stays black, the rest of the bar is white */
                ui_put(a, a->lay_body, k, y, ch,
                       k == 0 ? TH_FG : TH_SEL_FG, TH_SEL_BG,
                       LAT_ATTR_BOLD);
            }
        } else {
            int sl;
            int sx;
            int k;
            ui_text(a, a->lay_body, 0, y, row, TH_FG, TH_BG, 0, w);
            if (hot) {
                /* re-tint the status column hot */
                sl = (int)strlen(status);
                sx = 33;
                for (k = 0; k < sl && sx + k < w; k++)
                    ui_put(a, a->lay_body, sx + k, y, status[k], TH_HOT,
                           TH_BG, LAT_ATTR_BOLD);
            }
        }
        y++;
    }
    /* shell peek takes the remainder */
    if (y + 2 < h) {
        int pk;
        int p;
        ui_text(a, a->lay_body, 0, y++, "shell -- live", TH_FG, TH_BG,
                    LAT_ATTR_BOLD, w);
        pk = h - y;
        if (pk > 4)
            pk = 4;
        if (a->shell.running && pk > 0) {
            size_t kept = lat_stream_retained(&a->shell.out);
            size_t scan = kept > 2048 ? 2048 : kept;
            static char tail[2048];
            if (scan > 0)
                lat_stream_read(&a->shell.out, a->shell.out.head - scan,
                                scan, tail);
            {
                int total = 0;
                size_t k;
                int skip;
                int seen = 0;
                size_t pos = 0;
                for (k = 0; k < scan; k++) {
                    if (tail[k] == '\n')
                        total++;
                }
                skip = total - pk;
                if (skip < 0)
                    skip = 0;
                while (pos < scan && seen < skip) {
                    if (tail[pos++] == '\n')
                        seen++;
                }
                for (p = 0; p < pk && pos < scan && y < h; p++) {
                    char line[128];
                    int ll = 0;
                    while (pos < scan && tail[pos] != '\n' && ll < 120)
                        line[ll++] = tail[pos++];
                    if (pos < scan && tail[pos] == '\n')
                        pos++;
                    line[ll] = '\0';
                    ui_text(a, a->lay_body, 2, y++, line, TH_FG, TH_BG, 0,
                            w - 2);
                }
            }
        } else if (pk > 0) {
            ui_text(a, a->lay_body, 2, y++, "(shell idle -- open item 7)",
                    TH_FG, TH_BG, LAT_ATTR_DIM, w - 2);
        }
    }
}

void draw_body(struct app *a) {
    switch (a->screen) {
    case SCR_HOME:
        draw_home_content(a);
        break;
    case SCR_LOG:
        draw_log_body(a);
        break;
    case SCR_REGS:
        draw_regs_body(a);
        break;
    case SCR_EXC:
        draw_exc_body(a);
        break;
    case SCR_STORAGE:
        draw_storage_body(a);
        break;
    case SCR_PCI:
        draw_pci_body(a);
        break;
    case SCR_HEX:
        draw_hex_body(a);
        break;
    case SCR_SHELL:
        draw_shell_body(a);
        break;
    default:
        break;
    }
}
