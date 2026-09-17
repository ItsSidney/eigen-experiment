/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * app.h - eigen-recovery shared state.
 *
 * Layout skeleton for every screen: stack(chrome, split(menu, body)).
 * The menu rail lives in the background layer on all screens; each
 * screen draws its body into the body layer; modals float on a
 * full-screen transparent layer with dim_behind (signature transition).
 */

#ifndef EIGEN_RECOVERY_APP_H
#define EIGEN_RECOVERY_APP_H

#include <stddef.h>
#include <stdint.h>

#include "lattice/lattice.h"
#include "term.h"
#include "data/data.h"
#include "shell/shell.h"

enum screen {
    SCR_HOME = 0,
    SCR_LOG,
    SCR_REGS,
    SCR_EXC,
    SCR_STORAGE,
    SCR_PCI,
    SCR_HEX,
    SCR_SHELL,
    NSCR
};

/* Modal overlay kinds. CONFIRM carries an action id (see below). */
enum modal {
    M_NONE = 0,
    M_CONFIRM,
    M_MESSAGE,
    M_PALETTE,
    M_FILTER,
    M_GOTO
};

#define ACT_NONE 0
#define ACT_REBOOT 1
#define ACT_EXPORT 2
#define ACT_FORCE_MOUNT 3

struct confirm {
    char title[64];
    char text[256];
    int action;
    int focus; /* 0 confirm/yes, 1 cancel/no */
};

#define HEALTH_OK 0
#define HEALTH_DEGRADED 1
#define HEALTH_CRITICAL 2

#define MENU_W 24

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

    struct sysdata data;
    struct lat_signal health;
    uint8_t health_buf[8];
    struct lat_sub health_subs[4];
    int health_level;
    char reason[128];
    unsigned long exc_at_start;

    enum screen screen;
    int menu_sel;
    char crumb_extra[64];

    /* per-screen state */
    struct {
        uint64_t anchor; /* stream offset of top line (live tail when ~0) */
        int paused;
        char filter[64];
    } log;
    struct {
        int frozen;
        char pinned[NREGS];
        char snap[NREGS][20];
        int has_snap;
        int sel;
    } regs;
    int exc_sel;
    int storage_sel;
    int pci_sel;
    struct {
        uint64_t addr;
        int annotate;
    } hex;
    struct shell_state shell;

    /* modal + line editor (palette / filter / goto) */
    enum modal modal;
    struct confirm confirm;
    char confirm_param[128];
    char message[512];
    char input[128];
    size_t input_len;
    size_t input_cur;
    int pal_sel;

    int dirty;
    unsigned long tick;
    unsigned long last_kmsg_tick;
};

const char *screen_name(enum screen s);
const char *health_name(int level);

#endif /* EIGEN_RECOVERY_APP_H */
