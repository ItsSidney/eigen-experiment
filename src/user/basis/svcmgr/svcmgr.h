/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * svcmgr.h - Internal interface of basis-svcmgr.
 */

#ifndef SVCMGR_H
#define SVCMGR_H

#include "basis.h"

extern struct basis_service svcs[BASIS_MAX_SERVICES];
extern int nsvcs;

extern const char *milestones[];
#define N_MILESTONES 5

/* load.c */
void ensure_runtime(void);
int load_dir(const char *dir);
void apply_overrides(void);
void slug(const char *name, char *out, size_t n);
struct basis_service *find_svc(const char *name);

/* dag.c */
int milestone_idx(const char *m);
int check_cycles(void);
int dep_online(struct basis_service *s);
int deps_settled(struct basis_service *s);
void missing_dep_reason(struct basis_service *s, char *out, size_t n);

/* state.c */
void set_state(struct basis_service *s, int state, const char *reason);
void write_state(void);
void write_maint(struct basis_service *s);
void clear_maint(struct basis_service *s);

/* run.c */
int start_service(struct basis_service *s);

#endif /* SVCMGR_H */
