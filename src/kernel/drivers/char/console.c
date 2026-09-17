/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * console.c - Framebuffer graphical text console with hardware scrolling.
 */

#include <stdint.h>
#include <stddef.h>
#include "console.h"
#include "include/font.h"
#include "include/serial.h"
#include "mm/kheap.h"

static struct limine_framebuffer *fb = NULL;
static uint32_t fg = 0xFFFFFF;
static uint32_t bg = 0x0A0A1A;
static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;
static uint32_t saved_x = 0;
static uint32_t saved_y = 0;
static uint32_t cols = 0;
static uint32_t rows = 0;

/* Current SGR attributes (palette indices; reset by CSI 0 m). */
static uint8_t cur_fg = 7;
static uint8_t cur_bg = 0;
static int cur_reverse = 0;

/* VGA text palette (SGR colors). Entry 7 is the default fg (white). */
static uint32_t palette[16] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
    0x0000AA, 0xAA00AA, 0x00AAAA, 0xFFFFFF,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
    0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF
};

/* Text cell shadow buffer (exact restores for the XOR cursor). */
struct text_cell {
    uint8_t ch;
    uint8_t fg;
    uint8_t bg;
    uint8_t rev;
};

#define TEXT_MAX_COLS 256
#define TEXT_MAX_ROWS 128

static struct text_cell textbuf[TEXT_MAX_COLS * TEXT_MAX_ROWS];

/* Visible text cursor (DECTCEM, default on). Painted by XOR so no text
 * buffer is needed to restore the cell underneath. */
static int cursor_visible = 1;
/* True while the cursor's XOR pixels are actually on screen. The overlay is
 * XORed in (cursor_paint) and XORed out (cell_paint of the same cell), so
 * painting it twice without a repaint in between erases it - the flag is what
 * lets a repaint be idempotent. */
static int cursor_painted = 0;

/* Native framebuffer format (from Limine; VirtualBox may give 16/24bpp). */
static uint8_t px_bytes = 4;
static uint8_t r_size = 8, r_shift = 16;
static uint8_t g_size = 8, g_shift = 8;
static uint8_t b_size = 8, b_shift = 0;

static uint32_t chan_fit(uint32_t c8, uint8_t size) {
    if (size == 0) return 0;
    if (size >= 8) return c8 & 0xFF;
    return (c8 * ((1U << size) - 1) + 127) / 255;
}

static uint32_t to_native(uint32_t rgb) {
    uint32_t v = 0;
    v |= chan_fit((rgb >> 16) & 0xFF, r_size) << r_shift;
    v |= chan_fit((rgb >> 8) & 0xFF, g_size) << g_shift;
    v |= chan_fit(rgb & 0xFF, b_size) << b_shift;
    return v;
}

static inline void store_px(uint8_t *px, uint32_t v) {
    if (px_bytes == 4) {
        *(uint32_t *)px = v;
    } else if (px_bytes == 2) {
        px[0] = (uint8_t)v;
        px[1] = (uint8_t)(v >> 8);
    } else if (px_bytes == 3) {
        px[0] = (uint8_t)v;
        px[1] = (uint8_t)(v >> 8);
        px[2] = (uint8_t)(v >> 16);
    } else {
        /* Indexed/packed fallback: nearest mono */
        uint32_t lum = (v & 0xFF) + ((v >> 8) & 0xFF) + ((v >> 16) & 0xFF);
        px[0] = (lum >= 384) ? 0xFF : 0x00;
    }
}

static inline void putpix(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb || x >= fb->width || y >= fb->height) return;
    uint8_t *px = (uint8_t *)fb->address + (uint64_t)y * fb->pitch +
                  (uint64_t)x * px_bytes;
    store_px(px, to_native(color));
}

static void cell_paint(uint32_t cx, uint32_t cy);

static void draw_char(char c, uint32_t cx, uint32_t cy) {
    if (cx < cols && cy < rows) {
        struct text_cell *cell = &textbuf[cy * cols + cx];
        cell->ch = (uint8_t)c;
        cell->fg = cur_fg;
        cell->bg = cur_bg;
        cell->rev = cur_reverse ? 1 : 0;
    }
    cell_paint(cx, cy);
}

/* ── CP437 graphics glyphs (0x80-0xFF) ───────────────────────────────────
 * font8x16 carries ASCII only; the high half of CP437 is where the tiles
 * live that every boxed TUI needs.  slang resolves the `acsc` capability of
 * TERM=linux to these codes, so without them a window border collapses to a
 * blank cell and a dialog appears to have no frame at all.
 *
 * Each entry is one glyph: 16 rows, bit 7 = leftmost pixel of the 8px cell. */
struct cp437_glyph {
    uint8_t code;
    uint8_t rows[16];
};

static const struct cp437_glyph cp437_patch[] = {
    /* single-line box drawing: a 2px stroke through the cell centre */
    { 0xB3, { 0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18 } }, /* │ */
    { 0xC4, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ─ */
    { 0xDA, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18 } }, /* ┌ */
    { 0xBF, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF8,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18 } }, /* ┐ */
    { 0xC0, { 0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* └ */
    { 0xD9, { 0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xF8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ┘ */
    { 0xC3, { 0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18 } }, /* ├ */
    { 0xB4, { 0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xF8,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18 } }, /* ┤ */
    { 0xC2, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18 } }, /* ┬ */
    { 0xC1, { 0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ┴ */
    { 0xC5, { 0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18 } }, /* ┼ */
    /* double-line box drawing */
    { 0xBA, { 0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24 } }, /* ║ */
    { 0xCD, { 0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ═ */
    { 0xC9, { 0x00,0x00,0x00,0x00,0x00,0x00,0x3F,0x20,0x3F,0x24,0x24,0x24,0x24,0x24,0x24,0x24 } }, /* ╔ */
    { 0xBB, { 0x00,0x00,0x00,0x00,0x00,0x00,0xFC,0x04,0xFC,0x24,0x24,0x24,0x24,0x24,0x24,0x24 } }, /* ╗ */
    { 0xC8, { 0x24,0x24,0x24,0x24,0x24,0x24,0x3F,0x20,0x3F,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ╚ */
    { 0xBC, { 0x24,0x24,0x24,0x24,0x24,0x24,0xFC,0x04,0xFC,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ╝ */
    { 0xCC, { 0x24,0x24,0x24,0x24,0x24,0x24,0x3F,0x20,0x3F,0x24,0x24,0x24,0x24,0x24,0x24,0x24 } }, /* ╠ */
    { 0xB9, { 0x24,0x24,0x24,0x24,0x24,0x24,0xFC,0x04,0xFC,0x24,0x24,0x24,0x24,0x24,0x24,0x24 } }, /* ╣ */
    { 0xCB, { 0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0xFF,0x24,0x24,0x24,0x24,0x24,0x24,0x24 } }, /* ╦ */
    { 0xCA, { 0x24,0x24,0x24,0x24,0x24,0x24,0xFF,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ╩ */
    { 0xCE, { 0x24,0x24,0x24,0x24,0x24,0x24,0xFF,0x00,0xFF,0x24,0x24,0x24,0x24,0x24,0x24,0x24 } }, /* ╬ */
    /* mixed single/double box drawing */
    { 0xD5, { 0x00,0x00,0x00,0x00,0x00,0x00,0x3F,0x00,0x3F,0x18,0x18,0x18,0x18,0x18,0x18,0x18 } }, /* ╒ */
    { 0xD6, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0x1F,0x24,0x24,0x24,0x24,0x24,0x24,0x24 } }, /* ╓ */
    { 0xB8, { 0x00,0x00,0x00,0x00,0x00,0x00,0xFC,0x00,0xFC,0x18,0x18,0x18,0x18,0x18,0x18,0x18 } }, /* ╕ */
    { 0xB7, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF8,0xF8,0x24,0x24,0x24,0x24,0x24,0x24,0x24 } }, /* ╖ */
    { 0xD4, { 0x18,0x18,0x18,0x18,0x18,0x18,0x3F,0x00,0x3F,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ╘ */
    { 0xD3, { 0x24,0x24,0x24,0x24,0x24,0x24,0x1F,0x1F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ╙ */
    { 0xBE, { 0x18,0x18,0x18,0x18,0x18,0x18,0xFC,0x00,0xFC,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ╛ */
    { 0xBD, { 0x24,0x24,0x24,0x24,0x24,0x24,0xF8,0xF8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ╜ */
    { 0xC6, { 0x24,0x24,0x24,0x24,0x24,0x24,0x3F,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24 } }, /* ╞ */
    { 0xC7, { 0x18,0x18,0x18,0x18,0x18,0x18,0x3F,0x00,0x3F,0x18,0x18,0x18,0x18,0x18,0x18,0x18 } }, /* ╟ */
    { 0xB5, { 0x24,0x24,0x24,0x24,0x24,0x24,0xFC,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24 } }, /* ╡ */
    { 0xB6, { 0x18,0x18,0x18,0x18,0x18,0x18,0xFC,0x00,0xFC,0x18,0x18,0x18,0x18,0x18,0x18,0x18 } }, /* ╢ */
    { 0xD1, { 0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18 } }, /* ╤ */
    { 0xD2, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0x24,0x24,0x24,0x24,0x24,0x24,0x24 } }, /* ╥ */
    { 0xCF, { 0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ╧ */
    { 0xD0, { 0x24,0x24,0x24,0x24,0x24,0x24,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ╨ */
    { 0xD8, { 0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0x00,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18 } }, /* ╪ */
    { 0xD7, { 0x24,0x24,0x24,0x24,0x24,0x24,0xFF,0xFF,0x00,0x24,0x24,0x24,0x24,0x24,0x24,0x24 } }, /* ╫ */
    /* shade and block fills: scrollbars, gauges, button shadows */
    { 0xB0, { 0x88,0x22,0x88,0x22,0x88,0x22,0x88,0x22,0x88,0x22,0x88,0x22,0x88,0x22,0x88,0x22 } }, /* ░ */
    { 0xB1, { 0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA } }, /* ▒ */
    { 0xB2, { 0xEE,0xBB,0xEE,0xBB,0xEE,0xBB,0xEE,0xBB,0xEE,0xBB,0xEE,0xBB,0xEE,0xBB,0xEE,0xBB } }, /* ▓ */
    { 0xDB, { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF } }, /* █ */
    { 0xDF, { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ▀ */
    { 0xDC, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF } }, /* ▄ */
    { 0xDD, { 0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0 } }, /* ▌ */
    { 0xDE, { 0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F } }, /* ▐ */
    /* bullets, arrows and marks used by list/radio widgets */
    { 0x04, { 0x00,0x00,0x00,0x00,0x18,0x3C,0x7E,0xFF,0xFF,0x7E,0x3C,0x18,0x00,0x00,0x00,0x00 } }, /* ♦ */
    { 0x07, { 0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x3C,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* • */
    { 0x0F, { 0x00,0x00,0x18,0x3C,0x7E,0xFF,0xFF,0xFF,0x7E,0x3C,0x18,0x00,0x00,0x00,0x00,0x00 } }, /* ☼ */
    { 0x10, { 0x00,0x00,0x40,0x60,0x70,0x78,0x7C,0x7E,0x7C,0x78,0x70,0x60,0x40,0x00,0x00,0x00 } }, /* ► */
    { 0x11, { 0x00,0x00,0x02,0x06,0x0E,0x1E,0x3E,0x7E,0x3E,0x1E,0x0E,0x06,0x02,0x00,0x00,0x00 } }, /* ◄ */
    { 0x18, { 0x00,0x10,0x38,0x7C,0xFE,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x00,0x00,0x00,0x00 } }, /* ↑ */
    { 0x19, { 0x00,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0xFE,0x7C,0x38,0x10,0x00,0x00,0x00,0x00 } }, /* ↓ */
    { 0x1A, { 0x00,0x00,0x00,0x00,0x00,0x08,0x0C,0xFE,0x0C,0x08,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* → */
    { 0x1B, { 0x00,0x00,0x00,0x00,0x00,0x20,0x60,0xFE,0x60,0x20,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ← */
    { 0x1E, { 0x00,0x10,0x38,0x7C,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ▲ */
    { 0x1F, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x7C,0x38,0x10,0x00,0x00,0x00,0x00 } }, /* ▼ */
    /* misc punctuation the widgets lean on */
    { 0xB7, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* · */
    { 0xF8, { 0x00,0x00,0x18,0x3C,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ° */
    { 0xFE, { 0x00,0x00,0x3C,0x3C,0x3C,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ■ */
    /* math marks the acsc table references (g y z |) */
    { 0xF0, { 0x00,0x00,0x00,0x00,0x7E,0x7E,0x00,0x7E,0x7E,0x00,0x7E,0x7E,0x00,0x00,0x00,0x00 } }, /* ≡ */
    { 0xF1, { 0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x7E,0x7E,0x00,0x00,0x00,0x00,0x00,0x00 } }, /* ± */
    { 0xF2, { 0x00,0x00,0x7E,0x7E,0x02,0x04,0x08,0x10,0x08,0x04,0x02,0x7E,0x7E,0x00,0x00,0x00 } }, /* ≥ */
    { 0xF3, { 0x00,0x00,0x7E,0x7E,0x40,0x20,0x10,0x08,0x10,0x20,0x40,0x7E,0x7E,0x00,0x00,0x00 } }, /* ≤ */
};

static const uint8_t cp437_blank[16] = { 0 };
static const uint8_t *cp437_index[128]; /* 0x80..0xFF -> glyph rows, or NULL */
static int cp437_ready = 0;

static void cp437_build(void) {
    const unsigned n = sizeof(cp437_patch) / sizeof(cp437_patch[0]);
    for (unsigned i = 0; i < n; i++) {
        uint8_t code = cp437_patch[i].code;
        if (code >= 0x80) cp437_index[code - 0x80] = cp437_patch[i].rows;
    }
    cp437_ready = 1;
}

/* Glyph lookup: ASCII from the ROM table, CP437 graphics from the patch
 * table.  Anything not shipped renders as a blank cell rather than reading
 * off the end of font8x16. */
static const uint8_t *glyph_for(uint8_t ch) {
    if (ch < 0x80) return &font8x16[ch * 16];
    if (!cp437_ready) cp437_build();
    const uint8_t *g = cp437_index[ch - 0x80];
    return g ? g : cp437_blank;
}

/* DEC Special Graphics (selected with ESC ( 0): ASCII letters are displayed
 * as line-drawing characters. slang/ncurses reach for this when a terminal
 * has no usable acsc, so honour it by mapping the letters onto the CP437
 * tiles above — otherwise a border shows up as the literal letters j q x l. */
static int g_special_graphics = 0;

static uint8_t dec_graphics_remap(uint8_t c) {
    switch (c) {
        case '_': return ' ';
        case 'a': return 0xB1; /* ▒ */
        case 'f': return 0xF8; /* ° */
        case 'j': return 0xD9; /* ┘ */
        case 'k': return 0xBF; /* ┐ */
        case 'l': return 0xDA; /* ┌ */
        case 'm': return 0xC0; /* └ */
        case 'n': return 0xC5; /* ┼ */
        case 'o': return 0xC4; /* ⎺ */
        case 'p': return 0xC4; /* ⎻ */
        case 'q': return 0xC4; /* ─ */
        case 'r': return 0xC4; /* ⎼ */
        case 's': return 0xC4; /* ⎽ */
        case 't': return 0xC3; /* ├ */
        case 'u': return 0xB4; /* ┤ */
        case 'v': return 0xC1; /* ┴ */
        case 'w': return 0xC2; /* ┬ */
        case 'x': return 0xB3; /* │ */
        case 'y': return 0xF3; /* ≤  (ACS_LEQUAL) */
        case 'z': return 0xF2; /* ≥  (ACS_GEQUAL) */
        case '0': return 0xDB; /*   (ACS_BLOCK) */
        case ',': return 0x11; /* ◄  (ACS_LARROW) */
        case '+': return 0x10; /* ►  (ACS_RARROW) */
        case '-': return 0x18; /* ↑  (ACS_UARROW) */
        case '.': return 0x19; /* ↓  (ACS_DARROW) */
        case '`': return 0x04; /* ♦  (ACS_DIAMOND) */
        case 'g': return 0xF1; /* ±  (ACS_PLMINUS) */
        case 'h': return 0xB1; /* ▒  (ACS_BOARD) */
        case 'i': return 0x0F; /* ☼  (ACS_LANTERN) */
        case '|': return 0xF0; /* ≡  (ACS_NEQUAL) */
        case '~': return 0xB7; /* · */
        default:  return c;
    }
}

/* Repaint one cell from the shadow buffer (exact cursor restore). */
static void cell_paint(uint32_t cx, uint32_t cy) {
    if (cx >= cols || cy >= rows) return;
    /* Any repaint of the cursor's own cell removes the XOR overlay that was
     * sitting on top of it, so the tracking bit has to follow. */
    if (cx == cursor_x && cy == cursor_y) cursor_painted = 0;
    struct text_cell *cell = &textbuf[cy * cols + cx];
    uint32_t cfg = palette[cell->fg & 15];
    uint32_t cbg = palette[cell->bg & 15];
    if (cell->rev) {
        uint32_t t = cfg;
        cfg = cbg;
        cbg = t;
    }
    uint8_t uc = cell->ch ? cell->ch : ' ';
    if (g_special_graphics && uc < 0x80) uc = dec_graphics_remap(uc);
    const uint8_t *glyph = glyph_for(uc);

    for (int y = 0; y < 16; y++) {
        uint8_t line = glyph[y];
        for (int x = 0; x < 8; x++) {
            if (line & (0x80 >> x)) {
                putpix(cx * 8 + x, cy * 16 + y, cfg);
            } else {
                putpix(cx * 8 + x, cy * 16 + y, cbg);
            }
        }
    }
}

/* Cursor hide restores the exact cell; show XORs it. Hide is idempotent,
 * so stray hides never smear (every show pairs with a prior hide). */
static void cursor_paint(void);

static void cursor_hide(void) {
    if (cursor_visible) cell_paint(cursor_x, cursor_y);
}

static void cursor_show(void) {
    if (cursor_visible) cursor_paint();
}

/* Scroll region state (helpers defined below; full screen by default). */
static uint32_t region_top = 0;
static uint32_t region_bottom = 0;
static int region_active = 0;

static void blank_text_row(uint32_t y);
static void blank_pixel_row(uint32_t y);
static void copy_text_row(uint32_t dst_y, uint32_t src_y);
static void copy_pixel_row(uint32_t dst_y, uint32_t src_y);

static uint32_t region_end(void) {
    if (region_active) return region_bottom;
    return rows ? rows - 1 : 0;
}

static void scroll(void) {
    if (!fb) return;

    /* Scroll the active region up by one text row (16 pixels). */
    uint32_t top = region_active ? region_top : 0;
    uint32_t bot = region_end();
    if (top >= rows || bot >= rows || top > bot) return;

    for (uint32_t y = top; y < bot; y++) {
        copy_text_row(y, y + 1);
        copy_pixel_row(y, y + 1);
    }
    blank_text_row(bot);
    blank_pixel_row(bot);
}

void console_init(struct limine_framebuffer *framebuffer, uint32_t fg_color, uint32_t bg_color) {
    fb = framebuffer;
    fg = fg_color;
    bg = bg_color;
    cur_fg = 7;
    cur_bg = 0;
    cur_reverse = 0;
    cursor_x = 0;
    cursor_y = 0;
    cursor_visible = 1;

    /* Adopt the bootloader mode: 15/16/24/32bpp direct color. Anything
     * else falls back to 32-bit writes (best effort). */
    if (fb->memory_model == 1 &&
        (fb->bpp == 15 || fb->bpp == 16 || fb->bpp == 24 || fb->bpp == 32)) {
        px_bytes = (fb->bpp + 7) / 8;
        r_size = fb->red_mask_size;   r_shift = fb->red_mask_shift;
        g_size = fb->green_mask_size; g_shift = fb->green_mask_shift;
        b_size = fb->blue_mask_size;  b_shift = fb->blue_mask_shift;
        if (r_size + g_size + b_size == 0) {
            r_size = 8; r_shift = 16;
            g_size = 8; g_shift = 8;
            b_size = 8; b_shift = 0;
            px_bytes = 4;
        }
    } else {
        px_bytes = 4;
        r_size = 8; r_shift = 16;
        g_size = 8; g_shift = 8;
        b_size = 8; b_shift = 0;
    }

    cols = fb->width / 8;
    rows = fb->height / 16;
    if (cols > TEXT_MAX_COLS) cols = TEXT_MAX_COLS;
    if (rows > TEXT_MAX_ROWS) rows = TEXT_MAX_ROWS;

    serial_puts("[FB] mode: ");
    serial_puthex(fb->width);
    serial_puts("x");
    serial_puthex(fb->height);
    serial_puts("x");
    serial_puthex(fb->bpp);
    serial_puts(" masks r=");
    serial_puthex(r_size);
    serial_puts("/");
    serial_puthex(r_shift);
    serial_puts(" g=");
    serial_puthex(g_size);
    serial_puts("/");
    serial_puthex(g_shift);
    serial_puts(" b=");
    serial_puthex(b_size);
    serial_puts("/");
    serial_puthex(b_shift);
    serial_puts("\n");

    for (uint32_t i = 0; i < cols * rows; i++) {
        textbuf[i].ch = ' ';
        textbuf[i].fg = 7;
        textbuf[i].bg = 0;
        textbuf[i].rev = 0;
    }

    console_clear();
}

void console_clear(void) {
    if (!fb) return;
    for (uint32_t i = 0; i < cols * rows; i++) {
        textbuf[i].ch = ' ';
        textbuf[i].fg = cur_fg;
        textbuf[i].bg = cur_bg;
        textbuf[i].rev = 0;
    }
    uint32_t bgv = to_native(palette[cur_bg & 15]);
    for (uint32_t y = 0; y < fb->height; y++) {
        uint8_t *row = (uint8_t *)fb->address + (uint64_t)y * fb->pitch;
        for (uint32_t x = 0; x < fb->width; x++) {
            store_px(row + (uint64_t)x * px_bytes, bgv);
        }
    }
    cursor_x = 0;
    cursor_y = 0;
}

void console_get_size(uint16_t *r, uint16_t *c) {
    if (r) *r = (uint16_t)rows;
    if (c) *c = (uint16_t)cols;
}

/* ── Minimal ANSI CSI escape interpreter (for vi/less/top) ─────────────── */
/* Cursor motion, positioning, erase, SGR colors and cursor visibility.     */
/* Alt-screen toggles and scroll regions are parsed and ignored.            */
static int esc_state = 0; /* 0=normal, 1=after ESC, 2=in CSI, 3=extra, 4=OSC */
static int csi_params[8];
static int csi_idx = 0; /* which param is being accumulated */
static int csi_private = 0; /* '?' seen: DECTCEM cursor visibility etc. */
static char esc_charset_intro = 0; /* designator that opened esc_state 3 */

static void cursor_paint(void) {
    if (!fb) return;
    cursor_painted = 1;
    uint32_t cx = cursor_x * 8;
    uint32_t cy = cursor_y * 16;
    uint8_t *fbm = (uint8_t *)fb->address;
    for (uint32_t y = 0; y < 16 && cy + y < fb->height; y++) {
        uint8_t *row = fbm + (uint64_t)(cy + y) * fb->pitch;
        for (uint32_t x = 0; x < 8 && cx + x < fb->width; x++) {
            uint8_t *px = row + (uint64_t)(cx + x) * px_bytes;
            if (px_bytes == 4) {
                *(uint32_t *)px ^= 0xFFFFFF;
            } else if (px_bytes == 2) {
                px[0] ^= 0xFF;
                px[1] ^= 0xFF;
            } else if (px_bytes == 3) {
                px[0] ^= 0xFF;
                px[1] ^= 0xFF;
                px[2] ^= 0xFF;
            } else if (px_bytes >= 1) {
                px[0] ^= 0xFF;
            }
        }
    }
}

static void csi_reset(void) {
    esc_state = 0;
    for (int i = 0; i < 8; i++) csi_params[i] = 0;
    csi_idx = 0;
    csi_private = 0;
}

static int sgr_bold = 0;

/* Nearest palette entry for an RGB triple (weighted distance). */
static uint8_t nearest_palette(uint32_t r, uint32_t g, uint32_t b) {
    uint8_t best = 0;
    uint32_t best_d = 0xFFFFFFFFU;
    for (uint8_t i = 0; i < 16; i++) {
        int32_t dr = (int32_t)((palette[i] >> 16) & 0xFF) - (int32_t)r;
        int32_t dg = (int32_t)((palette[i] >> 8) & 0xFF) - (int32_t)g;
        int32_t db = (int32_t)(palette[i] & 0xFF) - (int32_t)b;
        uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 6 + db * db);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

/* Map an xterm 256-color index to the nearest palette entry. */
static uint8_t quantize_256(int n) {
    uint32_t r, g, b;
    if (n < 0) return 7;
    if (n < 16) return (uint8_t)n;
    if (n < 232) {
        int c = n - 16;
        int cr = (c / 36) % 6, cg = (c / 6) % 6, cb = c % 6;
        r = cr ? (uint32_t)(55 + cr * 40) : 0;
        g = cg ? (uint32_t)(55 + cg * 40) : 0;
        b = cb ? (uint32_t)(55 + cb * 40) : 0;
    } else {
        r = g = b = (uint32_t)(8 + (n - 232) * 10);
    }
    return nearest_palette(r, g, b);
}

/* Scroll region helpers (state declared above). */

/* Shift the tail of a row left by n cells (CSI P), blanking the gap. */
static void row_delete_chars(uint32_t y, uint32_t from, uint32_t n) {
    if (y >= rows || from >= cols) return;
    for (uint32_t x = from; x < cols; x++) {
        struct text_cell *dst = &textbuf[y * cols + x];
        uint32_t sx = x + n;
        if (sx < cols) {
            *dst = textbuf[y * cols + sx];
        } else {
            dst->ch = ' ';
            dst->fg = cur_fg;
            dst->bg = cur_bg;
            dst->rev = 0;
        }
        cell_paint(x, y);
    }
}

/* Shift the tail of a row right by n cells (CSI @), blanking the gap. */
static void row_insert_chars(uint32_t y, uint32_t from, uint32_t n) {
    if (y >= rows || from >= cols) return;
    for (uint32_t x = cols; x-- > from; ) {
        struct text_cell *dst = &textbuf[y * cols + x];
        if (x >= from + n) {
            *dst = textbuf[y * cols + x - n];
        } else {
            dst->ch = ' ';
            dst->fg = cur_fg;
            dst->bg = cur_bg;
            dst->rev = 0;
        }
        cell_paint(x, y);
    }
}

/* Blank n cells in place without moving the cursor (CSI X). */
static void row_erase_chars(uint32_t y, uint32_t from, uint32_t n) {
    if (y >= rows) return;
    for (uint32_t x = from; x < cols && x < from + n; x++) {
        struct text_cell *cell = &textbuf[y * cols + x];
        cell->ch = ' ';
        cell->fg = cur_fg;
        cell->bg = cur_bg;
        cell->rev = 0;
        cell_paint(x, y);
    }
}

static void blank_text_row(uint32_t y) {
    if (y >= rows) return;
    for (uint32_t x = 0; x < cols; x++) {
        struct text_cell *cell = &textbuf[y * cols + x];
        cell->ch = ' ';
        cell->fg = cur_fg;
        cell->bg = cur_bg;
        cell->rev = 0;
    }
}

static void blank_pixel_row(uint32_t y) {
    if (!fb) return;
    uint32_t bgv = to_native(palette[cur_bg & 15]);
    for (uint32_t ypix = y * 16; ypix < (y + 1) * 16 && ypix < fb->height; ypix++) {
        uint8_t *row = (uint8_t *)fb->address + (uint64_t)ypix * fb->pitch;
        for (uint32_t x = 0; x < fb->width; x++) {
            store_px(row + (uint64_t)x * px_bytes, bgv);
        }
    }
}

static void copy_text_row(uint32_t dst_y, uint32_t src_y) {
    if (dst_y >= rows || src_y >= rows) return;
    struct text_cell *dst = &textbuf[dst_y * cols];
    struct text_cell *src = &textbuf[src_y * cols];
    for (uint32_t x = 0; x < cols; x++) dst[x] = src[x];
}

static void copy_pixel_row(uint32_t dst_y, uint32_t src_y) {
    if (!fb) return;
    uint8_t *fbm = (uint8_t *)fb->address;
    uint32_t copy_bytes = fb->width * px_bytes;
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t dy = dst_y * 16 + i;
        uint32_t sy = src_y * 16 + i;
        if (dy >= fb->height || sy >= fb->height) break;
        uint8_t *dst = fbm + (uint64_t)dy * fb->pitch;
        uint8_t *src = fbm + (uint64_t)sy * fb->pitch;
        for (uint32_t b = 0; b < copy_bytes; b++) dst[b] = src[b];
    }
}

static void csi_sgr(void) {
    /* Bare "m" (no params) resets. */
    if (csi_idx == 0 && csi_params[0] == 0) {
        cur_fg = 7;
        cur_bg = 0;
        cur_reverse = 0;
        sgr_bold = 0;
        return;
    }
    for (int i = 0; i <= csi_idx && i < 8; i++) {
        int p = csi_params[i];
        if (p == 0) {
            cur_fg = 7;
            cur_bg = 0;
            cur_reverse = 0;
            sgr_bold = 0;
        } else if (p == 1) {
            sgr_bold = 1;
        } else if (p == 2 || p == 3 || p == 4 || p == 5) {
            /* faint/italic/underline/blink: accepted, not rendered */
        } else if (p == 7) {
            cur_reverse = 1;
        } else if (p == 22) {
            sgr_bold = 0;
        } else if (p == 24) {
            /* underline off: ignore */
        } else if (p == 27) {
            cur_reverse = 0;
        } else if (p == 39) {
            cur_fg = 7;
        } else if (p == 49) {
            cur_bg = 0;
        } else if (p >= 30 && p <= 37) {
            cur_fg = (uint8_t)(sgr_bold ? p - 30 + 8 : p - 30);
        } else if (p >= 90 && p <= 97) {
            cur_fg = (uint8_t)(p - 90 + 8);
        } else if (p >= 40 && p <= 47) {
            /* setab for TERM=linux expands to \e[4Nm; this is how a dialog
             * gets its panel colour. Without this branch every background is
             * dropped and a boxed TUI renders monochrome. */
            cur_bg = (uint8_t)(p - 40);
        } else if (p >= 100 && p <= 107) {
            cur_bg = (uint8_t)(p - 100 + 8);
        } else if ((p == 38 || p == 48) && i + 2 <= csi_idx) {
            int is_bg = (p == 48);
            if (csi_params[i + 1] == 5) {
                uint8_t idx = quantize_256(csi_params[i + 2]);
                if (is_bg) cur_bg = idx;
                else cur_fg = idx;
                i += 2;
            } else if (csi_params[i + 1] == 2 && i + 4 <= csi_idx) {
                uint8_t idx = nearest_palette(
                    (uint32_t)csi_params[i + 2],
                    (uint32_t)csi_params[i + 3],
                    (uint32_t)csi_params[i + 4]);
                if (is_bg) cur_bg = idx;
                else cur_fg = idx;
                i += 4;
            }
        }
    }
}

static void clear_line_part(uint32_t y, uint32_t x0, uint32_t x1) {
    if (!fb || y >= rows) return;
    if (x0 >= cols) return;
    if (x1 > cols) x1 = cols;
    for (uint32_t x = x0; x < x1; x++) {
        struct text_cell *cell = &textbuf[y * cols + x];
        cell->ch = ' ';
        cell->fg = cur_fg;
        cell->bg = cur_bg;
        cell->rev = 0;
    }
    uint32_t bgv = to_native(palette[cur_bg & 15]);
    uint8_t *fbm = (uint8_t *)fb->address;
    for (uint32_t ypix = y * 16; ypix < (y + 1) * 16 && ypix < fb->height; ypix++) {
        uint8_t *row = fbm + (uint64_t)ypix * fb->pitch;
        for (uint32_t x = x0 * 8; x < x1 * 8; x++) {
            store_px(row + (uint64_t)x * px_bytes, bgv);
        }
    }
}

static void repaint_all(void) {
    for (uint32_t y = 0; y < rows; y++) {
        for (uint32_t x = 0; x < cols; x++) {
            cell_paint(x, y);
        }
    }
}

/* Alternate screen (DECSET 1049): stash main screen, blank, restore on exit. */
static struct text_cell *altbuf = NULL;
static uint32_t alt_cx, alt_cy;
static uint8_t alt_fg, alt_bg;
static int alt_rev, alt_vis, alt_ract;
static uint32_t alt_rtop, alt_rbot;
static int alt_active = 0;

static void altscreen(int on) {
    if (on && !alt_active) {
        if (!altbuf) {
            altbuf = (struct text_cell *)kmalloc(
                (size_t)cols * rows * sizeof(struct text_cell));
            if (!altbuf) return;
        }
        for (uint32_t i = 0; i < cols * rows; i++) altbuf[i] = textbuf[i];
        alt_cx = cursor_x;
        alt_cy = cursor_y;
        alt_fg = cur_fg;
        alt_bg = cur_bg;
        alt_rev = cur_reverse;
        alt_vis = cursor_visible;
        alt_rtop = region_top;
        alt_rbot = region_bottom;
        alt_ract = region_active;
        alt_active = 1;
        for (uint32_t i = 0; i < cols * rows; i++) {
            textbuf[i].ch = ' ';
            textbuf[i].fg = cur_fg;
            textbuf[i].bg = cur_bg;
            textbuf[i].rev = 0;
        }
        region_active = 0;
        repaint_all();
    } else if (!on && alt_active) {
        alt_active = 0;
        for (uint32_t i = 0; i < cols * rows; i++) textbuf[i] = altbuf[i];
        cursor_x = alt_cx;
        cursor_y = alt_cy;
        cur_fg = alt_fg;
        cur_bg = alt_bg;
        cur_reverse = alt_rev;
        cursor_visible = alt_vis;
        region_top = alt_rtop;
        region_bottom = alt_rbot;
        region_active = alt_ract;
        if (cursor_x >= cols) cursor_x = cols - 1;
        if (cursor_y >= rows) cursor_y = rows - 1;
        repaint_all();
        if (cursor_visible) cursor_paint();
    }
}

static void csi_dispatch(char final) {
    int p0 = csi_params[0];
    int p1 = csi_params[1];
    uint32_t n;

    switch (final) {
        case 'A': /* cursor up */
            n = p0 ? (uint32_t)p0 : 1;
            cursor_y = (n > cursor_y) ? 0 : cursor_y - n;
            break;
        case 'B': /* cursor down (clamped, no scroll) */
        case 'e':
            n = p0 ? (uint32_t)p0 : 1;
            cursor_y += n;
            if (cursor_y > region_end()) cursor_y = region_end();
            break;
        case 'C': /* cursor forward */
            n = p0 ? (uint32_t)p0 : 1;
            cursor_x += n;
            if (cursor_x >= cols) cursor_x = cols - 1;
            break;
        case 'D': /* cursor back */
            n = p0 ? (uint32_t)p0 : 1;
            cursor_x = (n > cursor_x) ? 0 : cursor_x - n;
            break;
        case 'G': /* horizontal absolute (1-based) */
            cursor_x = p0 ? (uint32_t)(p0 - 1) : 0;
            if (cursor_x >= cols) cursor_x = cols - 1;
            break;
        case 'H': /* cursor position row;col (1-based) */
        case 'f':
            cursor_y = p0 ? (uint32_t)(p0 - 1) : 0;
            cursor_x = (csi_idx >= 1 && p1) ? (uint32_t)(p1 - 1) : 0;
            if (cursor_y >= rows) cursor_y = rows - 1;
            if (cursor_x >= cols) cursor_x = cols - 1;
            break;
        case 'J': /* erase in display */
            if (p0 == 2) {
                console_clear();
            } else {
                clear_line_part(cursor_y, cursor_x, cols);
                for (uint32_t y = cursor_y + 1; y < rows; y++) {
                    clear_line_part(y, 0, cols);
                }
            }
            break;
        case 'K': /* erase in line */
            if (p0 == 1) {
                clear_line_part(cursor_y, 0, cursor_x + 1);
            } else if (p0 == 2) {
                clear_line_part(cursor_y, 0, cols);
            } else {
                clear_line_part(cursor_y, cursor_x, cols);
            }
            break;
        case 'P': /* delete n characters; shift the tail left (vi/less) */
            row_delete_chars(cursor_y, cursor_x, p0 ? (uint32_t)p0 : 1);
            break;
        case '@': /* insert n blank characters; shift the tail right */
            row_insert_chars(cursor_y, cursor_x, p0 ? (uint32_t)p0 : 1);
            break;
        case 'X': /* erase n characters in place, cursor stays put */
            row_erase_chars(cursor_y, cursor_x, p0 ? (uint32_t)p0 : 1);
            break;
        case 'd': /* vertical position absolute (1-based) */
            cursor_y = p0 ? (uint32_t)(p0 - 1) : 0;
            if (cursor_y >= rows) cursor_y = rows - 1;
            break;
        case 'Z': /* cursor backward tabulation (n stops of 8) */
            n = p0 ? (uint32_t)p0 : 1;
            while (n-- > 0 && cursor_x >= 8) cursor_x -= 8;
            break;
        case 'L': /* insert lines at cursor (within region) */
            n = p0 ? (uint32_t)p0 : 1;
            if (cursor_y >= region_top && cursor_y <= region_end()) {
                while (n-- > 0) {
                    for (uint32_t y = region_end(); y > cursor_y; y--) {
                        copy_text_row(y, y - 1);
                        copy_pixel_row(y, y - 1);
                    }
                    blank_text_row(cursor_y);
                    blank_pixel_row(cursor_y);
                }
            }
            break;
        case 'M': /* delete lines at cursor (within region) */
            n = p0 ? (uint32_t)p0 : 1;
            if (cursor_y >= region_top && cursor_y <= region_end()) {
                while (n-- > 0) {
                    for (uint32_t y = cursor_y; y < region_end(); y++) {
                        copy_text_row(y, y + 1);
                        copy_pixel_row(y, y + 1);
                    }
                    blank_text_row(region_end());
                    blank_pixel_row(region_end());
                }
            }
            break;
        case 'r': /* set scroll region top;bottom (1-based, default full) */
            if (!p0 && csi_idx < 1) {
                region_active = 0;
                region_top = 0;
                region_bottom = 0;
            } else {
                uint32_t t = p0 ? (uint32_t)(p0 - 1) : 0;
                uint32_t b = (csi_idx >= 1 && p1) ? (uint32_t)(p1 - 1) : (rows ? rows - 1 : 0);
                if (t < rows && b < rows && t < b) {
                    region_top = t;
                    region_bottom = b;
                    region_active = 1;
                    cursor_x = 0;
                    cursor_y = 0;
                }
            }
            break;
        case 'm': /* SGR attributes */
            csi_sgr();
            break;
        case 'h': /* mode set: alt screen / cursor show */
            if (csi_private && (p0 == 1049 || (csi_idx >= 1 && p1 == 1049))) {
                altscreen(1);
            } else if (csi_private && p0 == 25) {
                cursor_visible = 1;
            }
            break;
        case 'l': /* mode reset: alt screen / cursor hide */
            if (csi_private && (p0 == 1049 || (csi_idx >= 1 && p1 == 1049))) {
                altscreen(0);
            } else if (csi_private && p0 == 25) {
                cursor_visible = 0;
            }
            break;
        case 's': /* save cursor */
            saved_x = cursor_x;
            saved_y = cursor_y;
            break;
        case 'u': /* restore cursor */
            cursor_x = saved_x;
            cursor_y = saved_y;
            break;
        default:
            break; /* SGR colors, modes, scroll regions: accepted, ignored */
    }
    csi_reset();
}

void console_putc(char c) {
    /* Also output to serial COM1 for logging */
    serial_putc_raw(c);

    /* Hide the cursor first (exact restore); repainted on every exit. */
    cursor_hide();

    /* Escape sequence state machine */
    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            for (int i = 0; i < 8; i++) csi_params[i] = 0;
            csi_idx = 0;
            csi_private = 0;
        } else if (c == '(' || c == ')' || c == '#' || c == '*' || c == '+') {
            esc_charset_intro = c;
            esc_state = 3; /* consume one more byte (charset designator) */
        } else if (c == ']') {
            esc_state = 4; /* OSC sequence: consume until BEL */
        } else if (c == '7') {
            /* DECSC: save cursor (ANSI.SYS style, used by vi/less) */
            saved_x = cursor_x;
            saved_y = cursor_y;
            csi_reset();
        } else if (c == '8') {
            /* DECRC: restore cursor */
            cursor_x = saved_x;
            cursor_y = saved_y;
            csi_reset();
        } else if (c == 'D') {
            /* Index: down one line, scrolling the region at the bottom */
            uint32_t bot = region_end();
            if (cursor_y >= bot) {
                uint32_t rtop = region_active ? region_top : 0;
                if (rtop < bot && bot < rows) {
                    for (uint32_t y = rtop; y < bot; y++) {
                        copy_text_row(y, y + 1);
                        copy_pixel_row(y, y + 1);
                    }
                    blank_text_row(bot);
                    blank_pixel_row(bot);
                }
            } else {
                cursor_y++;
            }
            csi_reset();
        } else if (c == 'E') {
            /* Next line: carriage return plus index */
            cursor_x = 0;
            if (cursor_y < region_end()) cursor_y++;
            csi_reset();
        } else if (c == 'M') {
            /* Reverse index: cursor up, scrolling the region down at top */
            uint32_t top = region_active ? region_top : 0;
            if (cursor_y == top || cursor_y == 0) {
                uint32_t bot = region_end();
                if (bot < rows && top < bot) {
                    for (uint32_t y = bot; y > top; y--) {
                        copy_text_row(y, y - 1);
                        copy_pixel_row(y, y - 1);
                    }
                    blank_text_row(top);
                    blank_pixel_row(top);
                }
            } else {
                cursor_y--;
            }
            csi_reset();
        } else {
            csi_reset(); /* lone ESC + char: ignore */
        }
        cursor_show();
        return;
    }
    if (esc_state == 4) {
        /* OSC: terminated by BEL, or by ESC which starts a new sequence */
        if (c == 0x07) {
            csi_reset();
        } else if (c == 0x1B) {
            esc_state = 1;
        }
        cursor_show();
        return;
    }
    if (esc_state == 3) {
        /* Charset designator terminator. ESC ( 0 selects DEC Special
         * Graphics on G0 (letters draw lines); ESC ( B restores ASCII. */
        if (esc_charset_intro == '(') g_special_graphics = (c == '0');
        csi_reset();
        cursor_show();
        return;
    }
    if (esc_state == 2) {
        if (c >= '0' && c <= '9') {
            csi_params[csi_idx] = csi_params[csi_idx] * 10 + (c - '0');
            cursor_show();
            return;
        }
        if (c == ';') {
            if (csi_idx < 7) csi_idx++;
            cursor_show();
            return;
        }
        if (c == '?' || c == '>' || c == '=' || c == ' ') {
            if (c == '?') csi_private = 1;
            cursor_show();
            return; /* private-mode / padding markers: skip */
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            csi_dispatch(c);
            cursor_show();
            return;
        }
        csi_reset(); /* unknown byte: drop sequence */
        cursor_show();
        return;
    }
    if (c == 0x1B) {
        esc_state = 1;
        cursor_show();
        return;
    }

    if (c == '\r') {
        cursor_x = 0;
        cursor_show();
        return;
    }

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y > region_end()) {
            scroll();
            cursor_y = region_end();
        }
        cursor_show();
        return;
    }

    if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            draw_char(' ', cursor_x, cursor_y);
        }
        cursor_show();
        return;
    }

    /* SO/SI (0x0E/0x0F) select the G1 graphics character set. The linux
     * terminfo drives line drawing through these: smacs=^N, rmacs=^O, and
     * sgr0 ends with ^O — so every attribute reset shifts back to ASCII.
     * Consume them instead of drawing: 0x0F would otherwise land in the font
     * as CP437 ☼ and the screen fills with star glyphs. */
    if (c == 0x0E) { /* SO: shift out - ACS letters become line drawing */
        g_special_graphics = 1;
        cursor_show();
        return;
    }
    if (c == 0x0F) { /* SI: shift in - back to plain ASCII */
        g_special_graphics = 0;
        cursor_show();
        return;
    }

    if (c == '\t') {
        cursor_x = (cursor_x + 4) & ~3;
        if (cursor_x >= cols) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y > region_end()) {
                scroll();
                cursor_y = region_end();
            }
        }
        cursor_show();
        return;
    }

    draw_char(c, cursor_x, cursor_y);
    cursor_x++;
    if (cursor_x >= cols) {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y > region_end()) {
            scroll();
            cursor_y = region_end();
        }
    }
    cursor_show();
}
void console_puts(const char *str) {
    while (*str) {
        console_putc(*str++);
    }
}

/* ─ Cell-level API for terminal emulators (third_party/libvterm glue) ────
 * The escape-sequence interpreter above is fine for simple programs, but a
 * full emulator owns its own screen model and needs direct cell access:
 * it decides the glyph, the pen and the cursor, and asks us to blit. */

uint32_t console_cols(void) { return cols; }
uint32_t console_rows(void) { return rows; }

/* Palette index for an xterm 256-colour / 8-16 colour value. */
uint8_t console_color_index(int n) {
    if (n < 0) return 7;
    if (n < 16) return (uint8_t)n;
    return quantize_256(n);
}

/* Nearest palette entry for a 24-bit colour (RC_* palette is 16 entries). */
uint8_t console_color_rgb(uint32_t r, uint32_t g, uint32_t b) {
    return nearest_palette(r, g, b);
}

/* Blit one cell with an explicit pen. `ch` is a CP437 glyph code.
 *
 * The cell API takes an already-resolved glyph: the emulator did its own
 * charset translation (SO/SI, ESC ( 0, UTF-8) before calling us, so the
 * interpreter's DEC-graphics remap must not run a second time. If it did, a
 * stale `g_special_graphics` would turn a genuine letter into a line-drawing
 * character - which is how stray tiles used to appear inside frames. */
void console_put_cell(uint32_t x, uint32_t y, uint8_t ch,
                      uint8_t fg, uint8_t bg, int reverse) {
    if (x >= cols || y >= rows) return;
    int saved_graphics = g_special_graphics;
    g_special_graphics = 0;

    struct text_cell *cell = &textbuf[y * cols + x];
    cell->ch = ch;
    cell->fg = fg;
    cell->bg = bg;
    cell->rev = reverse ? 1 : 0;
    cell_paint(x, y);

    g_special_graphics = saved_graphics;
}

void console_set_cursor_pos(uint32_t x, uint32_t y) {
    cursor_hide();
    cursor_x = (x < cols) ? x : (cols ? cols - 1 : 0);
    cursor_y = (y < rows) ? y : (rows ? rows - 1 : 0);
    cursor_show();
}

void console_set_cursor_visible(int on) {
    cursor_hide();
    cursor_visible = on ? 1 : 0;
    cursor_show();
}

/* Put the cursor overlay back after a batch of screen updates.
 *
 * A cell-level emulator blits whole cells, and every one of those blits
 * erases the XOR overlay sitting on the cursor's cell. The emulator reports
 * cursor motion while it parses, i.e. *before* the damage it accumulated is
 * flushed, so without this call a full-screen app (vi, whiptail) finishes
 * every update with the cursor painted over and looks like it has none.
 * Safe to call unconditionally - cursor_painted makes it a no-op when the
 * overlay survived. */
void console_cursor_repaint(void) {
    if (cursor_visible && !cursor_painted) cursor_paint();
}

void console_repaint(void) { repaint_all(); }
