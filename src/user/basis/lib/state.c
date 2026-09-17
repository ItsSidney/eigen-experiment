/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * state.c - Reading the Basis state snapshot.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "state.h"

void fmt_ago(long since, char *out, size_t n) {
    if (since <= 0) { snprintf(out, n, "-"); return; }
    long d = time(NULL) - since;
    if (d < 0) d = 0;
    if (d < 60) snprintf(out, n, "%lds ago", d);
    else if (d < 3600) snprintf(out, n, "%ldm ago", d / 60);
    else snprintf(out, n, "%ldh ago", d / 3600);
}

int state_load(struct state_entry *out, int max) {
    FILE *f = fopen(BASIS_STATE, "r");
    if (!f) return -1;
    int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) && n < max) {
        if (strncmp(line, "SVC ", 4) != 0) continue;
        struct state_entry *e = &out[n];
        memset(e, 0, sizeof(*e));
        char *p = line + 4;
        char *sp = strchr(p, ' ');
        if (!sp) continue;
        size_t namelen = (size_t)(sp - p);
        if (namelen >= sizeof(e->name)) namelen = sizeof(e->name) - 1;
        memcpy(e->name, p, namelen);
        e->name[namelen] = '\0';
        p = sp + 1;
        sp = strchr(p, ' ');
        if (!sp) continue;
        *sp = '\0';
        strncpy(e->state, p, sizeof(e->state) - 1);
        p = sp + 1;
        e->since = atol(p);
        char *g = strstr(p, "gen=");
        char *x = strstr(p, "exit=");
        if (g) e->gen = atoi(g + 4);
        if (x) {
            e->exit_code = atoi(x + 5);
            char *r = strchr(x, ' ');
            if (r && r[1]) {
                strncpy(e->reason, r + 1, sizeof(e->reason) - 1);
                size_t len = strlen(e->reason);
                while (len > 0 && (e->reason[len-1] == '\n' || e->reason[len-1] == '\r'))
                    e->reason[--len] = '\0';
            }
        }
        n++;
    }
    fclose(f);
    return n;
}

struct state_entry *state_find(struct state_entry *table, int n, const char *name) {
    for (int i = 0; i < n; i++) {
        if (strcmp(table[i].name, name) == 0) return &table[i];
    }
    return NULL;
}
