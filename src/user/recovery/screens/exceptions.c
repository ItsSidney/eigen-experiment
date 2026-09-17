/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * screens/exceptions.c - fault history body.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "screens/screens.h"
#include "ui/ui.h"
#include "overlays/overlays.h"

/* ── EXCEPTIONS ─────────────────────────────────────────────────────── */

void draw_exc_body(struct app *a) {
    int w = a->body_w;
    int y = 0;
    int i;
    char head[64];
    snprintf(head, sizeof head, "EXCEPTION HISTORY (%d)", a->data.nexc);
    ui_text(a, a->lay_body, 0, y++, head, TH_FG, TH_BG, LAT_ATTR_BOLD, w);
    if (a->data.nexc == 0) {
        ui_text(a, a->lay_body, 2, y++, "none recorded", TH_FG, TH_BG, LAT_ATTR_DIM, w - 2);
        return;
    }
    for (i = 0; i < a->data.nexc && y < a->body_h; i++) {
        struct exc_item *e = &a->data.excs[i];
        char line[128];
        unsigned long age =
            a->data.upsec > e->sec ? a->data.upsec - e->sec : 0;
        int cursor = (a->exc_sel == i);
        snprintf(line, sizeof line, "#%s %s %lus ago pid %lu", e->mnem,
                 e->rip, age, e->pid);
        ui_text(a, a->lay_body, cursor ? 0 : 2, y, line,
                cursor ? TH_SEL_FG : TH_FG, cursor ? TH_SEL_BG : TH_BG,
                cursor ? LAT_ATTR_BOLD : 0, w - 2);
        y++;
    }
}

int exc_key(struct app *a, struct lat_keyev ev) {
    if (a->data.nexc == 0)
        return 0;
    if (ev.key == KEY_UP) {
        a->exc_sel = (a->exc_sel + a->data.nexc - 1) % a->data.nexc;
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_DOWN) {
        a->exc_sel = (a->exc_sel + 1) % a->data.nexc;
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_ENTER) {
        struct exc_item *e = &a->data.excs[a->exc_sel];
        char detail[512];
        snprintf(detail, sizeof detail,
                 "ENTRY %lu #%s\nRIP %s\nt+%lu.%06lus  pid %lu\n\n"
                 "Full register context is kept with the\n"
                 "latest fault on the REGISTERS screen.",
                 e->idx, e->mnem, e->rip, e->sec, e->us, e->pid);
        show_message(a, detail);
        return 1;
    }
    return 0;
}

