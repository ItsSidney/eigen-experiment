/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * overlays/overlays.c - palette, modals, line editor.
 */

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "overlays/overlays.h"
#include "ui/ui.h"
#include "actions/actions.h"

void show_message(struct app *a, const char *text) {
    strncpy(a->message, text, sizeof a->message - 1);
    a->message[sizeof a->message - 1] = '\0';
    a->modal = M_MESSAGE;
    a->dirty = 1;
}

void show_confirm(struct app *a, const char *title, const char *text,
                  int action) {
    strncpy(a->confirm.title, title, sizeof a->confirm.title - 1);
    strncpy(a->confirm.text, text, sizeof a->confirm.text - 1);
    a->confirm.action = action;
    a->confirm.focus = 0;
    a->modal = M_CONFIRM;
    a->dirty = 1;
}

/* ── palette actions ──────────────────────────────────────────────── */

struct pal_action {
    const char *name;
    const char *help;
};

static const struct pal_action pal_actions[] = {
    { "home", "dashboard" },
    { "log", "kernel log stream" },
    { "registers", "register grid" },
    { "exceptions", "fault history" },
    { "storage", "mount inspector" },
    { "pci", "device tree" },
    { "hex", "memory viewer" },
    { "shell", "background pty shell" },
    { "export report", "write /tmp report" },
    { "reboot", "confirm + reboot" },
    { "shell respawn", "restart the pty shell" },
    { "clear log filter", "show all levels again" },
};
#define PAL_N ((int)(sizeof pal_actions / sizeof pal_actions[0]))
#define PAL_MAX 10

static int pal_match(const char *name, const char *frag) {
    size_t fl;
    size_t i;
    if (!frag[0])
        return 1;
    fl = strlen(frag);
    for (i = 0; name[i]; i++) {
        size_t k;
        for (k = 0; k < fl && name[i + k]; k++) {
            char a = name[i + k];
            char b = frag[k];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (b >= 'A' && b <= 'Z')
                b += 32;
            if (a != b)
                break;
        }
        if (k == fl)
            return 1;
    }
    return 0;
}

/* Fill matches[out_n] (<= PAL_MAX). "peek <addr>" becomes a hex jump. */
static void pal_compute(struct app *a, char matches[][48], int *out_n) {
    int n = 0;
    int i;
    if (strncasecmp(a->input, "peek ", 5) == 0 && a->input[5]) {
        snprintf(matches[n++], 48, "peek %s", a->input + 5);
        *out_n = n;
        return;
    }
    for (i = 0; i < PAL_N && n < PAL_MAX; i++) {
        if (pal_match(pal_actions[i].name, a->input)) {
            snprintf(matches[n], 48, "%s", pal_actions[i].name);
            n++;
        }
    }
    *out_n = n;
}

static void pal_run(struct app *a, const char *match) {
    if (strncmp(match, "peek ", 5) == 0) {
        unsigned long v = strtoul(match + 5, NULL, 0);
        a->hex.addr = v;
        app_goto(a, SCR_HEX);
        snprintf(a->crumb_extra, sizeof a->crumb_extra, "@0x%lX", v);
    } else if (strcmp(match, "home") == 0) {
        app_goto(a, SCR_HOME);
    } else if (strcmp(match, "log") == 0) {
        app_goto(a, SCR_LOG);
    } else if (strcmp(match, "registers") == 0) {
        app_goto(a, SCR_REGS);
    } else if (strcmp(match, "exceptions") == 0) {
        app_goto(a, SCR_EXC);
    } else if (strcmp(match, "storage") == 0) {
        app_goto(a, SCR_STORAGE);
    } else if (strcmp(match, "pci") == 0) {
        app_goto(a, SCR_PCI);
    } else if (strcmp(match, "hex") == 0) {
        app_goto(a, SCR_HEX);
    } else if (strcmp(match, "shell") == 0) {
        app_goto(a, SCR_SHELL);
    } else if (strcmp(match, "export report") == 0) {
        do_export(a);
        return;
    } else if (strcmp(match, "reboot") == 0) {
        show_confirm(a, "CONFIRM REBOOT",
                     "Reboot now? Unsaved state is lost.", ACT_REBOOT);
        return;
    } else if (strcmp(match, "shell respawn") == 0) {
        shell_free(&a->shell);
        memset(&a->shell, 0, sizeof a->shell);
        a->shell.master = -1;
        if (shell_spawn(&a->shell) != 0)
            show_message(a, "shell respawn failed (no pty?)");
        app_goto(a, SCR_SHELL);
        return;
    } else if (strcmp(match, "clear log filter") == 0) {
        a->log.filter[0] = '\0';
        app_goto(a, SCR_LOG);
        return;
    } else {
        return;
    }
    a->modal = M_NONE;
    a->dirty = 1;
}

/* ── modal drawing ────────────────────────────────────────────────── */

static void draw_confirm(struct app *a) {
    int ix, iy, iw, ih;
    char *line;
    char *save = NULL;
    int y;
    ui_modal_frame(a, a->confirm.title, 52, 12, &ix, &iy, &iw, &ih);
    y = iy + 1;
    line = strtok_r(a->confirm.text, "\n", &save);
    while (line && y < iy + ih - 3) {
        ui_text(a, a->lay_modal, ix, y++, line, 0, 7, 0, iw);
        line = strtok_r(NULL, "\n", &save);
    }
    /* NOTE: strtok mutates confirm.text; restore not needed (one-shot). */
    ui_button(a, a->lay_modal, ix + iw / 2 - 10, iy + ih - 2, "Confirm",
              a->confirm.focus == 0);
    ui_button(a, a->lay_modal, ix + iw / 2 + 10, iy + ih - 2, "Cancel",
              a->confirm.focus == 1);
}

static void draw_message(struct app *a) {
    int ix, iy, iw, ih;
    char copy[512];
    char *line;
    char *save = NULL;
    int y;
    ui_modal_frame(a, "NOTE", 56, 14, &ix, &iy, &iw, &ih);
    strncpy(copy, a->message, sizeof copy - 1);
    copy[sizeof copy - 1] = '\0';
    y = iy + 1;
    line = strtok_r(copy, "\n", &save);
    while (line && y < iy + ih - 2) {
        ui_text(a, a->lay_modal, ix, y++, line, 0, 7, 0, iw);
        line = strtok_r(NULL, "\n", &save);
    }
    ui_text(a, a->lay_modal, ix, iy + ih - 1, "[Enter/Esc] close", 0, 7, 0,
            iw);
}

static void draw_input_modal(struct app *a, const char *title,
                             const char *prompt) {
    int ix, iy, iw, ih;
    char line[160];
    ui_modal_frame(a, title, 56, 7, &ix, &iy, &iw, &ih);
    ui_text(a, a->lay_modal, ix, iy + 1, prompt, 0, 7, 0, iw);
    snprintf(line, sizeof line, "> %s", a->input);
    ui_text(a, a->lay_modal, ix, iy + 2, line, 0, 7, 0, iw);
    /* cursor */
    ui_put(a, a->lay_modal, ix + 2 + (int)a->input_cur, iy + 2, '_', 7, 1,
           LAT_ATTR_BOLD);
}

static void draw_palette(struct app *a) {
    int ix, iy, iw, ih;
    char matches[PAL_MAX][48];
    int n = 0;
    int i;
    int y;
    pal_compute(a, matches, &n);
    if (a->pal_sel >= n)
        a->pal_sel = n > 0 ? n - 1 : 0;
    ui_modal_frame(a, "COMMAND", 52, 6 + (n > 8 ? 8 : n + 1), &ix, &iy, &iw,
                   &ih);
    {
        char line[160];
        snprintf(line, sizeof line, "> %s", a->input);
        ui_text(a, a->lay_modal, ix, iy + 1, line, 0, 7, 0, iw);
    }
    y = iy + 3;
    for (i = 0; i < n && i < 8 && y < iy + ih; i++) {
        if (i == a->pal_sel)
            ui_text(a, a->lay_modal, ix, y, matches[i], 7, 1,
                    LAT_ATTR_BOLD, iw);
        else
            ui_text(a, a->lay_modal, ix + 2, y, matches[i], 0, 7, 0,
                    iw - 2);
        y++;
    }
    if (n == 0)
        ui_text(a, a->lay_modal, ix, y, "(no match)", TH_FG, TH_BG, LAT_ATTR_DIM, iw);
}

void overlays_show_help(struct app *a) {
    /* help rides the message modal */
    show_message(a, "GLOBAL\n"
                    "TAB focus menu/body   1-9 jump\n"
                    "/ palette (filter in LOG)   ? help\n"
                    "ESC back / quit\n\n"
                    "LOG: Space pause  END live\n"
                    "REGS: P pin  F freeze\n"
                    "HEX: PgUp/PgDn  G goto  A annotate\n"
                    "SHELL: type away; TAB/ESC stay here");
    a->modal = M_MESSAGE;
}

void draw_modal(struct app *a) {
    switch (a->modal) {
    case M_CONFIRM:
        draw_confirm(a);
        break;
    case M_MESSAGE:
        draw_message(a);
        break;
    case M_PALETTE:
        draw_palette(a);
        break;
    case M_FILTER:
        draw_input_modal(a, "FILTER", "substring (empty = clear):");
        break;
    case M_GOTO:
        draw_input_modal(a, "GOTO", "address (hex, e.g. 0x100000):");
        break;
    default:
        break;
    }
}

/* ── line editor (palette / filter / goto) ────────────────────────── */

static int input_key(struct app *a, struct lat_keyev ev) {
    if (ev.key == KEY_CHAR) {
        if (a->input_len + 1 < sizeof a->input) {
            memmove(a->input + a->input_cur + 1, a->input + a->input_cur,
                    a->input_len - a->input_cur + 1);
            a->input[a->input_cur] = ev.ch;
            a->input_cur++;
            a->input_len++;
        }
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_BACKSPACE) {
        if (a->input_cur > 0) {
            memmove(a->input + a->input_cur - 1, a->input + a->input_cur,
                    a->input_len - a->input_cur + 1);
            a->input_cur--;
            a->input_len--;
        }
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_LEFT) {
        if (a->input_cur > 0)
            a->input_cur--;
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_RIGHT) {
        if (a->input_cur < a->input_len)
            a->input_cur++;
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_HOME) {
        a->input_cur = 0;
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_END) {
        a->input_cur = a->input_len;
        a->dirty = 1;
        return 1;
    }
    return 0;
}

static void confirm_run(struct app *a) {
    int act = a->confirm.action;
    a->modal = M_NONE;
    if (act == ACT_REBOOT) {
        do_reboot(a);
    } else if (act == ACT_EXPORT) {
        do_export(a);
    } else if (act == ACT_FORCE_MOUNT) {
        char src[64] = "";
        char mp[64] = "";
        sscanf(a->confirm_param, "%63s %63s", src, mp);
        if (mp[0])
            do_force_mount(a, src, mp);
    }
    a->dirty = 1;
}

/* modal key routing; returns 1 when consumed */
int modal_key(struct app *a, struct lat_keyev ev) {
    switch (a->modal) {
    case M_CONFIRM:
        if (ev.key == KEY_LEFT || ev.key == KEY_RIGHT || ev.key == KEY_TAB) {
            a->confirm.focus = 1 - a->confirm.focus;
            a->dirty = 1;
            return 1;
        }
        if (ev.key == KEY_ENTER) {
            if (a->confirm.focus == 0)
                confirm_run(a);
            else {
                a->modal = M_NONE;
                a->dirty = 1;
            }
            return 1;
        }
        if (ev.key == KEY_ESC) {
            a->modal = M_NONE;
            a->dirty = 1;
            return 1;
        }
        return 1; /* swallow everything else */
    case M_MESSAGE:
        if (ev.key == KEY_ENTER || ev.key == KEY_ESC || ev.key == KEY_NONE) {
            a->modal = M_NONE;
            a->dirty = 1;
            return 1;
        }
        return 1;
    case M_PALETTE:
        if (ev.key == KEY_ESC) {
            a->modal = M_NONE;
            a->dirty = 1;
            return 1;
        }
        if (ev.key == KEY_UP) {
            if (a->pal_sel > 0)
                a->pal_sel--;
            a->dirty = 1;
            return 1;
        }
        if (ev.key == KEY_DOWN) {
            a->pal_sel++;
            a->dirty = 1;
            return 1;
        }
        if (ev.key == KEY_ENTER) {
            char matches[PAL_MAX][48];
            int n = 0;
            pal_compute(a, matches, &n);
            if (n > 0) {
                if (a->pal_sel >= n)
                    a->pal_sel = n - 1;
                a->modal = M_NONE;
                pal_run(a, matches[a->pal_sel]);
            }
            a->dirty = 1;
            return 1;
        }
        return input_key(a, ev);
    case M_FILTER:
        if (ev.key == KEY_ESC) {
            a->modal = M_NONE; /* keeps the existing filter */
            a->dirty = 1;
            return 1;
        }
        if (ev.key == KEY_ENTER) {
            strncpy(a->log.filter, a->input, sizeof a->log.filter - 1);
            a->modal = M_NONE;
            a->dirty = 1;
            return 1;
        }
        return input_key(a, ev);
    case M_GOTO:
        if (ev.key == KEY_ESC) {
            a->modal = M_NONE;
            a->dirty = 1;
            return 1;
        }
        if (ev.key == KEY_ENTER) {
            unsigned long v = strtoul(a->input, NULL, 0);
            a->hex.addr = v;
            snprintf(a->crumb_extra, sizeof a->crumb_extra, "@0x%lX", v);
            a->modal = M_NONE;
            a->dirty = 1;
            return 1;
        }
        return input_key(a, ev);
    default:
        return 0;
    }
}
