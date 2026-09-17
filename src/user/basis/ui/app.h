/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * app.h - basisui shared state.
 *
 * Single table screen (services from /tmp/basis/state, bound to an L2
 * Table) plus text-viewer modals (detail / deps / log / validate output)
 * and a confirm modal for disable/clear. Mutations shell out to
 * basisctl so CLI and TUI can never disagree.
 */

#ifndef BASISUI_APP_H
#define BASISUI_APP_H

#include <stddef.h>
#include <stdint.h>

#include "lattice.h"
#include "term.h"

#define SVC_NAME_LEN 64
#define SVC_STATE_LEN 16
#define SVC_REASON_LEN 160
#define SVC_MAX 64

struct svc {
    char name[SVC_NAME_LEN];
    char state[SVC_STATE_LEN];
    long since;
    int gen;
    int exit_code;
    char reason[SVC_REASON_LEN];
};

/* Modal overlay kinds. */
enum bmodal {
    BM_NONE = 0,
    BM_TEXT,    /* scrollable text viewer (detail/deps/log/validate) */
    BM_CONFIRM, /* destructive mutations */
    BM_HELP,
};

struct app {
    struct lat_term term;
    struct lat_surface surf;
    struct lat_comp comp;
    struct lat_cell *front;
    struct lat_cell *back;
    struct lat_cell *bgcells;
    struct lat_cell *bodycells;
    struct lat_cell *modalcells;
    int lay_bg;
    int lay_body;
    int lay_modal;
    int body_x;
    int body_y;
    int body_w;
    int body_h;

    /* services bound to an L2 Table (key = name) */
    struct lat_table table;
    struct lat_row rows[SVC_MAX];
    struct lat_sub subs[4];
    int nsvc;
    int sel;
    uint64_t gen;

    /* text viewer modal */
    enum bmodal modal;
    char view_title[64];
    char **view_lines;
    int view_n;
    int view_top;
    /* confirm modal */
    char confirm_title[64];
    char confirm_text[256];
    int confirm_focus;
    char confirm_cmd[32];
    char confirm_arg[64];

    char state_cache[8192];
    size_t state_len;

    int dirty;
    unsigned long tick;
};

const char *svc_state_name(const struct svc *s);

#endif /* BASISUI_APP_H */
