/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * dag.c - Dependency graph checks for basis-svcmgr.
 */

#include <string.h>
#include <stdio.h>
#include "svcmgr.h"

int milestone_idx(const char *m) {
    if (!m || !m[0]) return 3; /* no milestone named: default target */
    for (int i = 0; i < N_MILESTONES; i++) {
        if (strcmp(m, milestones[i]) == 0) return i;
    }
    return -1;
}

/* Depth-first cycle check over require-online edges. */
static int visit[BASIS_MAX_SERVICES];

static int dfs_cycle(int i) {
    visit[i] = 1; /* on stack */
    for (int d = 0; d < svcs[i].m.n_require_online; d++) {
        struct basis_service *dep = find_svc(svcs[i].m.require_online[d]);
        if (!dep) continue;
        int j = (int)(dep - svcs);
        if (visit[j] == 1) return -1;
        if (visit[j] == 0 && dfs_cycle(j) != 0) return -1;
    }
    visit[i] = 2;
    return 0;
}

int check_cycles(void) {
    memset(visit, 0, sizeof(visit));
    for (int i = 0; i < nsvcs; i++) {
        if (visit[i] == 0 && dfs_cycle(i) != 0) return -1;
    }
    return 0;
}

int dep_online(struct basis_service *s) {
    for (int d = 0; d < s->m.n_require_online; d++) {
        struct basis_service *dep = find_svc(s->m.require_online[d]);
        if (!dep || dep->state != RS_ONLINE) return 0;
    }
    if (s->m.n_require_any > 0) {
        int any = 0;
        for (int d = 0; d < s->m.n_require_any; d++) {
            struct basis_service *dep = find_svc(s->m.require_any[d]);
            if (dep && dep->state == RS_ONLINE) { any = 1; break; }
        }
        if (!any) return 0;
    }
    return 1;
}

void missing_dep_reason(struct basis_service *s, char *out, size_t n) {
    snprintf(out, n, "waiting on:");
    for (int d = 0; d < s->m.n_require_online; d++) {
        struct basis_service *dep = find_svc(s->m.require_online[d]);
        if (!dep || dep->state != RS_ONLINE) {
            strncat(out, " ", n - strlen(out) - 1);
            strncat(out, s->m.require_online[d], n - strlen(out) - 1);
        }
    }
}

/* A dependency is settled when it can never change state again this pass. */
static int dep_settled(struct basis_service *dep) {
    return dep->state == RS_ONLINE || dep->state == RS_OFFLINE ||
           dep->state == RS_DISABLED || dep->state == RS_MAINTENANCE;
}

int deps_settled(struct basis_service *s) {
    for (int d = 0; d < s->m.n_require_online; d++) {
        struct basis_service *dep = find_svc(s->m.require_online[d]);
        if (dep && !dep_settled(dep)) return 0;
    }
    for (int d = 0; d < s->m.n_require_any; d++) {
        struct basis_service *dep = find_svc(s->m.require_any[d]);
        if (dep && !dep_settled(dep)) return 0;
    }
    return 1;
}
