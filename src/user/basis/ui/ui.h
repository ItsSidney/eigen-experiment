/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * ui.h - basisui chrome in the shared nmtui language: blue screen,
 * floating white panel + shadow, blue selection bar, <buttons>.
 */

#ifndef BASISUI_UI_H
#define BASISUI_UI_H

#include "app.h"

#define TH_BG 7
#define TH_FG 0
#define TH_SEL_BG 4
#define TH_SEL_FG 7
#define TH_OK 2
#define TH_HOT 1
#define TH_WARN 5

int ui_setup(struct app *a);
void ui_teardown(struct app *a);
void ui_draw(struct app *a);
void ui_present(struct app *a);

/* Implemented by main.c (needs ui_draw's contract). */
void draw_screen(struct app *a);
void draw_modal(struct app *a);

void ui_text(struct app *a, int layer, int lx, int ly, const char *str,
             int fg, int bg, int attr, int maxlen);
void ui_put(struct app *a, int layer, int lx, int ly, char ch, int fg,
            int bg, int attr);
void ui_fill(struct app *a, int layer, int lx, int ly, int w, int h, char ch,
             int fg, int bg, int attr);

void ui_modal_frame(struct app *a, const char *title, int w, int h,
                    int *ix, int *iy, int *iw, int *ih);
void ui_button(struct app *a, int layer, int cx, int ly, const char *label,
               int focused);

/* state -> dot color */
int state_fg(const char *state);

#endif /* BASISUI_UI_H */
