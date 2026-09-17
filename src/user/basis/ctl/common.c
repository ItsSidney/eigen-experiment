/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * common.c - Shared manifest store and display helpers for basisctl.
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "ctl.h"
#include "toml.h"
#include "color.h"

static struct basis_manifest manifests[BASIS_MAX_SERVICES];
static int nmanifests = 0;

void slug(const char *name, char *out, size_t n) {
    size_t i = 0;
    while (i + 1 < n && name[i]) {
        out[i] = (name[i] == '/') ? '_' : name[i];
        i++;
    }
    out[i] = '\0';
}

int has_suffix(const char *name, const char *suf) {
    size_t n = strlen(name), m = strlen(suf);
    return n > m && strcmp(name + n - m, suf) == 0;
}

void load_manifests_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!has_suffix(e->d_name, ".toml")) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct basis_manifest m;
        if (toml_parse_manifest(path, &m) != 0) continue;
        int k;
        for (k = 0; k < nmanifests; k++) {
            if (strcmp(manifests[k].name, m.name) == 0) {
                manifests[k] = m;
                break;
            }
        }
        if (k == nmanifests && nmanifests < BASIS_MAX_SERVICES)
            manifests[nmanifests++] = m;
    }
    closedir(d);
}

struct basis_manifest *find_manifest(const char *name) {
    for (int i = 0; i < nmanifests; i++) {
        if (strcmp(manifests[i].name, name) == 0) return &manifests[i];
    }
    return NULL;
}

const char *state_color(const char *state) {
    color_init();
    if (!basis_color) return "";
    if (strcmp(state, "online") == 0) return C_RESET C_GREEN;
    if (strcmp(state, "maintenance") == 0) return C_RESET C_RED;
    if (strcmp(state, "starting") == 0) return C_RESET C_BLUE;
    if (strcmp(state, "restarting") == 0) return C_RESET C_BLUE;
    if (strcmp(state, "uninit") == 0) return C_RESET C_BLUE;
    return ""; /* offline, disabled, unknown: plain white */
}

const char *color_off(void) {
    return basis_color ? C_RESET : "";
}
