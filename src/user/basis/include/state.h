/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * state.h - Reading the Basis state snapshot (/tmp/basis/state).
 *
 * Shared by basisctl and anything else that reports on services.
 */

#ifndef BASIS_STATE_H
#define BASIS_STATE_H

#include "basis.h"

struct state_entry {
    char name[BASIS_NAME_LEN];
    char state[16];
    long since;
    int gen;
    int exit_code;
    char reason[BASIS_REASON_LEN];
};

/* Load the snapshot. Returns 0 on success, -1 if there is none yet. */
int state_load(struct state_entry *out, int max);

/* Find one service in a loaded table. Returns NULL when unknown. */
struct state_entry *state_find(struct state_entry *table, int n, const char *name);

/* "37s ago" / "2m ago" / "3h ago" / "-" into out. */
void fmt_ago(long since, char *out, size_t n);

#endif /* BASIS_STATE_H */
