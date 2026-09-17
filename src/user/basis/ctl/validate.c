/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * validate.c - basisctl manifest checking.
 *
 * Catches parse errors, duplicate names, unknown dependencies and
 * dependency cycles before they can ever brick a boot.
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "ctl.h"
#include "toml.h"

int validate_dir(const char *dir, int verbose) {
    /* Static: 64 manifests far exceed the 64KB user stack. */
    static struct basis_manifest ms[BASIS_MAX_SERVICES];
    int n = 0, errors = 0;
    DIR *d = opendir(dir);
    if (!d) {
        if (verbose) printf("basisctl: cannot open %s\n", dir);
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!has_suffix(e->d_name, ".toml")) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (n >= BASIS_MAX_SERVICES) break;
        if (toml_parse_manifest(path, &ms[n]) != 0) {
            printf("basisctl: %s: parse error (need name + command)\n", path);
            errors++;
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (strcmp(ms[i].name, ms[n].name) == 0) {
                printf("basisctl: duplicate service '%s' (%s)\n", ms[n].name, path);
                errors++;
            }
        }
        n++;
    }
    closedir(d);
    /* Unknown dependency targets are errors. */
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < ms[i].n_require_online; k++) {
            int found = 0;
            for (int j = 0; j < n; j++) {
                if (strcmp(ms[j].name, ms[i].require_online[k]) == 0) { found = 1; break; }
            }
            if (!found) {
                printf("basisctl: %s: unknown require-online '%s'\n", ms[i].name, ms[i].require_online[k]);
                errors++;
            }
        }
    }
    /* Iterative cycle check over require-online edges. */
    int state[BASIS_MAX_SERVICES] = {0};
    for (int i = 0; i < n; i++) {
        int stack[BASIS_MAX_SERVICES], top = 0, onstack[BASIS_MAX_SERVICES] = {0};
        stack[top++] = i;
        onstack[i] = 1;
        while (top > 0) {
            int cur = stack[top - 1];
            int advanced = 0;
            for (int k = 0; k < ms[cur].n_require_online; k++) {
                int j = -1;
                for (int t = 0; t < n; t++) {
                    if (strcmp(ms[t].name, ms[cur].require_online[k]) == 0) { j = t; break; }
                }
                if (j < 0) continue;
                if (onstack[j]) {
                    printf("basisctl: dependency cycle involving '%s' and '%s'\n",
                           ms[cur].name, ms[j].name);
                    errors++;
                    top = 0;
                    break;
                }
                if (state[j] == 0) {
                    state[j] = 1;
                    onstack[j] = 1;
                    stack[top++] = j;
                    advanced = 1;
                    break;
                }
            }
            if (!advanced && top > 0) {
                onstack[stack[--top]] = 0;
            }
        }
        state[i] = 2;
    }
    if (errors == 0 && verbose) printf("basisctl: %s OK (%d services)\n", dir, n);
    return errors;
}
