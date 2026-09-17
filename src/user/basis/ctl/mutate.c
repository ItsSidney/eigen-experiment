/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * mutate.c - basisctl mutations.
 *
 * Everything here writes into /tmp/basis and takes effect at the next
 * boot: the manager only runs at boot time until Eigen grows a
 * preemptive scheduler for a resident supervisor.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "ctl.h"
#include "toml.h"

int write_override(const char *name, int enabled) {
    char sl[BASIS_NAME_LEN], path[256];
    slug(name, sl, sizeof(sl));
    mkdir(BASIS_OVERRIDE, 0755);
    snprintf(path, sizeof(path), "%s/%s", BASIS_OVERRIDE, sl);
    FILE *f = fopen(path, "w");
    if (!f) {
        perror("basisctl");
        return 1;
    }
    fprintf(f, "enabled=%d\n", enabled);
    fclose(f);
    printf("%s %s (takes effect next boot)\n", enabled ? "enabled" : "disabled", name);
    return 0;
}

int cmd_clear(char *name) {
    char sl[BASIS_NAME_LEN], path[256];
    slug(name, sl, sizeof(sl));
    snprintf(path, sizeof(path), "%s/%s", BASIS_MAINT, sl);
    if (unlink(path) != 0) {
        printf("basisctl: %s is not in maintenance (or already cleared)\n", name);
        return 1;
    }
    printf("cleared %s (retries next boot)\n", name);
    return 0;
}

/* Copy file src -> dst. Returns 0 on success. */
static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "r");
    if (!in) return -1;
    FILE *out = fopen(dst, "w");
    if (!out) { fclose(in); return -1; }
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
    }
    fclose(in);
    fclose(out);
    return 0;
}

/* basisctl tzset <Zone/City>: point the whole system at a timezone.
 *
 * Validates the zone against the shipped IANA database, then writes
 * /etc/timezone (the kernel reads it per exec and exports TZ=) and copies the
 * compiled zone to /etc/localtime (musl's fallback path). Both writes use the
 * tarfs COW layer, so no reboot is needed. */
int cmd_tzset(char *zone) {
    if (!zone || !zone[0]) {
        printf("basisctl: usage: basisctl tzset <Zone/City>\n");
        return 1;
    }
    /* Reject anything with .. or a leading slash: the zone must resolve
     * inside /usr/share/zoneinfo, never outside it. */
    if (zone[0] == '/' || strstr(zone, "..") != NULL) {
        printf("basisctl: not a zone name: %s\n", zone);
        return 1;
    }

    char src[256];
    snprintf(src, sizeof(src), "/usr/share/zoneinfo/%s", zone);
    FILE *in = fopen(src, "r");
    if (!in) {
        printf("basisctl: unknown zone '%s' (not under /usr/share/zoneinfo)\n",
               zone);
        return 1;
    }
    fclose(in);

    FILE *f = fopen("/etc/timezone", "w");
    if (!f) {
        perror("basisctl: /etc/timezone");
        return 1;
    }
    fprintf(f, "%s\n", zone);
    fclose(f);

    /* /etc/localtime is the fallback path; keep it in step, but a failure
     * here is not fatal since the TZ variable already covers musl. */
    int copied = copy_file(src, "/etc/localtime") == 0;

    printf("timezone set to %s (new processes pick it up immediately)\n",
           zone);
    if (!copied)
        printf("warning: could not refresh /etc/localtime; TZ= still applies\n");
    return 0;
}

static int current_gen(const char *name) {
    load_manifests_dir(BASIS_ETC_DIR);
    load_manifests_dir(BASIS_OVERLAY);
    struct basis_manifest *m = find_manifest(name);
    return m ? m->generation : 0;
}

int cmd_apply(char *path) {
    struct basis_manifest m;
    if (toml_parse_manifest(path, &m) != 0) {
        printf("basisctl: cannot parse %s (need name + command)\n", path);
        return 1;
    }
    char sl[BASIS_NAME_LEN], ovpath[256], gendir[128], genpath[256];
    slug(m.name, sl, sizeof(sl));
    mkdir(BASIS_RUNTIME, 0755);
    mkdir(BASIS_OVERLAY, 0755);
    snprintf(gendir, sizeof(gendir), "%s/%s", BASIS_GENERATIONS, sl);
    mkdir(BASIS_GENERATIONS, 0755);
    mkdir(gendir, 0755);

    /* Stash the effective manifest being replaced, if any. */
    int gen = current_gen(m.name);
    if (gen > 0) {
        char cur[256];
        struct stat st;
        snprintf(cur, sizeof(cur), "%s/%s.toml", BASIS_OVERLAY, sl);
        if (stat(cur, &st) != 0) {
            /* find the /etc source file holding this service */
            cur[0] = '\0';
            DIR *d = opendir(BASIS_ETC_DIR);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) != NULL) {
                    if (!has_suffix(e->d_name, ".toml")) continue;
                    char p[512];
                    snprintf(p, sizeof(p), "%s/%s", BASIS_ETC_DIR, e->d_name);
                    struct basis_manifest t;
                    if (toml_parse_manifest(p, &t) == 0 && strcmp(t.name, m.name) == 0) {
                        strcpy(cur, p);
                        break;
                    }
                }
                closedir(d);
            }
        }
        if (cur[0]) {
            snprintf(genpath, sizeof(genpath), "%s/%d.toml", gendir, gen);
            copy_file(cur, genpath);
        }
    }

    snprintf(ovpath, sizeof(ovpath), "%s/%s.toml", BASIS_OVERLAY, sl);
    if (copy_file(path, ovpath) != 0) {
        printf("basisctl: cannot install %s\n", path);
        return 1;
    }
    FILE *f = fopen(ovpath, "a");
    if (f) {
        fprintf(f, "\n[service]\ngeneration = %d\n", gen + 1);
        fclose(f);
    }
    printf("applied %s as generation %d (takes effect next boot)\n", m.name, gen + 1);
    return 0;
}

int cmd_rollback(char *name, const char *genarg) {
    char sl[BASIS_NAME_LEN], gendir[128], ovpath[256];
    slug(name, sl, sizeof(sl));
    snprintf(gendir, sizeof(gendir), "%s/%s", BASIS_GENERATIONS, sl);
    snprintf(ovpath, sizeof(ovpath), "%s/%s.toml", BASIS_OVERLAY, sl);

    int cur = current_gen(name);
    int want = -1;
    if (genarg) {
        want = atoi(genarg);
    } else {
        /* Newest generation older than the current overlay. */
        DIR *d = opendir(gendir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (!has_suffix(e->d_name, ".toml")) continue;
                int g = atoi(e->d_name);
                if (g < cur && g > want) want = g;
            }
            closedir(d);
        }
        if (want < 0) {
            /* No older generation: drop the overlay to reveal /etc. */
            if (unlink(ovpath) == 0) {
                printf("rolled back %s to the shipped manifest (takes effect next boot)\n", name);
                return 0;
            }
            printf("basisctl: nothing to roll back for %s\n", name);
            return 1;
        }
    }
    char src[256];
    snprintf(src, sizeof(src), "%s/%d.toml", gendir, want);
    if (copy_file(src, ovpath) != 0) {
        printf("basisctl: generation %d not found for %s\n", want, name);
        return 1;
    }
    printf("rolled back %s to generation %d (takes effect next boot)\n", name, want);
    return 0;
}

int cmd_boot_until(char *m) {
    const char *ok[] = {"single-user", "filesystems", "network", "multi-user", "graphical"};
    for (int i = 0; i < 5; i++) {
        if (strcmp(m, ok[i]) == 0) {
            mkdir(BASIS_RUNTIME, 0755);
            FILE *f = fopen(BASIS_BOOT_UNTIL, "w");
            if (!f) { perror("basisctl"); return 1; }
            fprintf(f, "%s\n", m);
            fclose(f);
            printf("next boot will stop at milestone '%s'\n", m);
            return 0;
        }
    }
    printf("basisctl: unknown milestone '%s'\n", m);
    return 1;
}
