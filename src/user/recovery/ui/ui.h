/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * ui.h - chrome, menu rail and modal frame for eigen-recovery.
 */

#ifndef EIGEN_RECOVERY_UI_H
#define EIGEN_RECOVERY_UI_H

#include "app.h"

/* nmtui theme: white panel, black ink, blue selection bar. Yellow is
 * unreadable on white, so warnings use magenta. */
#define TH_BG 7
#define TH_FG 0
#define TH_SEL_BG 4
#define TH_SEL_FG 7
#define TH_OK 2
#define TH_HOT 1
#define TH_WARN 5

/* Menu entries (number keys 1-9). Export/Reboot are actions, not screens. */
#define MENU_N 9
struct menu_item {
    char key;
    const char *label;
    enum screen screen; /* NSCR = action */
    int action;         /* ACT_EXPORT / ACT_REBOOT for actions */
};

extern const struct menu_item menu_items[MENU_N];

int ui_setup(struct app *a);
void ui_teardown(struct app *a);
void ui_draw(struct app *a);
void ui_present(struct app *a);

/* Screen transition: kills the pty shell when leaving it, spawns it
 * when entering it (all opens/closes funnel through here). */
void app_goto(struct app *a, enum screen s);

/* Implemented by the screens (screens/home.c, screens/*.c) and overlays. */
void draw_body(struct app *a);
void draw_home_box(struct app *a);
void draw_modal(struct app *a);

/* layer-local text/put (clipped to the layer bounds) */
void ui_text(struct app *a, int layer, int lx, int ly, const char *str,
             int fg, int bg, int attr, int maxlen);
void ui_put(struct app *a, int layer, int lx, int ly, char ch, int fg,
            int bg, int attr);
void ui_fill(struct app *a, int layer, int lx, int ly, int w, int h, char ch,
             int fg, int bg, int attr);

/* Centered modal box on the modal layer; inner rect returned. */
void ui_modal_frame(struct app *a, const char *title, int w, int h,
                    int *ix, int *iy, int *iw, int *ih);
void ui_button(struct app *a, int layer, int cx, int ly, const char *label,
               int focused);

/* Health from reason + live exception escalation; updates the Signal. */
void health_poll(struct app *a);
const char *health_name(int level);
int health_fg(int level);

#endif /* EIGEN_RECOVERY_UI_H */
