/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * screens/storage.c - mount inspector body.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "screens/screens.h"
#include "ui/ui.h"
#include "overlays/overlays.h"

/* ── STORAGE + force-mount modal ────────────────────────────────────── */

void draw_storage_body(struct app *a) {
    int w = a->body_w;
    int y = 0;
    int i;
    ui_text(a, a->lay_body, 0, y++, "MOUNT  SOURCE  TYPE  STATE", TH_FG, TH_BG,
            LAT_ATTR_BOLD, w);
    if (a->data.nmnt == 0) {
        ui_text(a, a->lay_body, 2, y++, "no mounts visible", TH_FG, TH_BG, LAT_ATTR_DIM, w - 2);
        return;
    }
    if (a->storage_sel >= a->data.nmnt)
        a->storage_sel = a->data.nmnt - 1;
    for (i = 0; i < a->data.nmnt && y < a->body_h; i++) {
        struct mnt_item *m = &a->data.mnts[i];
        char line[160];
        int cursor = (a->storage_sel == i);
        snprintf(line, sizeof line, "%s  %s  %s  mounted", m->mp, m->src,
                 m->fs);
        ui_text(a, a->lay_body, cursor ? 0 : 2, y, line,
                cursor ? TH_SEL_FG : TH_OK, cursor ? TH_SEL_BG : TH_BG,
                cursor ? LAT_ATTR_BOLD : 0, w - 2);
        y++;
    }
    if (y < a->body_h)
        ui_text(a, a->lay_body, 0, y,
                "(no block layer: sizes unavailable)", TH_FG, TH_BG, LAT_ATTR_DIM, w);
}

int storage_key(struct app *a, struct lat_keyev ev) {
    if (a->data.nmnt == 0)
        return 0;
    if (ev.key == KEY_UP) {
        a->storage_sel =
            (a->storage_sel + a->data.nmnt - 1) % a->data.nmnt;
        snprintf(a->crumb_extra, sizeof a->crumb_extra, "%s",
                 a->data.mnts[a->storage_sel].mp);
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_DOWN) {
        a->storage_sel = (a->storage_sel + 1) % a->data.nmnt;
        snprintf(a->crumb_extra, sizeof a->crumb_extra, "%s",
                 a->data.mnts[a->storage_sel].mp);
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_ENTER) {
        struct mnt_item *m = &a->data.mnts[a->storage_sel];
        char detail[512];
        snprintf(detail, sizeof detail,
                 "MOUNT %s\nsource: %s\ntype: %s\nopts: rw\n\n"
                 "F attempts a real mount call.",
                 m->mp, m->src, m->fs);
        show_message(a, detail);
        return 1;
    }
    if (ev.key == KEY_CHAR && (ev.ch == 'f' || ev.ch == 'F')) {
        struct mnt_item *m = &a->data.mnts[a->storage_sel];
        char text[256];
        snprintf(text, sizeof text,
                 "Force mount %s?\n\nThe table stays visible behind this\n"
                 "confirm; nothing is hidden.",
                 m->mp);
        snprintf(a->confirm_param, sizeof a->confirm_param, "%s %s",
                 m->src, m->mp);
        show_confirm(a, "FORCE MOUNT", text, ACT_FORCE_MOUNT);
        return 1;
    }
    return 0;
}

