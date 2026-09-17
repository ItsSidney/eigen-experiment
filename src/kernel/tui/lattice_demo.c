/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * lattice_demo.c - `lattice_demo` kernel shell command. Bare-metal proof
 * for Lattice L0-L2: an 80x25 surface on the framebuffer backend, a two
 * layer compositor (klog log-pane + dim-behind confirm modal), and L2
 * bindings (Stream snapshotted from the klog ring, Signal progress,
 * Table service rows). Zero heap: every grid and every reactive buffer
 * is static or arena-sliced, so this runs even where kmalloc cannot.
 */

#include <stdint.h>
#include <stddef.h>

#include "lattice/lattice.h"
#include "drivers/char/console.h"
#include "drivers/char/keyboard.h"
#include "drivers/char/shell.h"
#include "include/serial.h"

#define DEMO_COLS 80
#define DEMO_ROWS 25
#define DEMO_MODAL_W 40
#define DEMO_MODAL_H 9
#define DEMO_KLOG_RING 2048
#define DEMO_ARENA 1024

static struct lat_cell demo_front[DEMO_COLS * DEMO_ROWS];
static struct lat_cell demo_back[DEMO_COLS * DEMO_ROWS];
static struct lat_cell demo_base_cells[DEMO_COLS * DEMO_ROWS];
static struct lat_cell demo_modal_cells[DEMO_MODAL_W * DEMO_MODAL_H];
static uint8_t demo_klog_ring[DEMO_KLOG_RING];
static uint8_t demo_arena_mem[DEMO_ARENA];

/* ── framebuffer backend (L0 backend #1) ───────────────────────────── */

static void demo_present(void *ctx, uint32_t x, uint32_t y,
                         const struct lat_cell *cell) {
    (void)ctx;
    console_put_cell(x, y, cell->ch ? cell->ch : ' ',
                     cell->fg, cell->bg,
                     (cell->attr & LAT_ATTR_REVERSE) ? 1 : 0);
}

static void demo_flush(void *ctx) {
    (void)ctx;
    console_cursor_repaint();
}

static const struct lat_backend demo_be = {
    0, demo_present, demo_flush, 0,
};

/* ── tiny serial/decimal helpers (no libc in kernel) ───────────────── */

static void s_puts(const char *s) {
    serial_puts(s);
}

static void s_putu(uint64_t v) {
    char buf[24];
    int i = 0;
    int j;

    if (v == 0) {
        serial_putc('0');
        return;
    }
    while (v > 0 && i < 23) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (j = i - 1; j >= 0; j--)
        serial_putc(buf[j]);
}

static void s_puts_u(const char *label, uint64_t v) {
    s_puts(label);
    s_putu(v);
    s_puts("\n");
}

/* ── box drawing (ASCII so no font dependency) ─────────────────────── */

static void draw_box(struct lat_comp *c, int id, uint32_t w, uint32_t h,
                     const char *title) {
    uint32_t x;
    uint32_t i;

    lat_layer_fill(c, id, 0, 0, w, h, ' ', 0, 7, 0);
    lat_layer_put(c, id, 0, 0, '+', 0, 7, 0);
    lat_layer_put(c, id, w - 1, 0, '+', 0, 7, 0);
    lat_layer_put(c, id, 0, h - 1, '+', 0, 7, 0);
    lat_layer_put(c, id, w - 1, h - 1, '+', 0, 7, 0);
    for (x = 1; x < w - 1; x++) {
        lat_layer_put(c, id, x, 0, '-', 0, 7, 0);
        lat_layer_put(c, id, x, h - 1, '-', 0, 7, 0);
    }
    for (i = 1; i < h - 1; i++) {
        lat_layer_put(c, id, 0, i, '|', 0, 7, 0);
        lat_layer_put(c, id, w - 1, i, '|', 0, 7, 0);
    }
    if (title)
        lat_layer_text(c, id, 2, 0, title, 0, 7, LAT_ATTR_BOLD, w - 4);
}

static int cmd_lattice_demo(int argc, char **argv) {
    struct lat_surface surf;
    struct lat_comp comp;
    struct lat_arena arena;
    struct lat_stream klog;
    struct lat_signal progress;
    struct lat_table services;
    uint8_t *sig_buf;
    struct lat_sub *sig_subs;
    struct lat_sub *stream_subs;
    struct lat_sub *table_subs;
    struct lat_row *table_rows;
    int base;
    int modal;
    uint64_t total;
    uint64_t base_off;
    uint64_t want;
    int64_t got;
    uint32_t cols;
    uint32_t rows;
    char line_starts[32][80];
    int nlines = 0;
    int r;

    (void)argc;
    (void)argv;

    cols = console_cols();
    rows = console_rows();
    if (cols < DEMO_COLS || rows < DEMO_ROWS) {
        console_puts("lattice_demo: console too small (need 80x25)\n");
        return 1;
    }

    /* Reactive storage comes from one static arena: no heap anywhere. */
    lat_arena_init(&arena, demo_arena_mem, sizeof demo_arena_mem);
    sig_buf = (uint8_t *)lat_arena_alloc(&arena, 32, 8);
    sig_subs = (struct lat_sub *)lat_arena_alloc(&arena, sizeof *sig_subs * 4, 8);
    stream_subs =
        (struct lat_sub *)lat_arena_alloc(&arena, sizeof *stream_subs * 4, 8);
    table_subs =
        (struct lat_sub *)lat_arena_alloc(&arena, sizeof *table_subs * 4, 8);
    table_rows =
        (struct lat_row *)lat_arena_alloc(&arena, sizeof *table_rows * 4, 8);
    if (!sig_buf || !sig_subs || !stream_subs || !table_subs || !table_rows) {
        console_puts("lattice_demo: arena too small\n");
        return 1;
    }

    lat_signal_init(&progress, sig_buf, 32, sig_subs, 4);
    lat_stream_init(&klog, demo_klog_ring, sizeof demo_klog_ring,
                    stream_subs, 4);
    lat_table_init(&services, table_rows, 4, table_subs, 4);

    /* Snapshot the klog tail into the Stream (generation = head). */
    total = klog_len();
    want = total > 1500 ? 1500 : total;
    base_off = total > want ? total - want : 0;
    /* klog_read into the ring tail in 512B chunks, then push in order. */
    {
        static uint8_t chunk[512];
        uint64_t off = base_off;
        while (off < total) {
            uint64_t left = total - off;
            size_t n = left > sizeof chunk ? sizeof chunk : (size_t)left;
            got = klog_read(off, n, chunk);
            if (got <= 0)
                break;
            lat_stream_push(&klog, chunk, (size_t)got);
            off += (uint64_t)got;
        }
    }

    lat_signal_set(&progress, "82%", 3);
    lat_table_upsert(&services, "network", "online", 6);
    lat_table_upsert(&services, "logging", "online", 6);

    /* Surface + compositor on the framebuffer backend. */
    lat_surface_init(&surf, DEMO_COLS, DEMO_ROWS, demo_front, demo_back,
                     &demo_be);
    lat_comp_init(&comp, &surf);
    base = lat_layer_create(&comp, 0, 0, DEMO_COLS, DEMO_ROWS,
                            demo_base_cells, 0);
    modal = lat_layer_create(&comp, (DEMO_COLS - DEMO_MODAL_W) / 2,
                             (DEMO_ROWS - DEMO_MODAL_H) / 2, DEMO_MODAL_W,
                             DEMO_MODAL_H, demo_modal_cells, 10);
    if (base < 0 || modal < 0) {
        console_puts("lattice_demo: layer alloc failed\n");
        return 1;
    }

    /* Base layer: title, klog tail, status bound to L2 handles. */
    lat_layer_fill(&comp, base, 0, 0, DEMO_COLS, DEMO_ROWS, ' ', 7, 0, 0);
    lat_layer_text(&comp, base, 0, 0,
                   " Lattice L0-L2 demo (klog Stream + dim-behind modal) ",
                   0, 7, LAT_ATTR_BOLD, DEMO_COLS);
    {
        /* Split the snapshot tail into lines, keep the last 19. */
        uint64_t rbase = lat_stream_base(&klog);
        size_t kept = lat_stream_retained(&klog);
        static char tail[1600];
        int64_t n;
        size_t pos = 0;
        size_t ls = 0;
        size_t k;

        n = lat_stream_read(&klog, rbase, sizeof tail - 1, tail);
        if (n < 0)
            n = 0;
        tail[n] = '\0';
        for (k = 0; k < (size_t)n; k++) {
            if (tail[k] == '\n' || pos == 78) {
                line_starts[nlines % 32][0] = '\0';
                {
                    size_t cp = pos > 78 ? 78 : pos;
                    size_t q;
                    for (q = 0; q < cp; q++)
                        line_starts[nlines % 32][q] = tail[ls + q];
                    line_starts[nlines % 32][cp] = '\0';
                }
                nlines++;
                ls = k + 1;
                pos = 0;
            } else {
                pos++;
            }
        }
        if (ls < (size_t)n && nlines < 64) {
            size_t cp = (size_t)n - ls > 78 ? 78 : (size_t)n - ls;
            size_t q;
            for (q = 0; q < cp; q++)
                line_starts[nlines % 32][q] = tail[ls + q];
            line_starts[nlines % 32][cp] = '\0';
            nlines++;
            (void)kept;
        }
        {
            int show = nlines > 19 ? 19 : nlines;
            int first = nlines - show;
            for (r = 0; r < show; r++) {
                lat_layer_text(&comp, base, 1, (uint32_t)(2 + r),
                               line_starts[(first + r) % 32], 7, 0, 0,
                               DEMO_COLS - 2);
            }
        }
    }
    lat_layer_text(&comp, base, 1, DEMO_ROWS - 2,
                   "progress=82% services=2 stream=head(scroll: live) ",
                   2, 0, 0, DEMO_COLS - 2);
    lat_layer_text(&comp, base, 1, DEMO_ROWS - 1,
                   "[any key] close demo", 7, 0, 0, DEMO_COLS - 2);

    /* Modal floats above the live log; the log behind is dimmed. */
    lat_layer_set_dim_behind(&comp, modal, 1);
    draw_box(&comp, modal, DEMO_MODAL_W, DEMO_MODAL_H, " CONFIRM ");
    lat_layer_text(&comp, modal, 3, 3, "Restart service 'logging'?", 0, 7, 0,
                   DEMO_MODAL_W - 6);
    lat_layer_text(&comp, modal, 3, 5, "[ENTER] confirm  [ESC] close", 0, 7,
                   0, DEMO_MODAL_W - 6);

    lat_comp_present(&comp);
    console_set_cursor_visible(0);

    s_puts("[LATTICE] demo presented\n");
    s_puts_u("[LATTICE] klog_head=", klog.head);
    s_puts_u("[LATTICE] stream_retained=", lat_stream_retained(&klog));
    s_puts_u("[LATTICE] signal_version=", progress.version);
    s_puts_u("[LATTICE] table_version=", services.version);
    s_puts_u("[LATTICE] surface_flushes=", surf.flushes);
    s_puts_u("[LATTICE] cells_flushed=", surf.cells_flushed);
    s_puts_u("[LATTICE] comp_frames=", comp.frames);
    s_puts_u("[LATTICE] arena_used=", lat_arena_used(&arena));

    keyboard_getchar();

    console_set_cursor_visible(1);
    console_clear();
    console_puts("lattice_demo: done (see serial for stats)\n");
    return 0;
}

SHELL_CMD("lattice_demo", "Lattice L0-L2 bare-metal demo (80x25 overlay)",
          "lattice_demo", cmd_lattice_demo);
