/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * toml.c - Tiny TOML-subset parser for Basis manifests.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include "toml.h"

/* Strip trailing whitespace and an optional trailing comment. */
static void clean_line(char *line) {
    int inq = 0;
    for (size_t i = 0; line[i]; i++) {
        if (line[i] == '"') inq = !inq;
        if (line[i] == '#' && !inq) {
            line[i] = '\0';
            break;
        }
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n-1] == ' ' || line[n-1] == '\t' ||
                     line[n-1] == '\n' || line[n-1] == '\r'))
        line[--n] = '\0';
}

/* Copy the first quoted string in s into dst. Returns 0 on success. */
static int quoted(const char *s, char *dst, size_t n) {
    const char *a = strchr(s, '"');
    if (!a) return -1;
    a++;
    const char *b = strchr(a, '"');
    if (!b) return -1;
    size_t len = (size_t)(b - a);
    if (len >= n) len = n - 1;
    memcpy(dst, a, len);
    dst[len] = '\0';
    return 0;
}

/* Parse ["a", "b", ...] into out[][64]. Returns element count or -1. */
static int str_array(const char *s, char out[][BASIS_NAME_LEN], int max) {
    const char *p = strchr(s, '[');
    const char *end = p ? strchr(p, ']') : NULL;
    if (!p || !end) return -1;
    p++;
    int n = 0;
    while (n < max) {
        const char *a = strchr(p, '"');
        if (!a || a >= end) break;
        a++;
        const char *b = strchr(a, '"');
        if (!b || b > end) break;
        size_t len = (size_t)(b - a);
        if (len >= BASIS_NAME_LEN) len = BASIS_NAME_LEN - 1;
        memcpy(out[n], a, len);
        out[n][len] = '\0';
        n++;
        p = b + 1;
    }
    return n;
}

static int parse_int(const char *s, int *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s) return -1;
    *out = (int)v;
    return 0;
}

static void manifest_defaults(struct basis_manifest *m) {
    memset(m, 0, sizeof(*m));
    m->type = RT_ONESHOT;
    m->restart = RR_ON_FAILURE;
    m->retry_budget = 5;
    m->retry_window = 60;
    m->backoff_base = 1;
    m->backoff_mult = 2;
    m->backoff_cap = 30;
    m->enabled = 1;
    m->generation = 1;
    strcpy(m->user, "root");
}

int toml_parse_manifest(const char *path, struct basis_manifest *m) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    manifest_defaults(m);

    char line[512];
    char section[32] = "";
    while (fgets(line, sizeof(line), f)) {
        clean_line(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') continue;
        if (*p == '[') {
            char *e = strchr(p, ']');
            if (!e) continue;
            size_t len = (size_t)(e - p - 1);
            if (len >= sizeof(section)) len = sizeof(section) - 1;
            memcpy(section, p + 1, len);
            section[len] = '\0';
            continue;
        }
        if (strcmp(section, "service") != 0) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        char *ke = key + strlen(key);
        while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = '\0';
        while (*val == ' ' || *val == '\t') val++;

        char tmp[256];
        if (strcmp(key, "name") == 0) {
            if (quoted(val, m->name, sizeof(m->name)) != 0) { fclose(f); return -1; }
        } else if (strcmp(key, "description") == 0) {
            quoted(val, m->description, sizeof(m->description));
        } else if (strcmp(key, "type") == 0) {
            if (quoted(val, tmp, sizeof(tmp)) == 0 && strcmp(tmp, "daemon") == 0)
                m->type = RT_DAEMON;
        } else if (strcmp(key, "command") == 0) {
            if (quoted(val, m->command, sizeof(m->command)) != 0) { fclose(f); return -1; }
        } else if (strcmp(key, "require-online") == 0) {
            int n = str_array(val, m->require_online, BASIS_MAX_DEPS);
            if (n >= 0) m->n_require_online = n;
        } else if (strcmp(key, "require-any-online") == 0) {
            int n = str_array(val, m->require_any, BASIS_MAX_DEPS);
            if (n >= 0) m->n_require_any = n;
        } else if (strcmp(key, "optional") == 0) {
            int n = str_array(val, m->optional, BASIS_MAX_DEPS);
            if (n >= 0) m->n_optional = n;
        } else if (strcmp(key, "milestone") == 0) {
            quoted(val, m->milestone, sizeof(m->milestone));
        } else if (strcmp(key, "restart") == 0) {
            if (quoted(val, tmp, sizeof(tmp)) == 0) {
                if (strcmp(tmp, "never") == 0) m->restart = RR_NEVER;
                else if (strcmp(tmp, "always") == 0) m->restart = RR_ALWAYS;
                else if (strcmp(tmp, "on-abnormal-exit") == 0) m->restart = RR_ON_ABNORMAL;
                else m->restart = RR_ON_FAILURE;
            }
        } else if (strcmp(key, "retry-budget") == 0) {
            parse_int(val, &m->retry_budget);
        } else if (strcmp(key, "retry-window") == 0) {
            parse_int(val, &m->retry_window);
        } else if (strcmp(key, "backoff-base") == 0) {
            parse_int(val, &m->backoff_base);
        } else if (strcmp(key, "backoff-mult") == 0) {
            parse_int(val, &m->backoff_mult);
        } else if (strcmp(key, "backoff-cap") == 0) {
            parse_int(val, &m->backoff_cap);
        } else if (strcmp(key, "restart-scope") == 0) {
            if (quoted(val, tmp, sizeof(tmp)) == 0 && strcmp(tmp, "dependents") == 0)
                m->restart_dependents = 1;
        } else if (strcmp(key, "user") == 0) {
            quoted(val, m->user, sizeof(m->user));
        } else if (strcmp(key, "enabled") == 0) {
            if (strncasecmp(val, "true", 4) == 0 || val[0] == '1') m->enabled = 1;
            else if (strncasecmp(val, "false", 5) == 0 || val[0] == '0') m->enabled = 0;
        } else if (strcmp(key, "generation") == 0) {
            parse_int(val, &m->generation);
        }
    }
    fclose(f);
    if (m->name[0] == '\0' || m->command[0] == '\0') return -1;
    return 0;
}

const char *basis_state_name(int state) {
    switch (state) {
    case RS_UNINIT: return "uninit";
    case RS_OFFLINE: return "offline";
    case RS_STARTING: return "starting";
    case RS_ONLINE: return "online";
    case RS_RESTARTING: return "restarting";
    case RS_DISABLED: return "disabled";
    case RS_MAINTENANCE: return "maintenance";
    default: return "unknown";
    }
}
