/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * vterm_glue.c - binds third_party/libvterm (MIT, vendored) to the Eigen
 * framebuffer console.
 *
 * Why a library: console.c's hand-written escape interpreter covers what
 * simple programs emit, but full-screen apps need everything a real terminal
 * does - the 256/truecolour cube, pen attributes, alt-screen, scroll
 * regions, UTF-8 widths, damage tracking. libvterm implements all of that
 * and hands us a damage rect, so the only thing left to write is the blit.
 *
 * Direction of control:
 *   userspace bytes -> vterm_input_write() -> libvterm parses and updates
 *   its screen model -> our screen callbacks fire (damage / movecursor /
 *   settermprop) -> console_put_cell() blits the affected cells.
 *
 * Everything libvterm needs from libc comes from src/kernel/compat (shim
 * headers forwarding to kheap) plus the allocator below, so no libc is
 * linked into the kernel.
 */

#include "vterm_glue.h"
#include "console.h"

#include "vterm.h"          /* third_party/libvterm/include */
#include "vterm_internal.h"  /* vendored state struct: mode.cursor */
#include "fs/vfs/vfs.h"
#include "mm/kheap.h"
#include "include/serial.h"

/* ── UTF-8 decoder for the keyboard→host path ───────────────────────────
 * libvterm's input API wants Unicode codepoints, but the i8042 driver hands
 * us raw bytes (0x1B 0x5B 0x42 for Down, single CP437 bytes for text).
 * libvterm only translates keyboard input when it is in "application" mode
 * and we drive it through vterm_keyboard_*; a plain terminal like ours must
 * forward raw bytes straight into the input stream. The host then decides:
 * plain bytes go to the tty reader; ESC-led sequences are looked up in the
 * vt100 key table below (matching what the PS/2 driver emits). Whether the
 * app is in normal or application-cursor mode comes from
 * vterm_state_get_cursor_mode(). */
static const struct { uint32_t ucs; const char *normal; const char *app; } vt_keys[] = {
    { VTERM_KEY_UP,        "\x1b[A",  "\x1bOA"  },
    { VTERM_KEY_DOWN,      "\x1b[B",  "\x1bOB"  },
    { VTERM_KEY_RIGHT,     "\x1b[C",  "\x1bOC"  },
    { VTERM_KEY_LEFT,      "\x1b[D",  "\x1bOD"  },
    { VTERM_KEY_HOME,      "\x1b[H",  "\x1bOH"  },
    { VTERM_KEY_END,       "\x1b[F",  "\x1bOF"  },
    { VTERM_KEY_INS,       "\x1b[2~", "\x1b[2~" },
    { VTERM_KEY_DEL,       "\x1b[3~", "\x1b[3~" },
    { VTERM_KEY_PAGEUP,    "\x1b[5~", "\x1b[5~" },
    { VTERM_KEY_PAGEDOWN,  "\x1b[6~", "\x1b[6~" },
    { VTERM_KEY_FUNCTION(1),  "\x1bOP",  "\x1bOP"  },
    { VTERM_KEY_FUNCTION(2),  "\x1bOQ",  "\x1bOQ"  },
    { VTERM_KEY_FUNCTION(3),  "\x1bOR",  "\x1bOR"  },
    { VTERM_KEY_FUNCTION(4),  "\x1bOS",  "\x1bOS"  },
    { VTERM_KEY_FUNCTION(5),  "\x1b[15~", "\x1b[15~" },
    { VTERM_KEY_FUNCTION(6),  "\x1b[17~", "\x1b[17~" },
    { VTERM_KEY_FUNCTION(7),  "\x1b[18~", "\x1b[18~" },
    { VTERM_KEY_FUNCTION(8),  "\x1b[19~", "\x1b[19~" },
    { VTERM_KEY_FUNCTION(9),  "\x1b[20~", "\x1b[20~" },
    { VTERM_KEY_FUNCTION(10), "\x1b[21~", "\x1b[21~" },
    { VTERM_KEY_FUNCTION(11), "\x1b[23~", "\x1b[23~" },
    { VTERM_KEY_FUNCTION(12), "\x1b[24~", "\x1b[24~" },
    { VTERM_KEY_ENTER,     "\r",      "\r"      },
    { VTERM_KEY_TAB,       "\t",      "\t"      },
    { VTERM_KEY_BACKSPACE, "\x7f",    "\x7f"    },
    { VTERM_KEY_ESCAPE,    "\x1b",    "\x1b"    },
};

static void vterm_host_feed(char c);
static void vterm_host_flush_pending(void);
static void vt_out_cb(const char *s, size_t len, void *user);

static VTerm       *g_vt    = NULL;
static VTermScreen *g_scr   = NULL;
static VTermState  *g_state = NULL;
static int          g_ready = 0;

/* ── allocator: libvterm requires zeroed memory from its malloc ────────── */
static void *vt_alloc(size_t size, void *data) {
    (void)data;
    return kzalloc(size ? size : 1);
}

static void vt_dealloc(void *ptr, void *data) {
    (void)data;
    kfree(ptr);
}

static const VTermAllocatorFunctions vt_allocator = {
    vt_alloc,
    vt_dealloc,
};

/* ── Unicode -> CP437 glyph mapping ──────────────────────────────────────
 * libvterm decodes UTF-8 into UCS-32 codepoints; our font is the IBM VGA
 * CP437 set (plus the tiles patched into console.c). Box-drawing and block
 * characters arrive as their Unicode codepoints, so translate the ones a
 * terminal actually uses and fall back to '?' for anything else. */
static uint8_t ucs_to_cp437(uint32_t ucs) {
    if (ucs < 0x80) return (uint8_t)ucs;
    /* There is deliberately no Latin-1 shortcut here. CP437's high half is
     * NOT ISO-8859-1: U+00B0 is 0xF8 in CP437, while byte 0xB0 is a shade
     * block, so forwarding the codepoint as its own byte paints random box
     * and shade tiles wherever an app meant to print a symbol. Everything is
     * resolved through the table below. */
    switch (ucs) {
        /* whitespace and spaces */
        case 0x00A0: /* NBSP */
        case 0x2000: case 0x2001: case 0x2002: case 0x2003:
        case 0x2004: case 0x2005: case 0x2006: case 0x2007:
        case 0x2008: case 0x2009: case 0x200A: case 0x200B:
        case 0x202F: case 0x205F: case 0x3000:
            return ' ';

        /* accents, punctuation, symbols */
        case 0x00A1: return 0xAD; /* ¡ */
        case 0x00A2: return 0x9B; /* ¢ */
        case 0x00A3: return 0x9C; /* £ */
        case 0x00A5: return 0x9D; /* ¥ */
        case 0x00AA: return 0xA6;
        case 0x00AB: return 0xAE;
        case 0x00A7: return 0x15; /* § */
        case 0x00AC: return 0xAA; /* ¬ */
        case 0x00B0: return 0xF8; /* ° */
        case 0x00B1: return 0xF1; /* ± */
        case 0x00B2: return 0xFD; /* ² */
        case 0x00B3: return 0xFC; /* ³ */
        case 0x00B5: return 0xE6; /* µ */
        case 0x00B6: return 0x14; /* ¶ */
        case 0x00B7: return 0xFA; /* · */
        case 0x00BA: return 0xA7;
        case 0x00BB: return 0xAF;
        case 0x00BC: return 0xAC; /* ¼ */
        case 0x00BD: return 0xAB; /* ½ */
        case 0x00BF: return 0xA8; /* ¿ */
        case 0x00F7: return 0xF6; /* ÷ */
        case 0x207F: return 0xFC; /* ⁿ */
        case 0x2219: return 0xF9; /* ∙ */
        case 0x2248: return 0xF7; /* ≈ */
        case 0x2320: return 0xF4; /* ⌠ */
        case 0x2321: return 0xF5; /* ⌡ */

        /* dashes, quotes, bullets */
        case 0x2010: case 0x2011: case 0x2012: case 0x2013:
        case 0x2014: case 0x2015: return '-';
        case 0x2018: case 0x2019: case 0x201A: case 0x201B: return '\'';
        case 0x201C: case 0x201D: case 0x201E: case 0x201F: return '\"';
        case 0x2026: return 0xFA; /* … -> · */
        case 0x2039: return '<';
        case 0x203A: return '>';

        /* arrows */
        case 0x2190: return 0x1B; /* ← */
        case 0x2191: return 0x18; /* ↑ */
        case 0x2192: return 0x1A; /* → */
        case 0x2193: return 0x19; /* ↓ */
        case 0x2194: return 0x1D; /* ↔ */
        case 0x2195: return 0x12; /* ↕ */

        /* single and heavy horizontal lines */
        case 0x2500: case 0x2501: case 0x2504: case 0x2505:
        case 0x2508: case 0x2509: case 0x254C: case 0x254E:
            return 0xC4; /* ─ */

        /* single and heavy vertical lines */
        case 0x2502: case 0x2503: case 0x2506: case 0x2507:
        case 0x250A: case 0x250B: case 0x254D: case 0x254F:
            return 0xB3; /* │ */

        /* corners: top-left (down and right) */
        case 0x250C: case 0x250D: case 0x250E: case 0x250F:
        case 0x256D: /* ╭ arc */
            return 0xDA; /* ┌ */

        /* corners: top-right (down and left) */
        case 0x2510: case 0x2511: case 0x2512: case 0x2513:
        case 0x256E: /* ╮ arc */
            return 0xBF; /* ┐ */

        /* corners: bottom-left (up and right) */
        case 0x2514: case 0x2515: case 0x2516: case 0x2517:
        case 0x2570: /* ╰ arc */
            return 0xC0; /* └ */

        /* corners: bottom-right (up and left) */
        case 0x2518: case 0x2519: case 0x251A: case 0x251B:
        case 0x256F: /* ╯ arc */
            return 0xD9; /* ┘ */

        /* tees: left (vertical and right) */
        case 0x251C: case 0x251D: case 0x251E: case 0x251F:
        case 0x2520: case 0x2521: case 0x2522: case 0x2523:
            return 0xC3; /* ├ */

        /* tees: right (vertical and left) */
        case 0x2524: case 0x2525: case 0x2526: case 0x2527:
        case 0x2528: case 0x2529: case 0x252A: case 0x252B:
            return 0xB4; /* ┤ */

        /* tees: top (horizontal and down) */
        case 0x252C: case 0x252D: case 0x252E: case 0x252F:
        case 0x2530: case 0x2531: case 0x2532: case 0x2533:
            return 0xC2; /* ┬ */

        /* tees: bottom (horizontal and up) */
        case 0x2534: case 0x2535: case 0x2536: case 0x2537:
        case 0x2538: case 0x2539: case 0x253A: case 0x253B:
            return 0xC1; /* ┴ */

        /* crosses */
        case 0x253C: case 0x253D: case 0x253E: case 0x253F:
        case 0x2540: case 0x2541: case 0x2542: case 0x2543:
        case 0x2544: case 0x2545: case 0x2546: case 0x2547:
        case 0x2548: case 0x2549: case 0x254A: case 0x254B:
            return 0xC5; /* ┼ */

        /* double-line and mixed box drawing */
        case 0x2550: return 0xCD; /* ═ */
        case 0x2551: return 0xBA; /* ║ */
        case 0x2552: return 0xD5; /* ╒ */
        case 0x2553: return 0xD6; /* ╓ */
        case 0x2554: return 0xC9; /* ╔ */
        case 0x2555: return 0xB8; /* ╕ */
        case 0x2556: return 0xB7; /* ╖ */
        case 0x2557: return 0xBB; /* ╗ */
        case 0x2558: return 0xD4; /* ╘ */
        case 0x2559: return 0xD3; /* ╙ */
        case 0x255A: return 0xC8; /* ╚ */
        case 0x255B: return 0xBE; /* ╛ */
        case 0x255C: return 0xBD; /* ╜ */
        case 0x255D: return 0xBC; /* ╝ */
        case 0x255E: return 0xC6; /* ╞ */
        case 0x255F: return 0xC7; /* ╟ */
        case 0x2560: return 0xCC; /* ╠ */
        case 0x2561: return 0xB5; /* ╡ */
        case 0x2562: return 0xB6; /* ╢ */
        case 0x2563: return 0xB9; /* ╣ */
        case 0x2564: return 0xD1; /* ╤ */
        case 0x2565: return 0xD2; /* ╥ */
        case 0x2566: return 0xCB; /* ╦ */
        case 0x2567: return 0xCF; /* ╧ */
        case 0x2568: return 0xD0; /* ╨ */
        case 0x2569: return 0xCA; /* ╩ */
        case 0x256A: return 0xD8; /* ╪ */
        case 0x256B: return 0xD7; /* ╫ */
        case 0x256C: return 0xCE; /* ╬ */

        /* diagonals */
        case 0x2571: return '/';
        case 0x2572: return '\\';
        case 0x2573: return 'X';

        /* half lines */
        case 0x2574: case 0x2576: case 0x2578: case 0x257A:
        case 0x257C: case 0x257E: return 0xC4;
        case 0x2575: case 0x2577: case 0x2579: case 0x257B:
        case 0x257D: case 0x257F: return 0xB3;

        /* blocks and shades */
        case 0x2580: case 0x2581: case 0x2582: case 0x2583: return 0xDF; /* ▀ */
        case 0x2584: case 0x2585: case 0x2586: case 0x2587: return 0xDC; /* ▄ */
        case 0x2588: case 0x2589: case 0x258A: case 0x258B: return 0xDB; /* █ */
        case 0x258C: case 0x258D: case 0x258E: case 0x258F: return 0xDD; /* ▌ */
        case 0x2590: case 0x2594: case 0x2595: return 0xDE; /* ▐ */
        case 0x2591: return 0xB0; /* ░ */
        case 0x2592: return 0xB1; /* ▒ */
        case 0x2593: return 0xB2; /* ▓ */

        /* geometric shapes, maths */
        case 0x2022: return 0x07; /* • */
        case 0x221A: return 0xFB; /* √ */
        case 0x221E: return 0xEC; /* ∞ */
        case 0x2261: return 0xF0; /* ≡ */
        case 0x2264: return 0xF3; /* ≤ */
        case 0x2265: return 0xF2; /* ≥ */
        case 0x2310: return 0xA9;
        case 0x25A0: case 0x25AA: case 0x25AB: return 0xFE; /* ■ */
        case 0x25B2: return 0x1E; /* ▲ */
        case 0x25BA: return 0x10; /* ► */
        case 0x25BC: return 0x1F; /* ▼ */
        case 0x25C4: return 0x11; /* ◄ */
        case 0x25C6: return 0x04; /* ◆ */
        case 0x25CB: return 0x09; /* ○ */
        case 0x25CF: return 0x0A; /* ● */
        case 0x03A3: return 0xE4; /* Σ */
        case 0x03C0: return 0xE3; /* π */
        case 0x03C3: return 0xE5; /* σ */

        /* horizontal scan lines (DEC o/p/r/s) and maths/other DEC graphics */
        case 0x23BA: return 0xC4; /* ⎺ */
        case 0x23BB: return 0xC4; /* ⎻ */
        case 0x23BC: return 0xC4; /* ⎼ */
        case 0x23BD: return 0xC4; /* ⎽ */
        case 0x2A7D: return 0xF3; /* ≤ (DEC y) */
        case 0x2A7E: return 0xF2; /* ≥ (DEC z) */
        case 0x2260: return 0xF0; /* ≠ (DEC =; CP437 nearest is ≡) */
        case 0x240B: return 0x0F; /* ☼ (DEC lantern i) */
        case 0x2409: return 0x1A; /* ␉ (DEC '2') */
        case 0x240A: return 0x19; /* ␊ (DEC '5') */
        case 0x240C: return 0x1A; /* ␌ (DEC '3') */
        case 0x240D: return 0x1B; /* ␍ (DEC '4') */
        case 0x240E: return 0x18; /* ␎ (DEC '8') */
        case 0x2424: return 0x19; /* ␤ (DEC '9') */
        case 0x2603: return 0x0F; /* ☼ (snowman fallback) */
        case 0x2660: return 0x06; /* ♠ */
        case 0x2663: return 0x05; /* ♣ */
        case 0x2665: return 0x03; /* ♥ */
        case 0x2666: return 0x04; /* ♦ */
        case 0x266A: return 0x0D; /* ♪ */

        default:
            if (ucs >= 0x2500 && ucs <= 0x257F) return 0xC4; /* fallback box */
            if (ucs >= 0x2580 && ucs <= 0x259F) return 0xDB; /* fallback block */
            return '?';
    }
}

/* ── pen translation ─────────────────────────────────────────────────── */
static uint8_t pen_color(const VTermColor *c, int is_bg) {
    if (!c) return is_bg ? 0 : 7;
    if (VTERM_COLOR_IS_INDEXED(c)) {
        if (c->indexed.idx < 16) return c->indexed.idx;
        return console_color_index((int)c->indexed.idx);
    }
    if (VTERM_COLOR_IS_RGB(c)) {
        return console_color_rgb((uint32_t)c->rgb.red,
                                 (uint32_t)c->rgb.green,
                                 (uint32_t)c->rgb.blue);
    }
    return is_bg ? 0 : 7; /* default fg / bg */
}

static void blit_cell(int row, int col, const VTermScreenCell *cell) {
    uint32_t ucs = cell->chars[0];
    uint8_t  ch  = ucs ? ucs_to_cp437(ucs) : (uint8_t)' ';
    uint8_t  fg  = pen_color(&cell->fg, 0);
    uint8_t  bg  = pen_color(&cell->bg, 1);
    int      rev = cell->attrs.reverse ? 1 : 0;

    if (ucs == 0 || ucs == ' ') ch = ' ';
    if (cell->attrs.conceal) { ch = ' '; fg = bg; }

    console_put_cell((uint32_t)col, (uint32_t)row, ch, fg, bg, rev);
}

/* ── libvterm screen callbacks ───────────────────────────────────────── */
static int cb_damage(VTermRect rect, void *user) {
    (void)user;
    for (int r = rect.start_row; r < rect.end_row; r++) {
        for (int c = rect.start_col; c < rect.end_col; c++) {
            VTermPos pos;
            VTermScreenCell cell;
            pos.row = r;
            pos.col = c;
            if (!vterm_screen_get_cell(g_scr, pos, &cell)) continue;
            blit_cell(r, c, &cell);
        }
    }
    return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user) {
    (void)oldpos;
    (void)user;
    console_set_cursor_visible(visible);
    console_set_cursor_pos((uint32_t)pos.col, (uint32_t)pos.row);
    return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *user) {
    (void)user;
    switch (prop) {
        case VTERM_PROP_CURSORVISIBLE:
            console_set_cursor_visible(val->boolean);
            break;
        case VTERM_PROP_ALTSCREEN:
            /* The emulator keeps its own alternate buffer; the visible screen
             * becomes whatever it damages next, so just drop stale pixels. */
            console_clear();
            break;
        case VTERM_PROP_TITLE:
        case VTERM_PROP_ICONNAME:
            /* OS window title: nothing to set on a raw framebuffer yet. */
            break;
        default:
            break;
    }
    return 1;
}

static int cb_bell(void *user) { (void)user; return 1; }

static int cb_resize(int rows, int cols, void *user) {
    (void)rows; (void)cols; (void)user;
    return 1;
}

static int cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user) {
    (void)cols; (void)cells; (void)user;
    return 0; /* no scrollback buffer yet */
}

static int cb_sb_popline(int cols, VTermScreenCell *cells, void *user) {
    (void)cols; (void)cells; (void)user;
    return 0;
}

static int cb_sb_clear(void *user) { (void)user; return 1; }

static const VTermScreenCallbacks screen_cbs = {
    cb_damage,
    NULL,               /* moverect: the damage rect already covers it */
    cb_movecursor,
    cb_settermprop,
    cb_bell,
    cb_resize,
    cb_sb_pushline,
    cb_sb_popline,
    cb_sb_clear,
};

/* ── public API ──────────────────────────────────────────────────────── */
void vterm_console_init(void) {
    if (g_ready) return;

    int rows = (int)console_rows();
    int cols = (int)console_cols();
    if (rows <= 0 || cols <= 0) {
        serial_puts("[VTERM] console has no geometry yet; deferring init\n");
        return;
    }

    struct VTermBuilder b;
    b.ver           = 0;
    b.rows          = rows;
    b.cols          = cols;
    b.allocator     = &vt_allocator;
    b.allocdata     = NULL;
    b.outbuffer_len = 4096;
    b.tmpbuffer_len = 4096;

    g_vt = vterm_build(&b);
    if (!g_vt) {
        serial_puts("[VTERM] vterm_build failed\n");
        return;
    }
    /* UTF-8 input: without this, GR bytes (0x80+) take the single-94
     * path, which strips the high bit (0xE2 -> 'b') and eats the rest.
     * Every box-drawing tile userspace emits arrives as UTF-8. */
    vterm_set_utf8(g_vt, 1);

    g_state = vterm_obtain_state(g_vt);
    g_scr   = vterm_obtain_screen(g_vt);
    /* Keyboard translations (key event -> escape bytes) flow out through
     * this callback straight into the tty input queue. */
    vterm_output_set_callback(g_vt, vt_out_cb, NULL);
    vterm_screen_set_callbacks(g_scr, &screen_cbs, NULL);
    /* Coalesce repaints: one rect per flush beats per-cell damage. */
    vterm_screen_set_damage_merge(g_scr, VTERM_DAMAGE_SCROLL);
    vterm_screen_enable_altscreen(g_scr, 1);
    vterm_state_set_bold_highbright(g_state, 1);
    vterm_screen_reset(g_scr, 1);

    g_ready = 1;

    serial_puts("[VTERM] libvterm ready (");
    serial_puthex((uint64_t)cols);
    serial_puts("x");
    serial_puthex((uint64_t)rows);
    serial_puts(")\n");
}

void vterm_console_write(const char *buf, size_t len) {
    if (!g_ready) vterm_console_init();
    if (!g_ready || !buf || !len) return;
    vterm_input_write(g_vt, buf, len);
    vterm_screen_flush_damage(g_scr);
    /* The blit repaints whole cells, which wipes the cursor overlay - and the
     * emulator reported cursor motion while parsing, before this flush. Put
     * it back or the cursor never survives an update. */
    console_cursor_repaint();
}

void vterm_console_reset(void) {
    if (!g_ready) { vterm_console_init(); return; }
    console_clear();
    vterm_screen_reset(g_scr, 1);
    vterm_screen_flush_damage(g_scr);
    console_cursor_repaint();
}

void vterm_console_resize(void) {
    if (!g_ready) { vterm_console_init(); return; }
    int rows = (int)console_rows();
    int cols = (int)console_cols();
    if (rows <= 0 || cols <= 0) return;
    vterm_set_size(g_vt, rows, cols);
    vterm_screen_flush_damage(g_scr);
    console_cursor_repaint();
}

int vterm_console_ready(void) { return g_ready; }

/* ── keyboard → host path ───────────────────────────────────────────────
 * libvterm translates keys only in "application" mode via vterm_keyboard_*;
 * a plain terminal must forward its own bytes. Bytes the i8042 already
 * decoded (plain text and ESC-led arrow sequences) are handed to
 * ── keyboard → host path (full libvterm) ────────────────────────────────
 * The i8042 driver decodes scancodes into bytes and hands them to
 * vterm_keyboard_feed(). This layer translates them into libvterm key
 * EVENTS (vterm_keyboard_unichar / vterm_keyboard_key), and libvterm
 * itself decides what bytes the "terminal" sends to the host — encoded
 * per its own DECCKM application-cursor and keypad modes — through the
 * VTermOutputCallback registered in vterm_console_init(). Those bytes land
 * in the tty input queue (vfs_tty_qput) that stdin_read drains.
 *
 * Nothing here emits sequences by hand: the table above only maps the
 * driver's ESC-led byte spellings BACK to key events. Unknown sequences
 * fall through to the queue byte-for-byte, so a key the table does not
 * know can never be swallowed. */

/* libvterm output callback: bytes the emulator emits (key translations)
 * join the tty input queue. Registered once at init. */
static void vt_out_cb(const char *s, size_t len, void *user) {
    (void)user;
    for (size_t i = 0; i < len; i++) vfs_tty_qput(s[i]);
}

static void queue_raw(const char *s, unsigned n) {
    for (unsigned i = 0; i < n; i++) vfs_tty_qput(s[i]);
}

/* Pending raw bytes that arrived mid-sequence (kept outside the tty so
 * they never reach a userspace read half-formed). */
#define HOST_PENDING_MAX 16
static char     host_pending[HOST_PENDING_MAX];
static unsigned host_pending_len = 0;

static void vterm_host_flush_pending(void) {
    queue_raw(host_pending, host_pending_len);
    host_pending_len = 0;
}

/* Exact compare of a NUL-terminated sequence against a table spelling. */
static int seq_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Match a completed ESC-led sequence against the table and return the key
 * event it represents, or VTERM_KEY_NONE when unknown. The caller must
 * NUL-terminate: an earlier longer key (PgUp's "\x1b[5~") leaves bytes past
 * the third slot, and comparing against those stale bytes stopped 3-byte
 * arrow sequences from matching after such a key had been pressed. */
static uint32_t seq_lookup_key(const char *seq) {
    const unsigned n = sizeof(vt_keys) / sizeof(vt_keys[0]);
    for (unsigned i = 0; i < n; i++) {
        if (seq_eq(seq, vt_keys[i].normal) || seq_eq(seq, vt_keys[i].app))
            return vt_keys[i].ucs;
    }
    return VTERM_KEY_NONE;
}

static void vterm_host_feed(char c) {
    if (host_pending_len == 0 && c != 0x1B) {
        /* Plain byte. Printables go through libvterm's keyboard layer so its
         * model stays consistent; the emulator emits the (UTF-8) byte back
         * through vt_out_cb. Control bytes bypass the keyboard layer — the
         * driver's ISIG handling and the line discipline own them — and
         * join the queue raw. */
        if (c >= 0x20 && (uint8_t)c != 0x7F) {
            if (g_ready && g_vt)
                vterm_keyboard_unichar(g_vt, (uint32_t)(uint8_t)c, VTERM_MOD_NONE);
            else
                vfs_tty_qput(c); /* emulator not up yet: raw is all we have */
        } else {
            vfs_tty_qput(c);
        }
        return;
    }
    if (host_pending_len < HOST_PENDING_MAX)
        host_pending[host_pending_len++] = c;

    /* Completed CSI/SS3 sequences end on a letter (or '~' for keypad). */
    if (host_pending_len >= 2 && host_pending[0] == 0x1B) {
        if (host_pending[1] == '[' || host_pending[1] == 'O') {
            char last = host_pending[host_pending_len - 1];
            if ((last >= 'A' && last <= 'Z') || (last >= 'a' && last <= 'z') || last == '~') {
                if (host_pending_len < HOST_PENDING_MAX)
                    host_pending[host_pending_len] = '\0';
                uint32_t key = seq_lookup_key(host_pending);
                host_pending_len = 0;
                if (key != VTERM_KEY_NONE) {
                    /* libvterm emits the DECCKM-correct spelling itself. */
                    if (g_ready && g_vt)
                        vterm_keyboard_key(g_vt, key, VTERM_MOD_NONE);
                } else {
                    /* Unknown sequence: hand it through untouched. */
                    vterm_host_flush_pending();
                }
                return;
            }
            if (host_pending_len >= HOST_PENDING_MAX) vterm_host_flush_pending();
            return;
        }
        /* Lone ESC followed by a non-'['/'O' byte: Escape key, then the
         * second byte re-enters the state machine on its own merits. */
        if (host_pending_len == 2) {
            char second = host_pending[1];
            host_pending_len = 0;
            vfs_tty_qput(0x1B);
            vterm_host_feed(second);
            return;
        }
    }
    if (host_pending_len >= HOST_PENDING_MAX) vterm_host_flush_pending();
}

/* Public entry points used by the tty layer (see vterm_glue.h). */
void vterm_keyboard_feed(char c) { vterm_host_feed(c); }

void vterm_keyboard_code(uint32_t ucs, uint32_t mod) {
    if (g_ready && g_vt) vterm_keyboard_unichar(g_vt, ucs, mod);
}