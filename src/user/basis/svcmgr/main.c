/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * main.c - basis-svcmgr entry point and milestone driver.
 *
 * Exit codes: 0 = console shell exited (core starts a fresh cycle),
 * 1 = nothing reached multi-user and no shell was started.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include "svcmgr.h"
#include "color.h"

static const char *r_tag(void) {
    color_init();
    return basis_color ? C_RESET C_BOLD C_BLUE "[basis] " C_RESET : "[basis] ";
}

static int milestone_reached(int idx, const char *until) {
    (void)until;
    color_init();
    if (basis_color)
        printf("%smilestone: " C_BOLD C_BLUE "%s" C_RESET " ................ reached\n",
               r_tag(), milestones[idx]);
    else
        printf("%smilestone: %s ................ reached\n", r_tag(), milestones[idx]);
    return 0;
}

static int read_boot_until(char *out, size_t n) {
    FILE *f = fopen(BASIS_BOOT_UNTIL, "r");
    if (!f) return 0;
    int ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) {
        size_t len = strlen(out);
        while (len > 0 && (out[len-1] == '\n' || out[len-1] == '\r' || out[len-1] == ' ')) out[--len] = '\0';
    }
    return ok && out[0];
}

int main(void) {
    /* Clear any attribute leaked by earlier boot output. */
    color_init();
    if (basis_color) printf("%s", C_RESET);
    ensure_runtime();
    load_dir(BASIS_ETC_DIR);
    load_dir(BASIS_OVERLAY);
    apply_overrides();

    if (nsvcs == 0) {
        printf("%sno manifests found in %s\n", r_tag(), BASIS_ETC_DIR);
        return 1;
    }
    if (check_cycles() != 0) {
        printf("%sdependency cycle detected; refusing to boot (run basisctl validate)\n", r_tag());
        return 1;
    }

    char until[32] = "";
    int read_until = read_boot_until(until, sizeof(until));
    int until_idx = read_until ? milestone_idx(until) : N_MILESTONES - 1;
    if (read_until && until_idx < 0) {
        printf("%sunknown boot-until milestone '%s'\n", r_tag(), until);
        return 1;
    }

    int online = 0, failed = 0;
    for (int m = 0; m <= until_idx; m++) {
        int started_here = 0;
        /* Fixpoint: attempt UNINIT services whose deps have all settled;
         * each attempt settles one more service, so this terminates. */
        for (;;) {
            int progress = 0;
            for (int i = 0; i < nsvcs; i++) {
                int mi = milestone_idx(svcs[i].m.milestone);
                if (mi < 0) {
                    if (svcs[i].state == RS_UNINIT) {
                        set_state(&svcs[i], RS_OFFLINE, "unknown milestone");
                        write_state();
                        failed++;
                        progress = 1;
                    }
                    continue;
                }
                if (mi != m || svcs[i].state != RS_UNINIT) continue;
                if (!deps_settled(&svcs[i])) continue;
                started_here = 1;
                progress = 1;
                if (start_service(&svcs[i])) online++;
                else if (svcs[i].state == RS_MAINTENANCE ||
                         (svcs[i].state == RS_OFFLINE && svcs[i].reason[0])) failed++;
            }
            if (!progress) break;
        }
        if (m > 0 || started_here) milestone_reached(m, until);
    }

    if (failed == 0)
        cprintf(C_GREEN, "[basis] boot complete (%d/%d services online)\n", online, nsvcs);
    else
        cprintf(C_RED, "[basis] boot complete (%d/%d services online, some failed (see basisctl status))\n",
                online, nsvcs);

    /* Console session: the machine needs an interactive shell, and under
     * the deferred execution model this doubles as supervision — when the
     * shell exits, the manager exits and core starts a fresh cycle. */
    pid_t sh = fork();
    if (sh == 0) {
        execl("/bin/sh", "sh", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(sh, &status, 0);
    printf("[basis] console shell exited\n");
    return 0;
}
