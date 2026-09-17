/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * overlays/overlays.h - modal layer: confirm/message/palette/filter/goto.
 */

#ifndef EIGEN_RECOVERY_OVERLAYS_H
#define EIGEN_RECOVERY_OVERLAYS_H

#include "app.h"

void show_message(struct app *a, const char *text);
void show_confirm(struct app *a, const char *title, const char *text,
                  int action);
void overlays_show_help(struct app *a);
void draw_modal(struct app *a);
/* Modal key routing; returns 1 when consumed. */
int modal_key(struct app *a, struct lat_keyev ev);

#endif /* EIGEN_RECOVERY_OVERLAYS_H */
