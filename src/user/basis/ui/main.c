/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * main.c - basisui: service console for Basis.
 *
 * One table screen bound to the state snapshot, text-viewer modals for
 * detail/deps/log/validate output, confirm modals for disable/clear.
 * Mutations go through basisctl; the table refreshes from the snapshot
 * every second. Exit 0 always.
 */

#define _DEFAULT_SOURCE /* strcasecmp */

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "app.h"
#include "ui.h"
#include "data.h"

/* ── table screen ─────────────────────────────────────────────────── */

static void selected_svc(struct app *a, char *name) {
    int seen = 0;
    int r;
    name[0] = '\0';
    for (r = 0; r < a->table.maxrows; r++) {
        if (!a->table.rows[r].used)
            continue;
        if (seen == a->sel) {
            strncpy(name, a->table.rows[r].key, SVC_NAME_LEN - 1);
            return;
        }
        seen++;
    }
}

void draw_screen(struct app *a) {    int w = a->body_w;
    int h = a->body_h;
    int y = 0;
    int r;
    int idx = 0;
    ui_text(a, a->lay_body, 0, y++, "  SERVICE                   STATE       "
                                    "SINCE       DETAIL",
            TH_FG, TH_BG, LAT_ATTR_BOLD, w);
    for (r = 0; r < a->table.maxrows && y < h; r++) {
        struct lat_row *row;
        char state[16];
        long since = 0;
        int gen = 0;
        int ec = 0;
        char reason[SVC_REASON_LEN];
        char ago[16];
        char line[192];
        int cursor;
        if (!a->table.rows[r].used)
            continue;
        row = &a->table.rows[r];
        unpack_svc(row, state, &since, &gen, &ec, reason, sizeof reason);
        (void)gen;
        fmt_ago(since, ago, sizeof ago);
        if (reason[0])
            snprintf(line, sizeof line, "%-25.25s %-11.11s %-11.11s %s",
                     row->key, state, ago, reason);
        else if (ec != 0)
            snprintf(line, sizeof line, "%-25.25s %-11.11s %-11.11s "
                                        "exit %d",
                     row->key, state, ago, ec);
        else
            snprintf(line, sizeof line, "%-25.25s %-11.11s %-11.11s",
                     row->key, state, ago);
        cursor = (idx == a->sel);
        {
            /* state dot at col 0, text from col 2 -- same rhythm both ways */
            char dot = 0x0A;
            int dotfg = cursor ? TH_SEL_FG : state_fg(state);
            int dotbg = cursor ? TH_SEL_BG : TH_BG;
            int k;
            int len;
            ui_put(a, a->lay_body, 0, y, dot, dotfg, dotbg,
                   LAT_ATTR_BOLD);
            if (cursor) {
                len = (int)strlen(line);
                for (k = 0; k + 2 < w; k++) {
                    char ch = k < len ? line[k] : ' ';
                    ui_put(a, a->lay_body, 2 + k, y, ch, TH_SEL_FG,
                           TH_SEL_BG, LAT_ATTR_BOLD);
                }
            } else {
                ui_text(a, a->lay_body, 2, y, line, TH_FG, TH_BG, 0,
                        w - 2);
            }
        }
        idx++;
        y++;
    }
    if (a->nsvc == 0 && y < h)
        ui_text(a, a->lay_body, 2, y++,
                "no state snapshot (has basis-svcmgr run?)", TH_FG, TH_BG,
                LAT_ATTR_DIM, w - 2);
}

/* ── viewer modal ─────────────────────────────────────────────────── */

static void view_open(struct app *a, const char *title, char **lines,
                      int n) {
    int i;
    for (i = 0; i < a->view_n; i++)
        free(a->view_lines[i]);
    free(a->view_lines);
    a->view_lines = lines;
    a->view_n = n < 0 ? 0 : n;
    a->view_top = 0;
    strncpy(a->view_title, title, sizeof a->view_title - 1);
    a->modal = BM_TEXT;
    a->dirty = 1;
}

static void view_close(struct app *a) {
    int i;
    for (i = 0; i < a->view_n; i++)
        free(a->view_lines[i]);
    free(a->view_lines);
    a->view_lines = NULL;
    a->view_n = 0;
    a->view_top = 0;
    if (a->modal == BM_TEXT)
        a->modal = BM_NONE;
    a->dirty = 1;
}

void draw_modal(struct app *a) {
    int ix, iy, iw, ih;
    int i;
    int y;
    if (a->modal == BM_TEXT) {
        int w = a->term.cols - 12;
        int h = a->term.rows - 8;
        char pos[32];
        if (w > 72)
            w = 72;
        if (h > 22)
            h = 22;
        if (w < 40)
            w = 40;
        if (h < 10)
            h = 10;
        ui_modal_frame(a, a->view_title, w, h, &ix, &iy, &iw, &ih);
        if (a->view_top > a->view_n - 1)
            a->view_top = a->view_n > 0 ? a->view_n - 1 : 0;
        if (a->view_top < 0)
            a->view_top = 0;
        y = iy + 1;
        for (i = a->view_top; i < a->view_n && y < iy + ih; i++) {
            ui_text(a, a->lay_modal, ix, y++, a->view_lines[i], 0, 7, 0,
                    iw);
        }
        snprintf(pos, sizeof pos, "[%d/%d]", a->view_top + 1,
                 a->view_n > 0 ? a->view_n : 1);
        ui_text(a, a->lay_modal, ix + iw - (int)strlen(pos) - 1,
                iy + ih - 1, pos, 0, 7, 0, 20);
    } else if (a->modal == BM_CONFIRM) {
        char *line;
        char *save = NULL;
        char copy[256];
        ui_modal_frame(a, a->confirm_title, 52, 10, &ix, &iy, &iw, &ih);
        strncpy(copy, a->confirm_text, sizeof copy - 1);
        copy[sizeof copy - 1] = '\0';
        y = iy + 1;
        line = strtok_r(copy, "\n", &save);
        while (line && y < iy + ih - 2) {
            ui_text(a, a->lay_modal, ix, y++, line, 0, 7, 0, iw);
            line = strtok_r(NULL, "\n", &save);
        }
        ui_button(a, a->lay_modal, ix + iw / 2 - 10, iy + ih - 1,
                  "Confirm", a->confirm_focus == 0);
        ui_button(a, a->lay_modal, ix + iw / 2 + 10, iy + ih - 1, "Cancel",
                  a->confirm_focus == 1);
    } else if (a->modal == BM_HELP) {
        ui_modal_frame(a, "HELP", 52, 14, &ix, &iy, &iw, &ih);
        {
            static const char *lines[] = {
                "Up/Down   move selection",
                "Enter     service detail",
                "L         service log",
                "D         dependency tree",
                "E / X     enable / disable",
                "C         clear maintenance flag",
                "V         validate manifests",
                "PgUp/PgDn scroll viewer",
                "?         this help",
                "Q / Esc   quit",
            };
            unsigned k;
            y = iy + 1;
            for (k = 0; k < sizeof lines / sizeof lines[0] &&
                        y < iy + ih - 1;
                 k++)
                ui_text(a, a->lay_modal, ix, y++, lines[k], 0, 7, 0, iw);
        }
    }
}

/* ── actions ──────────────────────────────────────────────────────── */

static void open_text(struct app *a, const char *title, char **lines,
                      int n) {
    if (n < 0 || !lines) {
        char **one = malloc(sizeof(char *));
        if (!one)
            return;
        one[0] = malloc(64);
        if (!one[0]) {
            free(one);
            return;
        }
        snprintf(one[0], 64, "(no output)");
        view_open(a, title, one, 1);
        return;
    }
    if (n == 0) {
        char **one = malloc(sizeof(char *));
        if (!one)
            return;
        one[0] = malloc(64);
        if (!one[0]) {
            free(one);
            return;
        }
        snprintf(one[0], 64, "(empty)");
        view_open(a, title, one, 1);
        return;
    }
    view_open(a, title, lines, n);
}

static void do_detail(struct app *a) {
    char name[SVC_NAME_LEN];
    char *argv[] = { "basisctl", "status", name, NULL };
    char **lines;
    int n;
    char title[96];
    selected_svc(a, name);
    if (!name[0])
        return;
    snprintf(title, sizeof title, "DETAIL %s", name);
    n = load_cmd_lines(argv, &lines);
    open_text(a, title, lines, n);
}

static void do_deps(struct app *a) {
    char name[SVC_NAME_LEN];
    char *argv[] = { "basisctl", "deps", name, NULL };
    char **lines;
    int n;
    char title[96];
    selected_svc(a, name);
    if (!name[0])
        return;
    snprintf(title, sizeof title, "DEPS %s", name);
    n = load_cmd_lines(argv, &lines);
    open_text(a, title, lines, n);
}

static void do_log(struct app *a) {
    char name[SVC_NAME_LEN];
    char **lines;
    int n;
    char title[96];
    selected_svc(a, name);
    if (!name[0])
        return;
    snprintf(title, sizeof title, "LOG %s", name);
    n = load_svc_log(name, &lines);
    if (n < 0) {
        lines = malloc(sizeof(char *));
        if (!lines)
            return;
        lines[0] = malloc(64);
        if (!lines[0]) {
            free(lines);
            return;
        }
        snprintf(lines[0], 64, "(no log file yet)");
        view_open(a, title, lines, 1);
        return;
    }
    open_text(a, title, lines, n);
}

static void do_validate(struct app *a) {
    char *argv[] = { "basisctl", "validate", NULL };
    char **lines;
    int n = load_cmd_lines(argv, &lines);
    open_text(a, "VALIDATE", lines, n);
}

static void do_mutate(struct app *a, const char *cmd) {
    char name[SVC_NAME_LEN];
    char *argv[] = { "basisctl", (char *)cmd, name, NULL };
    char out[2048];
    int rc;
    char title[96];
    char **lines;
    int n;
    selected_svc(a, name);
    if (!name[0])
        return;
    rc = run_basisctl(argv, out, sizeof out);
    snprintf(title, sizeof title, "%s %s (rc %d)", cmd, name, rc);
    {
        char *copy = malloc(strlen(out) + 1);
        char *p;
        int cap = 16;
        if (!copy)
            return;
        strcpy(copy, out);
        lines = malloc(sizeof(char *) * (size_t)cap);
        if (!lines) {
            free(copy);
            return;
        }
        n = 0;
        p = strtok(copy, "\n");
        while (p) {
            if (n >= cap) {
                cap *= 2;
                lines = realloc(lines, sizeof(char *) * (size_t)cap);
                if (!lines) {
                    free(copy);
                    return;
                }
            }
            lines[n] = malloc(strlen(p) + 1);
            if (!lines[n]) {
                free(copy);
                free_lines(lines, n);
                return;
            }
            strcpy(lines[n++], p);
            p = strtok(NULL, "\n");
        }
        free(copy);
    }
    open_text(a, title, lines, n);
    data_refresh(a);
    a->dirty = 1;
}

static void ask_confirm(struct app *a, const char *cmd) {
    char name[SVC_NAME_LEN];
    selected_svc(a, name);
    if (!name[0])
        return;
    snprintf(a->confirm_title, sizeof a->confirm_title, "CONFIRM %s", cmd);
    snprintf(a->confirm_text, sizeof a->confirm_text,
             "%s service '%s'?\n\nTakes effect as documented\n"
             "by basisctl.",
             cmd, name);
    strncpy(a->confirm_cmd, cmd, sizeof a->confirm_cmd - 1);
    strncpy(a->confirm_arg, name, sizeof a->confirm_arg - 1);
    a->confirm_focus = 0;
    a->modal = BM_CONFIRM;
    a->dirty = 1;
}

/* ── keys ─────────────────────────────────────────────────────────── */

static int modal_key(struct app *a, struct lat_keyev ev) {
    if (a->modal == BM_TEXT) {
        int page = a->body_h > 4 ? a->body_h - 4 : 4;
        if (ev.key == KEY_ESC || ev.key == KEY_NONE) {
            view_close(a);
            return 1;
        }
        if (ev.key == KEY_UP && a->view_top > 0) {
            a->view_top--;
            a->dirty = 1;
        } else if (ev.key == KEY_DOWN) {
            a->view_top++;
            a->dirty = 1;
        } else if (ev.key == KEY_PGUP) {
            a->view_top -= page;
            if (a->view_top < 0)
                a->view_top = 0;
            a->dirty = 1;
        } else if (ev.key == KEY_PGDN) {
            a->view_top += page;
            a->dirty = 1;
        } else if (ev.key == KEY_HOME) {
            a->view_top = 0;
            a->dirty = 1;
        } else if (ev.key == KEY_END) {
            a->view_top = 1 << 28;
            a->dirty = 1;
        }
        return 1;
    }
    if (a->modal == BM_CONFIRM) {
        if (ev.key == KEY_LEFT || ev.key == KEY_RIGHT || ev.key == KEY_TAB) {
            a->confirm_focus = 1 - a->confirm_focus;
            a->dirty = 1;
            return 1;
        }
        if (ev.key == KEY_ENTER) {
            if (a->confirm_focus == 0) {
                char cmd[32];
                strncpy(cmd, a->confirm_cmd, sizeof cmd - 1);
                cmd[sizeof cmd - 1] = '\0';
                a->modal = BM_NONE;
                do_mutate(a, cmd);
            } else {
                a->modal = BM_NONE;
                a->dirty = 1;
            }
            return 1;
        }
        if (ev.key == KEY_ESC) {
            a->modal = BM_NONE;
            a->dirty = 1;
            return 1;
        }
        return 1;
    }
    if (a->modal == BM_HELP) {
        if (ev.key == KEY_ENTER || ev.key == KEY_ESC || ev.key == KEY_NONE ||
            (ev.key == KEY_CHAR && ev.ch == '?')) {
            a->modal = BM_NONE;
            a->dirty = 1;
        }
        return 1;
    }
    return 0;
}

/* returns 1 to quit */
static int table_key(struct app *a, struct lat_keyev ev) {
    if (ev.key == KEY_UP) {
        if (a->sel > 0)
            a->sel--;
        a->dirty = 1;
        return 0;
    }
    if (ev.key == KEY_DOWN) {
        if (a->sel + 1 < a->nsvc)
            a->sel++;
        a->dirty = 1;
        return 0;
    }
    if (ev.key == KEY_HOME) {
        a->sel = 0;
        a->dirty = 1;
        return 0;
    }
    if (ev.key == KEY_END) {
        if (a->nsvc > 0)
            a->sel = a->nsvc - 1;
        a->dirty = 1;
        return 0;
    }
    if (ev.key == KEY_ENTER) {
        do_detail(a);
        return 0;
    }
    if (ev.key == KEY_ESC || ev.key == KEY_CTRL_C ||
        (ev.key == KEY_CHAR && (ev.ch == 'q' || ev.ch == 'Q'))) {
        return 1;
    }
    if (ev.key != KEY_CHAR)
        return 0;
    switch (ev.ch) {
    case 'l':
    case 'L':
        do_log(a);
        break;
    case 'd':
        do_deps(a);
        break;
    case 'e':
    case 'E':
        do_mutate(a, "enable");
        break;
    case 'x':
    case 'X':
        ask_confirm(a, "disable");
        break;
    case 'c':
    case 'C':
        ask_confirm(a, "clear");
        break;
    case 'v':
    case 'V':
        do_validate(a);
        break;
    case '?':
        a->modal = BM_HELP;
        a->dirty = 1;
        break;
    default:
        break;
    }
    return 0;
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    struct app a;
    struct pollfd pfd;
    int quit = 0;
    unsigned long last_present = (unsigned long)-2;
    (void)argc;
    (void)argv;
    memset(&a, 0, sizeof a);
    lat_term_init(&a.term, STDOUT_FILENO, STDIN_FILENO);
    if (ui_setup(&a) != 0) {
        fprintf(stderr, "basisui: need at least 80x24\n");
        return 1;
    }
    data_refresh(&a);
    a.dirty = 1;
    lat_term_raw_on(&a.term);
    lat_term_enter(&a.term);
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    while (!quit) {
        int pr = poll(&pfd, 1, 100);
        int interacted = 0;
        a.tick++;
        if (pr > 0) {
            struct lat_keyev ev = lat_term_read_key(&a.term);
            if (ev.key == KEY_NONE && !a.term.raw_ok) {
                quit = 1;
            } else {
                interacted = 1;
                if (a.modal != BM_NONE) {
                    modal_key(&a, ev);
                } else {
                    if (table_key(&a, ev))
                        quit = 1;
                }
            }
        }
        if (a.tick % 10 == 0) {
            if (data_refresh(&a))
                a.dirty = 1;
        }
        if (a.dirty && (interacted || a.tick - last_present >= 2)) {
            ui_present(&a);
            a.dirty = 0;
            last_present = a.tick;
        }
    }
    view_close(&a);
    ui_teardown(&a);
    lat_term_leave(&a.term, 0);
    lat_term_raw_off(&a.term);
    return 0;
}
