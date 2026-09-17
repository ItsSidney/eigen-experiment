/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * status.c - basisctl read-only reporting: status and deps.
 */

#include <stdio.h>
#include <string.h>
#include "ctl.h"
#include "color.h"

int cmd_status(int argc, char **argv) {
    struct state_entry entries[BASIS_MAX_SERVICES];
    int n = state_load(entries, BASIS_MAX_SERVICES);
    if (n < 0) {
        fprintf(stderr, "basisctl: no state snapshot (%s); has basis-svcmgr run?\n", BASIS_STATE);
        return 1;
    }
    if (argc >= 1) {
        struct state_entry *e = state_find(entries, n, argv[0]);
        if (!e) {
            printf("basisctl: unknown service '%s'\n", argv[0]);
            return 1;
        }
        char since[32];
        fmt_ago(e->since, since, sizeof(since));
        printf("SERVICE      %s\n", e->name);
        printf("STATE        %s%s%s  (since %s)\n", state_color(e->state), e->state, color_off(), since);
        if (e->reason[0]) printf("REASON       %s\n", e->reason);
        printf("LAST EXIT    %d\n", e->exit_code);
        printf("GENERATION   %d\n", e->gen);
        load_manifests_dir(BASIS_ETC_DIR);
        load_manifests_dir(BASIS_OVERLAY);
        struct basis_manifest *m = find_manifest(argv[0]);
        if (m) {
            printf("DEPENDS ON");
            if (m->n_require_online == 0 && m->n_require_any == 0) printf("   none\n");
            else printf("\n");
            for (int i = 0; i < m->n_require_online; i++) {
                struct state_entry *d = state_find(entries, n, m->require_online[i]);
                const char *st = d ? d->state : "unknown";
                printf("             require-online: %-28s [%s%s%s]\n",
                       m->require_online[i], state_color(st), st, color_off());
            }
            for (int i = 0; i < m->n_require_any; i++) {
                struct state_entry *d = state_find(entries, n, m->require_any[i]);
                const char *st = d ? d->state : "unknown";
                printf("             require-any:    %-28s [%s%s%s]\n",
                       m->require_any[i], state_color(st), st, color_off());
            }
        }
        if (e->state[0] == 'm')
            printf("\nRun 'basisctl clear %s' after resolving the issue (takes effect next boot).\n", e->name);
        return 0;
    }
    color_init();
    if (basis_color) printf("%s", C_BOLD);
    printf("%-12s %-10s %s\n", "STATE", "SINCE", "SERVICE");
    if (basis_color) printf("%s", C_RESET);
    for (int i = 0; i < n; i++) {
        char since[32];
        fmt_ago(entries[i].since, since, sizeof(since));
        const char *c = state_color(entries[i].state);
        const char *off = color_off();
        if (entries[i].reason[0])
            printf("%s%-12s%s %-10s %s  (%s)\n", c, entries[i].state, off, since, entries[i].name, entries[i].reason);
        else
            printf("%s%-12s%s %-10s %s\n", c, entries[i].state, off, since, entries[i].name);
    }
    return 0;
}

static void deps_tree(struct state_entry *entries, int n, const char *name, const char *prefix, int depth) {
    if (depth > 8) return;
    struct basis_manifest *m = find_manifest(name);
    if (!m) return;
    for (int i = 0; i < m->n_require_online; i++) {
        struct state_entry *dep = state_find(entries, n, m->require_online[i]);
        const char *st = dep ? dep->state : "unknown";
        int last = (i == m->n_require_online - 1) && m->n_require_any == 0;
        printf("%s%s require-online  %-28s [%s%s%s]\n", prefix, last ? "\\-" : "|-",
               m->require_online[i], state_color(st), st, color_off());
        char sub[128];
        snprintf(sub, sizeof(sub), "%s%s ", prefix, last ? "  " : "| ");
        deps_tree(entries, n, m->require_online[i], sub, depth + 1);
    }
    if (m->n_require_any > 0) {
        printf("%s\\- require-any-online\n", prefix);
        char sub[128];
        snprintf(sub, sizeof(sub), "%s   ", prefix);
        for (int i = 0; i < m->n_require_any; i++) {
            struct state_entry *dep = state_find(entries, n, m->require_any[i]);
            const char *st = dep ? dep->state : "unknown";
            printf("%s|- %-30s [%s%s%s]\n", sub,
                   m->require_any[i], state_color(st), st, color_off());
        }
    }
}

int cmd_deps(char *name) {
    load_manifests_dir(BASIS_ETC_DIR);
    load_manifests_dir(BASIS_OVERLAY);
    struct state_entry entries[BASIS_MAX_SERVICES];
    int n = state_load(entries, BASIS_MAX_SERVICES);
    if (n < 0) {
        fprintf(stderr, "basisctl: no state snapshot (%s); has basis-svcmgr run?\n", BASIS_STATE);
        return 1;
    }
    if (!find_manifest(name)) {
        printf("basisctl: unknown service '%s'\n", name);
        return 1;
    }
    printf("%s\n", name);
    deps_tree(entries, n, name, "", 0);
    return 0;
}
