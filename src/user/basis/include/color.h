/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * color.h - Restrained ANSI colors for Basis output (OpenRC style).
 *
 * White for headings and neutral text, green for OK, red for failures,
 * blue for milestones and in-progress states. Colors are enabled only
 * when stdout is a terminal and NO_COLOR is unset, so redirected output
 * and log files stay plain text.
 */

#ifndef BASIS_COLOR_H
#define BASIS_COLOR_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define C_RESET   "\x1b[0m"
#define C_BOLD    "\x1b[1m"
#define C_WHITE   "\x1b[37m"
#define C_GREEN   "\x1b[32m"
#define C_BLUE    "\x1b[34m"
#define C_RED     "\x1b[31m"

static int basis_color = -1;

static void color_init(void) {
    if (basis_color >= 0) return;
    basis_color = 0;
    if (getenv("NO_COLOR")) return;
    if (isatty(STDOUT_FILENO)) basis_color = 1;
}

/* Printf wrapper: each line opens with a reset (so a leaked attribute
 * from anything printed earlier can never bleed in) and closes with one. */
#define cprintf(color, ...) do { \
    color_init(); \
    if (basis_color) printf("%s%s", C_RESET, color); \
    printf(__VA_ARGS__); \
    if (basis_color) printf("%s", C_RESET); \
} while (0)

#endif /* BASIS_COLOR_H */
