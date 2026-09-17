/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * basis.h - Shared definitions for the Basis service supervisor.
 *
 * Layout at runtime (all under /tmp, which is tmpfs):
 *   /etc/basis/manifests dir  shipped manifests (read-only root)
 *   /tmp/basis/overlay dir     applied manifests, shadow /etc versions
 *   /tmp/basis/override/<name>     one-line "enabled=0|1" admin overrides
 *   /tmp/basis/generations/<slug>/<N>.toml  immutable applied versions
 *   /tmp/basis/maint/<slug>        sticky maintenance marker (reason inside)
 *   /tmp/basis/log/<slug>.log      per-service output
 *   /tmp/basis/state               text snapshot for basisctl
 */

#ifndef BASIS_H
#define BASIS_H

#define BASIS_ETC_DIR    "/etc/basis/manifests"
#define BASIS_RUNTIME    "/tmp/basis"
#define BASIS_OVERLAY    "/tmp/basis/overlay"
#define BASIS_OVERRIDE   "/tmp/basis/override"
#define BASIS_GENERATIONS "/tmp/basis/generations"
#define BASIS_MAINT      "/tmp/basis/maint"
#define BASIS_LOGDIR     "/tmp/basis/log"
#define BASIS_STATE      "/tmp/basis/state"
#define BASIS_BOOT_UNTIL "/tmp/basis/boot-until"

#define BASIS_MAX_SERVICES  64
#define BASIS_MAX_DEPS      16
#define BASIS_NAME_LEN      64
#define BASIS_CMD_LEN       256
#define BASIS_REASON_LEN    160

enum basis_state {
    RS_UNINIT = 0,
    RS_OFFLINE,
    RS_STARTING,
    RS_ONLINE,
    RS_RESTARTING,
    RS_DISABLED,
    RS_MAINTENANCE,
};

enum basis_restart {
    RR_NEVER = 0,
    RR_ON_FAILURE,
    RR_ALWAYS,
    RR_ON_ABNORMAL,
};

enum basis_type {
    RT_ONESHOT = 0,
    RT_DAEMON,
};

struct basis_manifest {
    char name[BASIS_NAME_LEN];
    char description[128];
    int type;               /* enum basis_type */
    char command[BASIS_CMD_LEN];
    char require_online[BASIS_MAX_DEPS][BASIS_NAME_LEN];
    int n_require_online;
    char require_any[BASIS_MAX_DEPS][BASIS_NAME_LEN];
    int n_require_any;
    char optional[BASIS_MAX_DEPS][BASIS_NAME_LEN];
    int n_optional;
    char milestone[32];
    int restart;            /* enum basis_restart */
    int retry_budget;
    int retry_window;       /* seconds */
    int backoff_base;
    int backoff_mult;
    int backoff_cap;
    int restart_dependents; /* restart-scope: 0=self, 1=dependents */
    char user[32];
    int enabled;            /* default 1 */
    int generation;
};

struct basis_service {
    struct basis_manifest m;
    int state;              /* enum basis_state */
    int degraded;           /* orthogonal flag */
    long since;             /* time() of last transition */
    char reason[BASIS_REASON_LEN];
    int failures;
    long window_start;
    int pid;                /* last child pid */
    int exit_code;
};

const char *basis_state_name(int state);

#endif /* BASIS_H */
