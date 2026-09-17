/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * shell.h - background pty shell for eigen-recovery.
 *
 * The shell is a real /bin/sh on a pty pair. Its output pumps into an
 * L2 Stream whether or not the shell screen is focused, so a watch
 * loop started here keeps running while the operator checks the log.
 * TAB/ESC stay with the app (focus/menu); everything else forwards.
 */

#ifndef EIGEN_RECOVERY_SHELL_H
#define EIGEN_RECOVERY_SHELL_H

#include <stddef.h>
#include <stdint.h>

#include "lattice/lattice.h"

#define SHELL_CAP (32 * 1024)

struct shell_state {
    int running; /* child alive */
    int master;  /* pty master fd, -1 when down */
    int pid;
    struct lat_stream out;
    uint8_t *buf;
    struct lat_sub *subs;
};

/* Spawn (or respawn) the shell. Returns 0 on success. */
int shell_spawn(struct shell_state *s);
/* Pump available output into the stream. Returns 1 when new bytes moved. */
int shell_pump(struct shell_state *s);
/* Forward one byte to the shell. */
void shell_send(struct shell_state *s, char c);
void shell_free(struct shell_state *s);

#endif /* EIGEN_RECOVERY_SHELL_H */
