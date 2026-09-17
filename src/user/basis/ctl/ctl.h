/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * ctl.h - Internal interface of basisctl.
 */

#ifndef BASIS_CTL_H
#define BASIS_CTL_H

#include "basis.h"
#include "state.h"

/* common.c: shared manifest store and small helpers. */
void slug(const char *name, char *out, size_t n);
int has_suffix(const char *name, const char *suf);
void load_manifests_dir(const char *dir);
struct basis_manifest *find_manifest(const char *name);
const char *state_color(const char *state);
const char *color_off(void);

/* status.c */
int cmd_status(int argc, char **argv);
int cmd_deps(char *name);

/* mutate.c */
int write_override(const char *name, int enabled);
int cmd_clear(char *name);
int cmd_apply(char *path);
int cmd_rollback(char *name, const char *genarg);
int cmd_boot_until(char *m);
int cmd_tzset(char *zone);

/* validate.c: returns error count. */
int validate_dir(const char *dir, int verbose);

#endif /* BASIS_CTL_H */
