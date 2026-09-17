/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * screens/regs.c - register grid + live diffs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "screens/screens.h"
#include "ui/ui.h"

/* ── REGISTERS: grid + live diff glyphs ─────────────────────────────── */

void draw_regs_body(struct app *a) {
    int w = a->body_w;
    int y = 0;
    int i;
    static char prev[NREGS][20];
    static int have_prev = 0;
    if (!a->data.regs_valid) {
        ui_text(a, a->lay_body, 0, y++, "REGISTERS", TH_FG, TH_BG, LAT_ATTR_BOLD, w);
        ui_text(a, a->lay_body, 0, y++,
                "No exception recorded yet.", TH_FG, TH_BG, LAT_ATTR_DIM, w);
        ui_text(a, a->lay_body, 0, y++,
                "Registers appear after the first CPU fault.", TH_FG, TH_BG, LAT_ATTR_DIM, w);
        return;
    }
    {
        char head[128];
        snprintf(head, sizeof head, "REGISTERS  %s  #%s %s", a->regs.frozen ? "frozen" : "* live, polling", a->data.regs_vec, a->data.regs_mnem);
        ui_text(a, a->lay_body, 0, y++, head, TH_FG, TH_BG, LAT_ATTR_BOLD, w);
    }
    if (!a->regs.frozen || !have_prev) {
        /* refresh the diff baseline when live */
        if (!a->regs.frozen) {
            memcpy(prev, a->data.regval, sizeof prev);
            have_prev = 1;
        }
    }
    for (i = 0; i < NREGS && y < a->body_h; i++) {
        char line[64];
        int changed = have_prev &&
                      memcmp(prev[i], a->data.regval[i], 20) != 0;
        int pinned = a->regs.pinned[i] != 0;
        int cursor = (a->regs.sel == i);
        snprintf(line, sizeof line, "%c %s %s%s", changed ? '^' : ' ',
                 a->data.regname[i], a->data.regval[i],
                 pinned ? "  *pinned" : "");
        ui_text(a, a->lay_body, cursor ? 0 : 2, y, line,
                cursor ? TH_SEL_FG : changed ? TH_WARN : TH_FG,
                cursor ? TH_SEL_BG : TH_BG,
                cursor ? LAT_ATTR_BOLD : 0, w - 2);
        y++;
    }
    if (y < a->body_h) {
        char fl[128];
        static const char *bits[7] = { "CF", "PF", "ZF", "SF",
                                       "IF", "DF", "OF" };
        char bstr[64] = "";
        for (i = 0; i < 7; i++) {
            char b[8];
            if (a->data.flagbit[i] < 0)
                snprintf(b, sizeof b, "%s?", bits[i]);
            else
                snprintf(b, sizeof b, "%s%c", bits[i],
                         a->data.flagbit[i] ? '*' : '.');
            strncat(bstr, b, sizeof bstr - strlen(bstr) - 1);
            strncat(bstr, " ", sizeof bstr - strlen(bstr) - 1);
        }
        snprintf(fl, sizeof fl, "RFLAGS  %s", bstr);
        ui_text(a, a->lay_body, 0, y++, fl, TH_FG, TH_BG, 0, w);
    }
}

int regs_key(struct app *a, struct lat_keyev ev) {
    if (ev.key == KEY_UP) {
        a->regs.sel = (a->regs.sel + NREGS - 1) % NREGS;
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_DOWN) {
        a->regs.sel = (a->regs.sel + 1) % NREGS;
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_CHAR && (ev.ch == 'p' || ev.ch == 'P')) {
        a->regs.pinned[a->regs.sel] = !a->regs.pinned[a->regs.sel];
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_CHAR && (ev.ch == 'f' || ev.ch == 'F')) {
        a->regs.frozen = !a->regs.frozen;
        a->dirty = 1;
        return 1;
    }
    return 0;
}

