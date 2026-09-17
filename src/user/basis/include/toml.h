/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * toml.h - Tiny TOML-subset parser for Basis manifests.
 *
 * Supports: [section] headers, key = "string", key = 123,
 * key = true/false, key = ["a", "b"]. Everything else is ignored.
 * '#' starts a comment outside quotes.
 */

#ifndef BASIS_TOML_H
#define BASIS_TOML_H

#include "basis.h"

/* Parse one manifest file into m. Returns 0 on success, -1 on error
 * (missing name/command counts as an error). */
int toml_parse_manifest(const char *path, struct basis_manifest *m);

#endif /* BASIS_TOML_H */
