/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * data.c - snapshot parsing + command runner. Userspace (musl OK).
 */

#define _DEFAULT_SOURCE /* popen */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "data.h"

void fmt_ago(long since, char *out, size_t n) {
    long d;
    if (since <= 0) {
        snprintf(out, n, "-");
        return;
    }
    d = (long)time(NULL) - since;
    if (d < 0)
        d = 0;
    if (d < 60)
        snprintf(out, n, "%lds ago", d);
    else if (d < 3600)
        snprintf(out, n, "%ldm ago", d / 60);
    else
        snprintf(out, n, "%ldh ago", d / 3600);
}

const char *svc_state_name(const struct svc *s) {
    return s->state;
}

/* Pack a service into a table row value (128 bytes: state, since,
 * generation, exit code, reason head). */
static size_t pack_svc(const struct svc *s, uint8_t *out) {
    memset(out, 0, 128);
    memcpy(out, s->state, 16);
    memcpy(out + 16, &s->since, sizeof s->since);
    memcpy(out + 24, &s->gen, sizeof s->gen);
    memcpy(out + 28, &s->exit_code, sizeof s->exit_code);
    strncpy((char *)out + 32, s->reason, 95);
    return 128;
}

void unpack_svc(const struct lat_row *row, char *state, long *since,
                int *gen, int *exit_code, char *reason, size_t rcap) {
    if (!row) {
        if (state)
            state[0] = '\0';
        if (since)
            *since = 0;
        if (gen)
            *gen = 0;
        if (exit_code)
            *exit_code = 0;
        if (reason && rcap)
            reason[0] = '\0';
        return;
    }
    if (state) {
        memcpy(state, row->val, 16);
        state[15] = '\0';
    }
    if (since)
        memcpy(since, row->val + 16, sizeof *since);
    if (gen)
        memcpy(gen, row->val + 24, sizeof *gen);
    if (exit_code)
        memcpy(exit_code, row->val + 28, sizeof *exit_code);
    if (reason && rcap) {
        size_t n = row->vlen > 32 ? row->vlen - 32 : 0;
        if (n >= rcap)
            n = rcap - 1;
        memcpy(reason, row->val + 32, n);
        reason[n] = '\0';
    }
}

static int parse_snapshot(const char *text, struct svc *out, int max) {
    int n = 0;
    const char *p = text;
    while (*p && n < max) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > 4 && memcmp(p, "SVC ", 4) == 0) {
            char tmp[512];
            struct svc *s = &out[n];
            char *q;
            char *sp;
            memset(s, 0, sizeof *s);
            if (len >= sizeof tmp)
                len = sizeof tmp - 1;
            memcpy(tmp, p, len);
            tmp[len] = '\0';
            q = tmp + 4;
            sp = strchr(q, ' ');
            if (!sp)
                goto next;
            *sp = '\0';
            strncpy(s->name, q, sizeof s->name - 1);
            q = sp + 1;
            sp = strchr(q, ' ');
            if (!sp)
                goto next;
            *sp = '\0';
            strncpy(s->state, q, sizeof s->state - 1);
            q = sp + 1;
            s->since = atol(q);
            {
                char *g = strstr(q, "gen=");
                char *x = strstr(q, "exit=");
                if (g)
                    s->gen = atoi(g + 4);
                if (x) {
                    s->exit_code = atoi(x + 5);
                    {
                        char *r = strchr(x, ' ');
                        if (r && r[1]) {
                            strncpy(s->reason, r + 1,
                                    sizeof s->reason - 1);
                            {
                                size_t rl = strlen(s->reason);
                                while (rl > 0 &&
                                       (s->reason[rl - 1] == '\n' ||
                                        s->reason[rl - 1] == '\r'))
                                    s->reason[--rl] = '\0';
                            }
                        }
                    }
                }
            }
            n++;
        }
    next:
        if (!eol)
            break;
        p = eol + 1;
    }
    return n;
}

static int read_state_file(char *buf, size_t cap) {
    FILE *f = fopen("/tmp/basis/state", "r");
    size_t n = 0;
    size_t r;
    if (!f)
        return -1;
    while (n + 1 < cap) {
        r = fread(buf + n, 1, cap - 1 - n, f);
        if (r == 0)
            break;
        n += r;
    }
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

int data_refresh(struct app *a) {
    char buf[8192];
    int n;
    struct svc svcs[SVC_MAX];
    int i;
    int changed = 0;
    n = read_state_file(buf, sizeof buf);
    if (n < 0)
        return 0; /* no snapshot yet; keep old table */
    if ((size_t)n == a->state_len &&
        memcmp(buf, a->state_cache, (size_t)n) == 0)
        return 0;
    memcpy(a->state_cache, buf, (size_t)n);
    a->state_len = (size_t)n;
    n = parse_snapshot(buf, svcs, SVC_MAX);
    /* rebuild the table in snapshot order */
    lat_table_init(&a->table, a->rows, SVC_MAX, a->subs, 4);
    for (i = 0; i < n; i++) {
        uint8_t packed[128];
        size_t len = pack_svc(&svcs[i], packed);
        lat_table_upsert(&a->table, svcs[i].name, packed, len);
    }
    if (a->nsvc != n)
        changed = 1;
    else {
        /* same count: generation still moves so the view refreshes */
        changed = 1;
    }
    a->nsvc = n;
    if (a->sel >= n && n > 0)
        a->sel = n - 1;
    if (changed)
        a->gen++;
    return changed;
}

int run_basisctl(char *const argv[], char *out, size_t cap) {
    int fds[2];
    pid_t pid;
    size_t n = 0;
    if (pipe(fds) != 0)
        return 127;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return 127;
    }
    if (pid == 0) {
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        execvp("basisctl", argv);
        _exit(127);
    }
    close(fds[1]);
    while (n + 1 < cap) {
        ssize_t r = read(fds[0], out + n, cap - 1 - n);
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    close(fds[0]);
    {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0)
            ;
        out[n] = '\0';
        if (WIFEXITED(status))
            return WEXITSTATUS(status);
        return 128;
    }
}

static int split_lines(char *buf, char ***out_lines) {
    int cap = 64;
    int n = 0;
    char **lines = malloc(sizeof(char *) * (size_t)cap);
    char *p = buf;
    if (!lines)
        return -1;
    while (*p) {
        char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char *line;
        if (n >= cap) {
            cap *= 2;
            lines = realloc(lines, sizeof(char *) * (size_t)cap);
            if (!lines)
                return -1;
        }
        line = malloc(len + 1);
        if (!line) {
            free_lines(lines, n);
            return -1;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        lines[n++] = line;
        if (!eol)
            break;
        p = eol + 1;
    }
    *out_lines = lines;
    return n;
}

void free_lines(char **lines, int n) {
    int i;
    if (!lines)
        return;
    for (i = 0; i < n; i++)
        free(lines[i]);
    free(lines);
}

int load_svc_log(const char *svc, char ***out_lines) {
    char slug[128];
    char path[192];
    size_t i = 0;
    FILE *f;
    /* cap: 128KB keeps the pager bounded */
    static const size_t cap = 128 * 1024;
    char *buf;
    size_t n = 0;
    int r;
    while (i + 1 < sizeof slug && svc[i]) {
        slug[i] = (svc[i] == '/') ? '_' : svc[i];
        i++;
    }
    slug[i] = '\0';
    snprintf(path, sizeof path, "/tmp/basis/log/%s.log", slug);
    f = fopen(path, "rb");
    if (!f)
        return -1;
    buf = malloc(cap + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    while (n < cap) {
        size_t k = fread(buf + n, 1, cap - n, f);
        if (k == 0)
            break;
        n += k;
    }
    fclose(f);
    buf[n] = '\0';
    r = split_lines(buf, out_lines);
    free(buf);
    return r;
}

int load_cmd_lines(char *const argv[], char ***out_lines) {
    static char buf[32768];
    int rc = run_basisctl(argv, buf, sizeof buf);
    int n;
    (void)rc;
    {
        char *copy = malloc(strlen(buf) + 1);
        if (!copy)
            return -1;
        strcpy(copy, buf);
        n = split_lines(copy, out_lines);
        free(copy);
        return n;
    }
}
