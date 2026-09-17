/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * main.c - eigen-recovery entry, event loop, key routing.
 *
 * Usage: eigen-recovery [reason]     (or --reason <text>)
 * Exit 0 always (diagnostic tool, not a test).
 */

#define _DEFAULT_SOURCE

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "app.h"
#include "ui/ui.h"
#include "screens/screens.h"
#include "overlays/overlays.h"
#include "actions/actions.h"


/* ── menu activation ──────────────────────────────────────────────── */

static void menu_activate(struct app *a, int idx) {
    const struct menu_item *m = &menu_items[idx];
    if (m->action == ACT_EXPORT) {
        do_export(a);
        return;
    }
    if (m->action == ACT_REBOOT) {
        show_confirm(a, "CONFIRM REBOOT", "Reboot now? Unsaved state is lost.",
                     ACT_REBOOT);
        return;
    }
    app_goto(a, m->screen);
}

/* returns 1 to quit */
static int menu_key(struct app *a, struct lat_keyev ev) {
    int i;
    if (ev.key == KEY_UP) {
        a->menu_sel = (a->menu_sel + MENU_N - 1) % MENU_N;
        a->dirty = 1;
        return 0;
    }
    if (ev.key == KEY_DOWN) {
        a->menu_sel = (a->menu_sel + 1) % MENU_N;
        a->dirty = 1;
        return 0;
    }
    if (ev.key == KEY_ENTER) {
        menu_activate(a, a->menu_sel);
        return 0;
    }
    if (ev.key == KEY_ESC || ev.key == KEY_CTRL_C) {
        return 1;
    }
    if (ev.key == KEY_CHAR) {
        if (ev.ch == '?') {
            overlays_show_help(a);
            a->dirty = 1;
            return 0;
        }
        if (ev.ch == '/') {
            a->input[0] = '\0';
            a->input_len = 0;
            a->input_cur = 0;
            a->pal_sel = 0;
            a->modal = M_PALETTE;
            a->dirty = 1;
            return 0;
        }
        for (i = 0; i < MENU_N; i++) {
            if (menu_items[i].key == ev.ch) {
                a->menu_sel = i;
                menu_activate(a, i);
                return 0;
            }
        }
    }
    return 0;
}

/* body key routing; returns 1 to quit */
static int body_key(struct app *a, struct lat_keyev ev) {
    /* shell owns nearly everything; Tab/Esc close it and go home */
    if (a->screen == SCR_SHELL) {
        if (ev.key == KEY_TAB || ev.key == KEY_ESC) {
            app_goto(a, SCR_HOME);
            return 0;
        }
        if (ev.key == KEY_ENTER) {
            if (!a->shell.running)
                shell_spawn(&a->shell);
            else
                shell_send(&a->shell, '\r');
            a->dirty = 1;
            return 0;
        }
        if (ev.key == KEY_CHAR) {
            shell_send(&a->shell, ev.ch);
            a->dirty = 1;
            return 0;
        }
        if (ev.key == KEY_CTRL_C) {
            shell_send(&a->shell, 0x03);
            return 0;
        }
        if (ev.key == KEY_BACKSPACE) {
            shell_send(&a->shell, 0x7F);
            return 0;
        }
        if (ev.key == KEY_UP)
            shell_send(&a->shell, 0x1B), shell_send(&a->shell, '['),
                shell_send(&a->shell, 'A');
        else if (ev.key == KEY_DOWN)
            shell_send(&a->shell, 0x1B), shell_send(&a->shell, '['),
                shell_send(&a->shell, 'B');
        else if (ev.key == KEY_RIGHT)
            shell_send(&a->shell, 0x1B), shell_send(&a->shell, '['),
                shell_send(&a->shell, 'C');
        else if (ev.key == KEY_LEFT)
            shell_send(&a->shell, 0x1B), shell_send(&a->shell, '['),
                shell_send(&a->shell, 'D');
        a->dirty = 1;
        return 0;
    }
    if (ev.key == KEY_TAB) {
        app_goto(a, SCR_HOME);
        return 0;
    }
    if (ev.key == KEY_ESC) {
        app_goto(a, SCR_HOME);
        return 0;
    }
    if (ev.key == KEY_CHAR && ev.ch == '?') {
        overlays_show_help(a);
        a->dirty = 1;
        return 0;
    }
    if (ev.key == KEY_CHAR && ev.ch == '/') {
        if (a->screen == SCR_LOG) {
            strncpy(a->input, a->log.filter, sizeof a->input - 1);
            a->input_len = strlen(a->input);
            a->input_cur = a->input_len;
            a->modal = M_FILTER;
        } else {
            a->input[0] = '\0';
            a->input_len = 0;
            a->input_cur = 0;
            a->pal_sel = 0;
            a->modal = M_PALETTE;
        }
        a->dirty = 1;
        return 0;
    }
    switch (a->screen) {
    case SCR_LOG:
        log_key(a, ev);
        return 0;
    case SCR_REGS:
        regs_key(a, ev);
        return 0;
    case SCR_EXC:
        exc_key(a, ev);
        return 0;
    case SCR_STORAGE:
        storage_key(a, ev);
        return 0;
    case SCR_PCI:
        pci_key(a, ev);
        return 0;
    case SCR_HEX:
        hex_key(a, ev);
        return 0;
    default:
        return 0;
    }
}

/* ── main ─────────────────────────────────────────────────────────── */

static const char *parse_reason(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--reason") == 0 && i + 1 < argc)
            return argv[i + 1];
    }
    if (argc > 1 && strncmp(argv[1], "--", 2) != 0)
        return argv[1];
    return "";
}

int main(int argc, char **argv) {
    struct app a;
    struct pollfd pfd;
    int quit = 0;
    unsigned long last_present = (unsigned long)-2; /* first frame ASAP */
    memset(&a, 0, sizeof a);
    strncpy(a.reason, parse_reason(argc, argv), sizeof a.reason - 1);
    a.shell.master = -1;

    lat_term_init(&a.term, STDOUT_FILENO, STDIN_FILENO);
    if (ui_setup(&a) != 0) {
        fprintf(stderr, "eigen-recovery: need at least 80x24\n");
        return 1;
    }
    if (data_init(&a.data) != 0) {
        fprintf(stderr, "eigen-recovery: out of memory\n");
        ui_teardown(&a);
        return 1;
    }
    a.exc_at_start = a.data.exc_count;
    a.hex.addr = 0x100000;
    /* shell opens on command (menu 7 / palette / digit); peek covers idle */
    health_poll(&a);
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
                /* piped stdin hit EOF: present once more, then leave */
                quit = 1;
            } else {
                interacted = 1;
                if (a.modal != M_NONE) {
                    modal_key(&a, ev);
                } else if (ev.key == KEY_CHAR && ev.ch >= '1' &&
                           ev.ch <= '9' && a.screen != SCR_SHELL) {
                    /* global jump digits (shell and modals own theirs) */
                    int idx = ev.ch - '1';
                    if (idx < MENU_N) {
                        a.menu_sel = idx;
                        menu_activate(&a, idx);
                    }
                } else if (a.screen == SCR_HOME) {
                    if (menu_key(&a, ev))
                        quit = 1;
                } else {
                    if (body_key(&a, ev))
                        quit = 1;
                }
            }
        }
        /* live data */
        if (data_poll_kmsg(&a.data)) {
            a.last_kmsg_tick = a.tick;
            a.dirty = 1;
        }
        if (a.tick % 5 == 0) {
            if (data_poll_slow(&a.data))
                a.dirty = 1;
            health_poll(&a);
        }
        if (a.shell.running || a.screen == SCR_SHELL) {
            if (shell_pump(&a.shell))
                a.dirty = 1;
        }
        /* keys present at once; timer redraws throttle to ~5Hz */
        if (a.dirty && (interacted || a.tick - last_present >= 2)) {
            ui_present(&a);
            a.dirty = 0;
            last_present = a.tick;
        }
    }

    shell_free(&a.shell);
    data_free(&a.data);
    ui_teardown(&a);
    lat_term_leave(&a.term, 0);
    lat_term_raw_off(&a.term);
    return 0;
}
