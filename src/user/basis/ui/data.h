/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * data.h - snapshot parsing + basisctl runner for basisui.
 */

#ifndef BASISUI_DATA_H
#define BASISUI_DATA_H

#include <stddef.h>

#include "app.h"

/* Reload /tmp/basis/state into the Table. Returns 1 when anything
 * changed (bump generation, redraw). */
int data_refresh(struct app *a);

/* Unpack one table row back into fields. */
void unpack_svc(const struct lat_row *row, char *state, long *since,
                int *gen, int *exit_code, char *reason, size_t rcap);

/* "37s ago" style age. */
void fmt_ago(long since, char *out, size_t n);

/* Run basisctl <args...> capturing combined output. Returns exit code,
 * output (possibly truncated) in out. */
int run_basisctl(char *const argv[], char *out, size_t cap);

/* Load a service log (/tmp/basis/log/<slug>.log) into malloc'd lines.
 * Caller frees each line + the array. Returns line count, -1 on error. */
int load_svc_log(const char *svc, char ***out_lines);

/* Load arbitrary command output (validate/deps/detail) into lines. */
int load_cmd_lines(char *const argv[], char ***out_lines);

void free_lines(char **lines, int n);

#endif /* BASISUI_DATA_H */
