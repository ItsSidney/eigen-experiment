/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * state.c - Service state tracking for basis-svcmgr.
 *
 * Every transition is snapshotted to /tmp/basis/state for basisctl,
 * and maintenance entries leave a sticky marker under /tmp/basis/maint.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "svcmgr.h"

void set_state(struct basis_service *s, int state, const char *reason) {
    s->state = state;
    s->since = time(NULL);
    if (reason) {
        strncpy(s->reason, reason, sizeof(s->reason) - 1);
        s->reason[sizeof(s->reason) - 1] = '\0';
    } else {
        s->reason[0] = '\0';
    }
}

/* Rewrite the whole snapshot. Written directly: Eigen has no rename()
 * yet, so no atomic swap; the file is small and rewritten after every
 * transition. */
void write_state(void) {
    FILE *f = fopen(BASIS_STATE, "w");
    if (!f) return;
    fprintf(f, "# basis state v1\n");
    for (int i = 0; i < nsvcs; i++) {
        fprintf(f, "SVC %s %s %ld gen=%d exit=%d %s\n",
                svcs[i].m.name, basis_state_name(svcs[i].state),
                (long)svcs[i].since, svcs[i].m.generation,
                svcs[i].exit_code, svcs[i].reason);
    }
    fclose(f);
}

void write_maint(struct basis_service *s) {
    char path[256], sl[BASIS_NAME_LEN];
    slug(s->m.name, sl, sizeof(sl));
    snprintf(path, sizeof(path), "%s/%s", BASIS_MAINT, sl);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", s->reason);
        fclose(f);
    }
}

void clear_maint(struct basis_service *s) {
    char path[256], sl[BASIS_NAME_LEN];
    slug(s->m.name, sl, sizeof(sl));
    snprintf(path, sizeof(path), "%s/%s", BASIS_MAINT, sl);
    unlink(path);
}
