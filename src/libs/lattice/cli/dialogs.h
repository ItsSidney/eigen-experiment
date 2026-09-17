/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * dialogs.h - whiptail-subset dialogs rendered on the Lattice stack.
 *
 * Every dialog owns an L0 surface (terminal backend) plus an L1
 * compositor: a fullscreen background layer and a centered dialog
 * layer with dim_behind, so the chrome reads as one floating box.
 * L2 bindings back the live widgets: the menu rows live in a Table,
 * the textbox pages through a Stream, the gauge percent is a Signal
 * whose version bump drives re-renders.
 *
 * Results go to out_fd (stderr in main); return DLG_OK / DLG_CANCEL /
 * DLG_ERROR like dialog(1).
 */

#ifndef LATTICE_CLI_DIALOGS_H
#define LATTICE_CLI_DIALOGS_H

#include "term.h"

#define DLG_OK 0
#define DLG_CANCEL 1
#define DLG_ERROR 255

int dlg_msgbox(struct lat_term *t, const char *title, const char *backtitle,
               const char *text, int h, int w, int infobox);
int dlg_yesno(struct lat_term *t, const char *title, const char *backtitle,
              const char *text, int h, int w);
int dlg_menu(struct lat_term *t, const char *title, const char *backtitle,
             const char *text, int h, int w, int menu_h,
             char **tags, char **items, int n, int out_fd);
int dlg_inputbox(struct lat_term *t, const char *title,
                 const char *backtitle, const char *text, int h, int w,
                 const char *init, int password, int out_fd);
int dlg_textbox(struct lat_term *t, const char *title,
                const char *backtitle, const char *path, int h, int w);
int dlg_gauge(struct lat_term *t, const char *title, const char *backtitle,
              const char *text, int h, int w, int pct, int in_fd);

#endif /* LATTICE_CLI_DIALOGS_H */
