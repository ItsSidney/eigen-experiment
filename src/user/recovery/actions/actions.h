/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * actions/actions.h - confirm-backed actions: export, reboot, force-mount.
 */

#ifndef EIGEN_RECOVERY_ACTIONS_H
#define EIGEN_RECOVERY_ACTIONS_H

#include "app.h"

void do_export(struct app *a);
void do_reboot(struct app *a);
void do_force_mount(struct app *a, const char *src, const char *mp);

#endif /* EIGEN_RECOVERY_ACTIONS_H */
