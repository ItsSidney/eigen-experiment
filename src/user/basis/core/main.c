/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * core.c - basis-core: Basis PID 1. Deliberately tiny.
 *
 * Reaps zombies, restarts a crashed service manager, and never exits on
 * signals. If the manager fails repeatedly it falls back to /bin/sh so
 * the machine always reaches a usable console.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include "color.h"

static const char *r_tag(void) {
    color_init();
    return basis_color ? C_RESET C_BOLD C_BLUE "[basis] " C_RESET : "[basis] ";
}

#define SVCMGR_PATH "/usr/basisd/sbin/basis-svcmgr"
#define FALLBACK_SHELL "/bin/sh"
#define MAX_CRASHES 3
#define CRASH_WINDOW 60

static void usage(void) {
    fprintf(stderr, "usage: basis-core\n");
}

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage();
        return 0;
    }

    /* PID 1 ignores stop/terminal signals; only SIGKILL/SIGSTOP bite. */
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    if (getpid() != 1)
        fprintf(stderr, "[basis] warning: running as pid %d, not init\n", getpid());

    printf("%score started (pid 1)\n", r_tag());

    time_t crashes[MAX_CRASHES];
    int ncrash = 0;

    for (;;) {
        pid_t mgr = fork();
        if (mgr < 0) {
            perror("[basis] fork");
            sleep(5);
            continue;
        }
        if (mgr == 0) {
            execl(SVCMGR_PATH, "basis-svcmgr", (char *)NULL);
            perror("[basis] execl basis-svcmgr");
            _exit(127);
        }

        int status = 0;
        pid_t w;
        /* Reap everything; the manager is the only child we restart. */
        while ((w = waitpid(-1, &status, 0)) > 0) {
            if (w == mgr) break;
        }

        time_t now = time(NULL);
        printf("%smanager pid %d exited (status %d)\n", r_tag(), mgr, status);

        /* Track rapid crashes; a flapping manager means boot is broken. */
        if (ncrash == MAX_CRASHES) {
            memmove(crashes, crashes + 1, sizeof(time_t) * (MAX_CRASHES - 1));
            ncrash--;
        }
        crashes[ncrash++] = now;
        if (ncrash == MAX_CRASHES && now - crashes[0] < CRASH_WINDOW) {
            cprintf(C_RED, "[basis] manager failed %d times in %ds; falling back to %s\n",
                    MAX_CRASHES, CRASH_WINDOW, FALLBACK_SHELL);
            execl(FALLBACK_SHELL, "sh", (char *)NULL);
            perror("[basis] execl fallback shell");
            return 127;
        }

        printf("%srestarting manager in 1s\n", r_tag());
        sleep(1);
    }
    return 0;
}
