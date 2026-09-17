/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * run.c - Service execution for basis-svcmgr.
 *
 * Oneshots run via fork/exec/wait with output captured to the service
 * log; failures back off exponentially inside the retry budget and park
 * in sticky maintenance past it.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include "svcmgr.h"
#include "color.h"

/* Run one oneshot; output goes to the service log. Returns exit code. */
static int run_oneshot(struct basis_service *s) {
    char sl[BASIS_NAME_LEN], logpath[256];
    slug(s->m.name, sl, sizeof(sl));
    snprintf(logpath, sizeof(logpath), "%s/%s.log", BASIS_LOGDIR, sl);

    FILE *lh = fopen(logpath, "w");
    if (lh) {
        fprintf(lh, "=== %s: %s ===\n", s->m.name, s->m.command);
        fclose(lh);
    }

    pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        int fd = open(logpath, O_WRONLY | O_APPEND);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) close(fd);
        }
        execl("/bin/sh", "sh", "-c", s->m.command, (char *)NULL);
        _exit(127);
    }
    s->pid = pid;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        /* retry on EINTR */
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + WTERMSIG(status);
}

static int wants_restart(struct basis_service *s, int code) {
    switch (s->m.restart) {
    case RR_NEVER: return 0;
    case RR_ALWAYS: return 1;
    case RR_ON_ABNORMAL: return code != 0;
    case RR_ON_FAILURE:
    default: return code != 0;
    }
}

static long backoff_delay(struct basis_service *s, int attempt) {
    long d = s->m.backoff_base;
    for (int i = 1; i < attempt; i++) {
        d *= s->m.backoff_mult;
        if (d >= s->m.backoff_cap) { d = s->m.backoff_cap; break; }
    }
    if (d < 1) d = 1;
    if (d > s->m.backoff_cap) d = s->m.backoff_cap;
    return d;
}

/* Re-run online dependents of s (restart-scope=dependents). Depth-capped
 * so two flapping services cannot ping-pong forever. */
static void cascade_dependents(struct basis_service *s, int depth);

static int start_service_depth(struct basis_service *s, int depth) {
    if (!s->m.enabled) {
        set_state(s, RS_DISABLED, NULL);
        write_state();
        return 0;
    }
    if (s->m.type == RT_DAEMON) {
        set_state(s, RS_OFFLINE, "daemon supervision needs scheduler");
        write_state();
        printf("  [SKIP] %-32s (daemon: parked until scheduler lands)\n", s->m.name);
        return 0;
    }
    if (!dep_online(s)) {
        char reason[BASIS_REASON_LEN];
        missing_dep_reason(s, reason, sizeof(reason));
        set_state(s, RS_OFFLINE, reason);
        write_state();
        return 0;
    }

    set_state(s, RS_STARTING, NULL);
    write_state();

    if (s->window_start == 0) s->window_start = time(NULL);
    int attempt = 0;
    for (;;) {
        attempt++;
        int code = run_oneshot(s);
        s->exit_code = code;
        if (code == 0) {
            int recovered = (attempt > 1);
            s->failures = 0;
            clear_maint(s);
            set_state(s, RS_ONLINE, NULL);
            write_state();
            cprintf(C_GREEN, "  [OK]   %s\n", s->m.name);
            if (recovered && s->m.restart_dependents && depth < 1)
                cascade_dependents(s, depth + 1);
            return 1;
        }
        /* Failure: budget check first (sticky maintenance beats retry). */
        long now = time(NULL);
        if (now - s->window_start > s->m.retry_window) {
            s->window_start = now;
            s->failures = 0;
        }
        s->failures++;
        if (s->failures >= s->m.retry_budget || !wants_restart(s, code)) {
            char reason[BASIS_REASON_LEN];
            if (s->failures >= s->m.retry_budget)
                snprintf(reason, sizeof(reason), "restart budget exceeded: %d failures in %ds (exit %d)",
                         s->failures, s->m.retry_window, code);
            else
                snprintf(reason, sizeof(reason), "failed (exit %d), restart=never", code);
            set_state(s, RS_MAINTENANCE, reason);
            write_maint(s);
            write_state();
            cprintf(C_RED, "  [FAIL] %s (exit %d, %s)\n", s->m.name, code, reason);
            return 0;
        }
        set_state(s, RS_RESTARTING, NULL);
        write_state();
        long delay = backoff_delay(s, attempt);
        printf("  [RETRY] %s in %lds (attempt %d, exit %d)\n", s->m.name, delay, attempt, code);
        struct timespec ts = { delay, 0 };
        nanosleep(&ts, NULL);
    }
}

static void cascade_dependents(struct basis_service *s, int depth) {
    for (int i = 0; i < nsvcs; i++) {
        if (svcs[i].state != RS_ONLINE) continue;
        for (int d = 0; d < svcs[i].m.n_require_online; d++) {
            if (strcmp(svcs[i].m.require_online[d], s->m.name) == 0) {
                cprintf(C_BLUE, "  [CASCADE] re-running %s (dependency %s recovered)\n",
                        svcs[i].m.name, s->m.name);
                svcs[i].state = RS_UNINIT;
                start_service_depth(&svcs[i], depth);
                break;
            }
        }
    }
}

int start_service(struct basis_service *s) {
    return start_service_depth(s, 0);
}
