/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * term.h - L0 terminal backend + keyboard reader for the lattice CLI.
 *
 * Backend: an L0 lat_backend over stdout. Changed cells go out as
 * cursor-addressed SGR text; the pen is tracked so repeat colours cost
 * nothing. Size comes from TIOCGWINSZ, then $COLUMNS/$LINES, then 80x24.
 *
 * Input: stdin in raw mode (when it is a tty; pipes still work, just
 * without raw semantics). read_key() blocks for one key and folds the
 * common sequences: arrows, Home/End, Enter, Tab, Backspace, ESC.
 * A lone ESC (nothing follows within ~60ms, via poll) means cancel.
 * Ctrl-C arrives as a byte (ISIG off in raw mode) and maps to KEY_CTRL_C.
 *
 * Convention (whiptail-compatible): the UI goes to stdout, the dialog
 * result goes to stderr, exit 0 = ok, 1 = cancel/no, 255 = error. Shell
 * capture looks like:  out=$(lattice --menu ... 3>&1 1>&2 2>&3)
 */

#ifndef LATTICE_CLI_TERM_H
#define LATTICE_CLI_TERM_H

#include <stdint.h>

#include "lattice.h"

enum lat_key {
    KEY_NONE = 0,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_ENTER,
    KEY_ESC,
    KEY_TAB,
    KEY_BACKSPACE,
    KEY_CTRL_C,
    KEY_CHAR, /* printable byte in key_ch */
};

struct lat_keyev {
    enum lat_key key;
    char ch;
};

struct lat_term {
    int out_fd;   /* UI fd (stdout) */
    int in_fd;    /* input fd (stdin) */
    int rows;
    int cols;
    int raw_ok;   /* termios raw mode actually engaged */
    /* pen tracking for SGR elision */
    int last_fg;
    int last_bg;
    int last_attr;
    /* backend cursor: skip addressing when the next cell continues it */
    int cur_x;
    int cur_y;
    /* output batching */
    char outbuf[8192];
    size_t outlen;
    struct lat_backend be;
};

/* Detect size, install the backend struct. No terminal modes touched. */
void lat_term_init(struct lat_term *t, int out_fd, int in_fd);

/* Enter alt-screen, hide cursor, clear. */
void lat_term_enter(struct lat_term *t);
/* Show cursor, leave alt-screen; --clear blanks the restored screen. */
void lat_term_leave(struct lat_term *t, int clear_screen);

/* Engage raw mode if stdin is a tty. Returns 1 when raw, 0 otherwise. */
int lat_term_raw_on(struct lat_term *t);
/* Restore the saved line discipline (no-op when raw was never engaged). */
void lat_term_raw_off(struct lat_term *t);

/* Block for one key event. */
struct lat_keyev lat_term_read_key(struct lat_term *t);

/* Flush pending output bytes. */
void lat_term_flush(struct lat_term *t);

#endif /* LATTICE_CLI_TERM_H */
