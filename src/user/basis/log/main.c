/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * log.c - basis-log: read per-service logs captured by basis-svcmgr.
 *
 * Usage: basis-log <service> [-n lines]   (default: whole file)
 *        basis-log --list
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "basis.h"

static void slug(const char *name, char *out, size_t n) {
    size_t i = 0;
    while (i + 1 < n && name[i]) {
        out[i] = (name[i] == '/') ? '_' : name[i];
        i++;
    }
    out[i] = '\0';
}

static int list_logs(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ls %s", BASIS_LOGDIR);
    return system(cmd);
}

/* Print the last n lines (n <= 0 means the whole file). */
static int tail_log(const char *path, long n) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "basis-log: no log for that service (%s)\n", path);
        return 1;
    }
    if (n <= 0) {
        int c;
        while ((c = fgetc(f)) != EOF) putchar(c);
        fclose(f);
        return 0;
    }
    /* Ring holding the file offsets of recent line starts. Line 1
     * starts at offset 0; the start of line k lands in slot k%(n+1). */
    long *offs = malloc(sizeof(long) * (size_t)(n + 1));
    if (!offs) { fclose(f); return 1; }
    long total = 0;
    int c, prev = '\n';
    while ((c = fgetc(f)) != EOF) {
        if (prev == '\n') {
            total++;
            offs[total % (n + 1)] = ftell(f) - 1;
        }
        prev = c;
    }
    /* The last n lines start at line (total-n+1); fewer lines: rewind. */
    if (total <= n) rewind(f);
    else fseek(f, offs[(total - n + 1) % (n + 1)], SEEK_SET);
    while ((c = fgetc(f)) != EOF) putchar(c);
    free(offs);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        fprintf(stderr, "usage: basis-log <service> [-n lines]\n       basis-log --list\n");
        return argc < 2 ? 1 : 0;
    }
    if (strcmp(argv[1], "--list") == 0) return list_logs();

    long n = 0;
    const char *svc = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            n = atol(argv[++i]);
        } else if (svc == NULL) {
            svc = argv[i];
        } else {
            fprintf(stderr, "usage: basis-log [-n lines] <service>\n       basis-log --list\n");
            return 1;
        }
    }
    if (!svc) {
        fprintf(stderr, "usage: basis-log [-n lines] <service>\n       basis-log --list\n");
        return 1;
    }
    char sl[BASIS_NAME_LEN], path[256];
    slug(svc, sl, sizeof(sl));
    snprintf(path, sizeof(path), "%s/%s.log", BASIS_LOGDIR, sl);
    return tail_log(path, n);
}
