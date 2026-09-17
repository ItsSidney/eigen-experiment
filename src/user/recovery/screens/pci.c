/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * screens/pci.c - PCI device tree body.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "screens/screens.h"
#include "ui/ui.h"
#include "overlays/overlays.h"

/* ── PCI ────────────────────────────────────────────────────────────── */

void draw_pci_body(struct app *a) {
    int w = a->body_w;
    int y = 0;
    int i;
    ui_text(a, a->lay_body, 0, y++, "BDF  CLASS  DESCRIPTION", TH_FG, TH_BG,
            LAT_ATTR_BOLD, w);
    if (a->data.npci == 0) {
        ui_text(a, a->lay_body, 2, y++, "no PCI list visible", TH_FG, TH_BG, LAT_ATTR_DIM,
                w - 2);
        return;
    }
    if (a->pci_sel >= a->data.npci)
        a->pci_sel = a->data.npci - 1;
    for (i = 0; i < a->data.npci && y < a->body_h; i++) {
        struct pci_item *p = &a->data.pcis[i];
        char line[160];
        int cursor = (a->pci_sel == i);
        snprintf(line, sizeof line, "%s  %s  %s", p->bdf, p->cls, p->desc);
        ui_text(a, a->lay_body, cursor ? 0 : 2, y, line,
                cursor ? TH_SEL_FG : TH_FG, cursor ? TH_SEL_BG : TH_BG,
                cursor ? LAT_ATTR_BOLD : 0, w - 2);
        y++;
    }
}

int pci_key(struct app *a, struct lat_keyev ev) {
    if (a->data.npci == 0)
        return 0;
    if (ev.key == KEY_UP) {
        a->pci_sel = (a->pci_sel + a->data.npci - 1) % a->data.npci;
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_DOWN) {
        a->pci_sel = (a->pci_sel + 1) % a->data.npci;
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_ENTER) {
        struct pci_item *p = &a->data.pcis[a->pci_sel];
        char detail[512];
        snprintf(detail, sizeof detail, "DEVICE %s\nclass %s\n%s%s", p->bdf,
                 p->cls, p->desc,
                 strncmp(p->cls, "01", 2) == 0
                     ? "\n\nstorage controller: see STORAGE"
                     : "");
        show_message(a, detail);
        return 1;
    }
    return 0;
}

