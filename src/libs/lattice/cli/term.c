/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * term.c - L0 terminal backend + keyboard reader implementation.
 * Userspace (musl/POSIX); the shared lattice core stays freestanding.
 */

#define _DEFAULT_SOURCE /* cfmakeraw */

#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "term.h"

/* ── tiny output helpers (raw write, no stdio interleave on fd 1) ──── */

static void out_raw(struct lat_term *t, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    /* Flush the batch buffer first so ordering stays exact. */
    if (t->outlen > 0)
        lat_term_flush(t);
    while (len > 0) {
        ssize_t n = write(t->out_fd, p, len);
        if (n <= 0)
            return;
        p += n;
        len -= (size_t)n;
    }
}

static void out_buf(struct lat_term *t, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t i;
    for (i = 0; i < len; i++) {
        if (t->outlen >= sizeof t->outbuf)
            lat_term_flush(t);
        t->outbuf[t->outlen++] = p[i];
    }
}

void lat_term_flush(struct lat_term *t) {
    size_t off = 0;
    while (off < t->outlen) {
        ssize_t n = write(t->out_fd, t->outbuf + off, t->outlen - off);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    t->outlen = 0;
}

/* unsigned decimal, returns length. */
static int utoa(unsigned v, char *out) {
    char tmp[12];
    int n = 0;
    int i;
    if (v == 0) {
        out[0] = '0';
        return 1;
    }
    while (v > 0 && n < 11) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

/* ── backend: cell -> ANSI ──────────────────────────────────────────── */

static void term_pen(struct lat_term *t, const struct lat_cell *c) {
    char seq[32];
    size_t n = 0;
    if (c->fg == t->last_fg && c->bg == t->last_bg && c->attr == t->last_attr)
        return;
    seq[n++] = '\033';
    seq[n++] = '[';
    seq[n++] = '0';
    if (c->attr & LAT_ATTR_BOLD) {
        seq[n++] = ';';
        seq[n++] = '1';
    }
    if (c->attr & LAT_ATTR_DIM) {
        seq[n++] = ';';
        seq[n++] = '2';
    }
    if (c->attr & LAT_ATTR_REVERSE) {
        seq[n++] = ';';
        seq[n++] = '7';
    }
    seq[n++] = ';';
    if ((c->fg & 8) == 0) {
        seq[n++] = '3';
        seq[n++] = (char)('0' + (c->fg & 7));
    } else {
        seq[n++] = '9';
        seq[n++] = (char)('0' + (c->fg & 7));
    }
    seq[n++] = ';';
    if ((c->bg & 8) == 0) {
        seq[n++] = '4';
        seq[n++] = (char)('0' + (c->bg & 7));
    } else {
        seq[n++] = '1';
        seq[n++] = '0';
        seq[n++] = (char)('0' + (c->bg & 7));
    }
    seq[n++] = 'm';
    out_buf(t, seq, n);
    t->last_fg = c->fg;
    t->last_bg = c->bg;
    t->last_attr = c->attr;
}

/* CP437 cell -> UTF-8 on the wire. Eigen's console decodes UTF-8 back to
 * its CP437 tiles; a Linux terminal renders the same bytes natively.
 * Unmapped high bytes become '?' (never raw controls). */
static void term_put_glyph(struct lat_term *t, uint8_t ch) {
    static const struct {
        uint8_t cp;
        char u8[3];
    } map[] = {
        { 0x04, { '\xe2', '\x97', '\x86' } }, /* ◆ */
        { 0x07, { '\xe2', '\x80', '\xa2' } }, /* • */
        { 0x07, { '\xe2', '\x80', '\xa2' } }, /* • */
        { 0x0A, { '\xe2', '\x97', '\x8f' } }, /* ● */
        { 0x10, { '\xe2', '\x96', '\xba' } }, /* ► */
        { 0x11, { '\xe2', '\x97', '\x84' } }, /* ◄ */
        { 0x18, { '\xe2', '\x86', '\x91' } }, /* ↑ */
        { 0x19, { '\xe2', '\x86', '\x93' } }, /* ↓ */
        { 0x1A, { '\xe2', '\x86', '\x92' } }, /* → */
        { 0x1B, { '\xe2', '\x86', '\x90' } }, /* ← */
        { 0x1E, { '\xe2', '\x96', '\xb2' } }, /* ▲ */
        { 0x1F, { '\xe2', '\x96', '\xbc' } }, /* ▼ */
        { 0xB0, { '\xe2', '\x96', '\x91' } }, /* ░ */
        { 0xB1, { '\xe2', '\x96', '\x92' } }, /* ▒ */
        { 0xB2, { '\xe2', '\x96', '\x93' } }, /* ▓ */
        { 0xB3, { '\xe2', '\x94', '\x82' } }, /* │ */
        { 0xB4, { '\xe2', '\x94', '\xa4' } }, /* ┤ */
        { 0xB7, { '\xc2', '\xb7', '\0' } },   /* · */
        { 0xBA, { '\xe2', '\x95', '\x91' } }, /* ║ */
        { 0xBB, { '\xe2', '\x95', '\x97' } }, /* ╗ */
        { 0xBC, { '\xe2', '\x95', '\x9d' } }, /* ╝ */
        { 0xB9, { '\xe2', '\x95', '\xa3' } }, /* ╣ */
        { 0xC0, { '\xe2', '\x94', '\x94' } }, /* └ */
        { 0xC1, { '\xe2', '\x94', '\xb4' } }, /* ┴ */
        { 0xC2, { '\xe2', '\x94', '\xac' } }, /* ┬ */
        { 0xC3, { '\xe2', '\x94', '\x9c' } }, /* ├ */
        { 0xC4, { '\xe2', '\x94', '\x80' } }, /* ─ */
        { 0xC5, { '\xe2', '\x94', '\xbc' } }, /* ┼ */
        { 0xC8, { '\xe2', '\x95', '\x9a' } }, /* ╚ */
        { 0xC9, { '\xe2', '\x95', '\x94' } }, /* ╔ */
        { 0xCA, { '\xe2', '\x95', '\xa9' } }, /* ╩ */
        { 0xCB, { '\xe2', '\x95', '\xa6' } }, /* ╦ */
        { 0xCC, { '\xe2', '\x95', '\xa0' } }, /* ╠ */
        { 0xCD, { '\xe2', '\x95', '\x90' } }, /* ═ */
        { 0xCE, { '\xe2', '\x95', '\xac' } }, /* ╬ */
        { 0xD9, { '\xe2', '\x94', '\x98' } }, /* ┘ */
        { 0xDA, { '\xe2', '\x94', '\x8c' } }, /* ┌ */
        { 0xDB, { '\xe2', '\x96', '\x88' } }, /* █ */
        { 0xBF, { '\xe2', '\x94', '\x90' } }, /* ┐ */
        { 0xF1, { '\xc2', '\xb1', '\0' } },   /* ± */
        { 0xF8, { '\xc2', '\xb0', '\0' } },   /* ° */
        { 0xFE, { '\xe2', '\x96', '\xa0' } }, /* ■ */
    };
    size_t i;
    /* Printable ASCII goes out as-is. Everything else (CP437 graphics,
     * arrows, dots -- including C0 controls like 0x0A/0x10 used as
     * glyph cells) resolves through the map: emitting a raw control
     * byte would move/swallow the terminal cursor mid-frame. */
    if (ch >= 0x20 && ch < 0x7F) {
        out_buf(t, (char *)&ch, 1);
        return;
    }
    for (i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (map[i].cp == ch) {
            size_t len = map[i].u8[2] ? 3 : 2;
            out_buf(t, map[i].u8, len);
            return;
        }
    }
    out_buf(t, "?", 1);
}

static void term_present(void *ctx, uint32_t x, uint32_t y,
                         const struct lat_cell *cell) {
    struct lat_term *t = (struct lat_term *)ctx;
    char seq[16];
    size_t n = 0;
    term_pen(t, cell);
    /* Sequential cells need no re-addressing: the cursor is already there. */
    if ((int)x != t->cur_x || (int)y != t->cur_y) {
        seq[n++] = '\033';
        seq[n++] = '[';
        n += (size_t)utoa(y + 1, seq + n);
        seq[n++] = ';';
        n += (size_t)utoa(x + 1, seq + n);
        seq[n++] = 'H';
        out_buf(t, seq, n);
    }
    term_put_glyph(t, cell->ch);
    t->cur_x = (int)x + 1;
    t->cur_y = (int)y;
}

static void term_flush_cb(void *ctx) {
    lat_term_flush((struct lat_term *)ctx);
}

void lat_term_init(struct lat_term *t, int out_fd, int in_fd) {
    struct winsize ws;
    const char *env;
    memset(t, 0, sizeof *t);
    t->out_fd = out_fd;
    t->in_fd = in_fd;
    t->rows = 24;
    t->cols = 80;
    t->last_fg = -1;
    t->last_bg = -1;
    t->last_attr = -1;
    t->cur_x = -1;
    t->cur_y = -1;
    if (ioctl(out_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        t->cols = ws.ws_col;
        t->rows = ws.ws_row;
    } else {
        env = getenv("COLUMNS");
        if (env && atoi(env) > 0)
            t->cols = atoi(env);
        env = getenv("LINES");
        if (env && atoi(env) > 0)
            t->rows = atoi(env);
    }
    if (t->cols < 20)
        t->cols = 20;
    if (t->rows < 10)
        t->rows = 10;
    t->be.ctx = t;
    t->be.present = term_present;
    t->be.flush = term_flush_cb;
    t->be.cursor = 0;
}

void lat_term_enter(struct lat_term *t) {
    static const char alt[] = "\033[?1049h\033[H\033[?25l";
    t->last_fg = -1;
    t->last_bg = -1;
    t->last_attr = -1;
    t->cur_x = -1;
    t->cur_y = -1;
    out_raw(t, alt, sizeof alt - 1);
}

void lat_term_leave(struct lat_term *t, int clear_screen) {
    static const char bye[] = "\033[0m\033[?25h\033[?1049l";
    static const char wipe[] = "\033[H\033[2J";
    lat_term_flush(t);
    out_raw(t, bye, sizeof bye - 1);
    if (clear_screen)
        out_raw(t, wipe, sizeof wipe - 1);
    t->last_fg = -1;
    t->last_bg = -1;
    t->last_attr = -1;
}

/* ── raw mode ───────────────────────────────────────────────────────── */

static struct termios saved_termios;
static int have_saved = 0;

int lat_term_raw_on(struct lat_term *t) {
    struct termios raw;
    t->raw_ok = 0;
    if (tcgetattr(t->in_fd, &saved_termios) != 0)
        return 0; /* pipe/file: carry on in the current discipline */
    have_saved = 1;
    raw = saved_termios;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(t->in_fd, TCSANOW, &raw) != 0)
        return 0;
    t->raw_ok = 1;
    return 1;
}

void lat_term_raw_off(struct lat_term *t) {
    if (have_saved)
        tcsetattr(t->in_fd, TCSANOW, &saved_termios);
    have_saved = 0;
    t->raw_ok = 0;
}

/* ── key reader ─────────────────────────────────────────────────────── */

static int read_byte(struct lat_term *t) {
    unsigned char c;
    ssize_t n;
    for (;;) {
        n = read(t->in_fd, &c, 1);
        if (n == 1)
            return (int)c;
        if (n == 0)
            return -1; /* EOF */
        /* EINTR: retry; anything else on a blocking fd is fatal-ish. */
    }
}

static int byte_ready(struct lat_term *t, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = t->in_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    return poll(&pfd, 1, timeout_ms) > 0;
}

struct lat_keyev lat_term_read_key(struct lat_term *t) {
    struct lat_keyev ev = { KEY_NONE, 0 };
    int c = read_byte(t);
    int c2;
    int c3;
    if (c < 0)
        return ev; /* EOF -> KEY_NONE; dialogs treat as cancel/done */
    if (c == '\r' || c == '\n') {
        ev.key = KEY_ENTER;
        return ev;
    }
    if (c == '\t') {
        ev.key = KEY_TAB;
        return ev;
    }
    if (c == 0x7F || c == 0x08) {
        ev.key = KEY_BACKSPACE;
        return ev;
    }
    if (c == 0x03) {
        ev.key = KEY_CTRL_C;
        return ev;
    }
    if (c != 0x1B) {
        if (c >= 0x20 && c < 0x7F) {
            ev.key = KEY_CHAR;
            ev.ch = (char)c;
        }
        return ev;
    }
    /* ESC: bare key or the start of a sequence? */
    if (!byte_ready(t, 60)) {
        ev.key = KEY_ESC;
        return ev;
    }
    c2 = read_byte(t);
    if (c2 != '[') {
        ev.key = KEY_ESC; /* Alt-combo: cancel is the safe reading */
        return ev;
    }
    c3 = read_byte(t);
    switch (c3) {
    case 'A': ev.key = KEY_UP; break;
    case 'B': ev.key = KEY_DOWN; break;
    case 'C': ev.key = KEY_RIGHT; break;
    case 'D': ev.key = KEY_LEFT; break;
    case 'H': ev.key = KEY_HOME; break;
    case 'F': ev.key = KEY_END; break;
    case '5': /* PgUp = "5~" */
        if (byte_ready(t, 30) && read_byte(t) == '~')
            ev.key = KEY_PGUP;
        break;
    case '6': /* PgDn = "6~" */
        if (byte_ready(t, 30) && read_byte(t) == '~')
            ev.key = KEY_PGDN;
        break;
    default: break;
    }
    return ev;
}
