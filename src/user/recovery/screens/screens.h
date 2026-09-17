/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * screens/screens.h - one body + key handler per screen.
 * Implemented across screens/*.c; dispatched by draw_body (home.c).
 */

#ifndef EIGEN_RECOVERY_SCREENS_H
#define EIGEN_RECOVERY_SCREENS_H

#include "app.h"

void draw_log_body(struct app *a);
int log_key(struct app *a, struct lat_keyev ev);
void draw_regs_body(struct app *a);
int regs_key(struct app *a, struct lat_keyev ev);
void draw_exc_body(struct app *a);
int exc_key(struct app *a, struct lat_keyev ev);
void draw_storage_body(struct app *a);
int storage_key(struct app *a, struct lat_keyev ev);
void draw_pci_body(struct app *a);
int pci_key(struct app *a, struct lat_keyev ev);
void draw_hex_body(struct app *a);
int hex_key(struct app *a, struct lat_keyev ev);
void draw_shell_body(struct app *a);

#endif /* EIGEN_RECOVERY_SCREENS_H */
