/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * main.c - lattice CLI entry point. Whiptail-style invocation:
 *
 *   lattice [--title T] [--backtitle B] [--clear] <widget> ...
 *
 * Widgets: --msgbox --infobox --yesno --menu --inputbox --passwordbox
 * --textbox --gauge. H/W (and menu height) of 0 mean auto-size.
 * UI goes to stdout, the result to stderr, exit 0 = ok, 1 = cancel/no,
 * 255 = error -- so shell capture uses the dialog dance:
 *   choice=$(lattice --menu "Pick one" 0 0 0 a A b B 3>&1 1>&2 2>&3)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "term.h"
#include "dialogs.h"

static void usage(void) {
    static const char *msg =
        "usage: lattice [--title T] [--backtitle B] [--clear] <widget> ...\n"
        "  --msgbox text h w\n"
        "  --infobox text h w\n"
        "  --yesno text h w\n"
        "  --menu text h w mh tag item [tag item]...\n"
        "  --inputbox text h w [init]\n"
        "  --passwordbox text h w [init]\n"
        "  --textbox file h w\n"
        "  --gauge text h w [pct]     (reads NNN/XXX lines on stdin)\n"
        "h/w/mh of 0 auto-size. result on stderr, exit 0 ok / 1 cancel.\n";
    (void)write(STDERR_FILENO, msg, strlen(msg));
}

static int num_arg(const char *s, int *ok) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v > 10000) {
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return (int)v;
}

int main(int argc, char **argv) {
    const char *title = "";
    const char *backtitle = "";
    int clear_screen = 0;
    int i = 1;
    struct lat_term term;
    int rc = DLG_ERROR;
    /* skip argv[0]; collect globals */
    while (i < argc && argv[i][0] == '-' && argv[i][1] == '-') {
        if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            title = argv[++i];
            i++;
        } else if (strcmp(argv[i], "--backtitle") == 0 && i + 1 < argc) {
            backtitle = argv[++i];
            i++;
        } else if (strcmp(argv[i], "--clear") == 0) {
            clear_screen = 1;
            i++;
        } else {
            break;
        }
    }
    if (i >= argc) {
        usage();
        return DLG_ERROR;
    }
    lat_term_init(&term, STDOUT_FILENO, STDIN_FILENO);
    lat_term_raw_on(&term);
    lat_term_enter(&term);

    {
        const char *w = argv[i];
        int ok;
        int h;
        int ww;
        if (strcmp(w, "--msgbox") == 0 || strcmp(w, "--infobox") == 0) {
            int infobox = strcmp(w, "--infobox") == 0;
            if (i + 3 >= argc) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            h = num_arg(argv[i + 2], &ok);
            if (!ok) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            ww = num_arg(argv[i + 3], &ok);
            if (!ok) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            rc = dlg_msgbox(&term, title, backtitle, argv[i + 1], h, ww,
                            infobox);
        } else if (strcmp(w, "--yesno") == 0) {
            if (i + 3 >= argc) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            h = num_arg(argv[i + 2], &ok);
            if (!ok) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            ww = num_arg(argv[i + 3], &ok);
            if (!ok) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            rc = dlg_yesno(&term, title, backtitle, argv[i + 1], h, ww);
        } else if (strcmp(w, "--menu") == 0) {
            int mh;
            int n;
            int k;
            char **tags;
            char **items;
            if (i + 4 >= argc) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            h = num_arg(argv[i + 2], &ok);
            if (!ok) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            ww = num_arg(argv[i + 3], &ok);
            if (!ok) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            mh = num_arg(argv[i + 4], &ok);
            if (!ok) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            n = (argc - (i + 5)) / 2;
            if (n <= 0 || (argc - (i + 5)) % 2 != 0) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            tags = malloc(sizeof(char *) * (size_t)n);
            items = malloc(sizeof(char *) * (size_t)n);
            if (!tags || !items) {
                free(tags);
                free(items);
                rc = DLG_ERROR;
                goto out;
            }
            for (k = 0; k < n; k++) {
                tags[k] = argv[i + 5 + k * 2];
                items[k] = argv[i + 5 + k * 2 + 1];
            }
            rc = dlg_menu(&term, title, backtitle, argv[i + 1], h, ww, mh,
                          tags, items, n, STDERR_FILENO);
            free(tags);
            free(items);
        } else if (strcmp(w, "--inputbox") == 0 ||
                   strcmp(w, "--passwordbox") == 0) {
            int password = strcmp(w, "--passwordbox") == 0;
            const char *init = (i + 4 < argc) ? argv[i + 4] : "";
            if (i + 3 >= argc) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            h = num_arg(argv[i + 2], &ok);
            if (!ok) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            ww = num_arg(argv[i + 3], &ok);
            if (!ok) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            rc = dlg_inputbox(&term, title, backtitle, argv[i + 1], h, ww,
                              init, password, STDERR_FILENO);
        } else if (strcmp(w, "--textbox") == 0) {
            if (i + 3 >= argc) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            h = num_arg(argv[i + 2], &ok);
            if (!ok) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            ww = num_arg(argv[i + 3], &ok);
            if (!ok) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            rc = dlg_textbox(&term, title, backtitle, argv[i + 1], h, ww);
        } else if (strcmp(w, "--gauge") == 0) {
            int pct = 0;
            if (i + 3 >= argc) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            h = num_arg(argv[i + 2], &ok);
            if (!ok) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            ww = num_arg(argv[i + 3], &ok);
            if (!ok) {
                usage();
                rc = DLG_ERROR;
                goto out;
            }
            if (i + 4 < argc) {
                pct = num_arg(argv[i + 4], &ok);
                if (!ok) {
                    usage();
                    rc = DLG_ERROR;
                    goto out;
                }
            }
            rc = dlg_gauge(&term, title, backtitle, argv[i + 1], h, ww, pct,
                           STDIN_FILENO);
        } else {
            usage();
            rc = DLG_ERROR;
        }
    }
out:
    lat_term_leave(&term, clear_screen);
    lat_term_raw_off(&term);
    return rc;
}
