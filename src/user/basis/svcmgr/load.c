/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * load.c - Manifest loading for basis-svcmgr.
 *
 * /etc carries the shipped set; /tmp/basis/overlay shadows it entry by
 * entry so applied generations win without touching the read-only root.
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "svcmgr.h"
#include "toml.h"

struct basis_service svcs[BASIS_MAX_SERVICES];
int nsvcs = 0;

const char *milestones[] = {
    "single-user", "filesystems", "network", "multi-user", "graphical",
};

void slug(const char *name, char *out, size_t n) {
    size_t i = 0;
    while (i + 1 < n && name[i]) {
        out[i] = (name[i] == '/') ? '_' : name[i];
        i++;
    }
    out[i] = '\0';
}

struct basis_service *find_svc(const char *name) {
    for (int i = 0; i < nsvcs; i++) {
        if (strcmp(svcs[i].m.name, name) == 0) return &svcs[i];
    }
    return NULL;
}

void ensure_runtime(void) {
    mkdir(BASIS_RUNTIME, 0755);
    mkdir(BASIS_OVERLAY, 0755);
    mkdir(BASIS_OVERRIDE, 0755);
    mkdir(BASIS_GENERATIONS, 0755);
    mkdir(BASIS_MAINT, 0755);
    mkdir(BASIS_LOGDIR, 0755);
}

static int has_suffix(const char *name, const char *suf) {
    size_t n = strlen(name), m = strlen(suf);
    return n > m && strcmp(name + n - m, suf) == 0;
}

/* Load dir into the table; entries with a duplicate name replace in place. */
int load_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!has_suffix(e->d_name, ".toml")) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct basis_manifest m;
        if (toml_parse_manifest(path, &m) != 0) {
            fprintf(stderr, "[basis] warning: skipping bad manifest %s\n", path);
            continue;
        }
        struct basis_service *old = find_svc(m.name);
        if (old) {
            int gen = old->m.generation;
            old->m = m;
            if (old->m.generation <= gen) old->m.generation = gen + 1;
        } else {
            if (nsvcs >= BASIS_MAX_SERVICES) {
                fprintf(stderr, "[basis] too many services, ignoring %s\n", m.name);
                continue;
            }
            memset(&svcs[nsvcs], 0, sizeof(svcs[nsvcs]));
            svcs[nsvcs].m = m;
            svcs[nsvcs].state = RS_UNINIT;
            nsvcs++;
        }
    }
    closedir(d);
    return 0;
}

/* Admin override: /tmp/basis/override/<slug> contains "enabled=0|1". */
void apply_overrides(void) {
    for (int i = 0; i < nsvcs; i++) {
        char path[256], sl[BASIS_NAME_LEN], buf[32];
        slug(svcs[i].m.name, sl, sizeof(sl));
        snprintf(path, sizeof(path), "%s/%s", BASIS_OVERRIDE, sl);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(buf, sizeof(buf), f) && strstr(buf, "enabled=0"))
            svcs[i].m.enabled = 0;
        else if (strstr(buf, "enabled=1"))
            svcs[i].m.enabled = 1;
        fclose(f);
    }
}
