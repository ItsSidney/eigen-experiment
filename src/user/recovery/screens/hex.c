/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * screens/hex.c - /dev/mem viewer + annotate.
 */

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>

#include "screens/screens.h"
#include "ui/ui.h"

/* ── HEX viewer + annotate ──────────────────────────────────────────── */

static int mem_fd = -1;

void draw_hex_body(struct app *a) {
    int w = a->body_w;
    int h = a->body_h;
    int y = 0;
    char head[96];
    int rows;
    int r;
    if (mem_fd < 0)
        mem_fd = open("/dev/mem", O_RDONLY);
    snprintf(head, sizeof head, "MEM @ 0x%08lX%s", a->hex.addr,
             a->hex.annotate ? "  [annotated]" : "");
    ui_text(a, a->lay_body, 0, y++, head, TH_FG, TH_BG, LAT_ATTR_BOLD, w);
    rows = h - 1 - (a->hex.annotate ? 5 : 1);
    if (rows < 1)
        rows = 1;
    for (r = 0; r < rows; r++) {
        uint8_t bytes[16];
        uint64_t off = a->hex.addr + (uint64_t)r * 16;
        char line[96];
        int pos = 0;
        int i;
        ssize_t n = mem_fd >= 0 ? pread(mem_fd, bytes, 16, (off_t)off) : -1;
        pos += snprintf(line + pos, sizeof line - (size_t)pos, "%08lX  ",
                        off & 0xFFFFFFFFUL);
        for (i = 0; i < 16; i++) {
            if (n == 16)
                pos += snprintf(line + pos, sizeof line - (size_t)pos,
                                "%02X ", bytes[i]);
            else
                pos += snprintf(line + pos, sizeof line - (size_t)pos,
                                "?? ");
        }
        pos += snprintf(line + pos, sizeof line - (size_t)pos, " ");
        for (i = 0; i < 16 && n == 16; i++) {
            uint8_t b = bytes[i];
            line[pos++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        }
        line[pos] = '\0';
        ui_text(a, a->lay_body, 0, y++, line, TH_FG, TH_BG, 0, w);
    }
    if (a->hex.annotate) {
        uint8_t magic[4];
        ssize_t n = mem_fd >= 0
                        ? pread(mem_fd, magic, 4, (off_t)a->hex.addr)
                        : -1;
        if (n == 4 && magic[0] == 0x7F && magic[1] == 'E' &&
            magic[2] == 'L' && magic[3] == 'F') {
            uint8_t eh[64];
            if (pread(mem_fd, eh, sizeof eh, (off_t)a->hex.addr) ==
                (ssize_t)sizeof eh) {
                char an[128];
                snprintf(an, sizeof an,
                         "* ELF: magic ok class=%d data=%d ver=%d "
                         "type=%d",
                         eh[4], eh[5], eh[6], eh[16] | (eh[17] << 8));
                ui_text(a, a->lay_body, 0, y++, an, TH_OK, TH_BG, 0, w);
                snprintf(an, sizeof an, "* entry field @+0x18, "
                                         "phoff @+0x20");
                ui_text(a, a->lay_body, 0, y++, an, TH_OK, TH_BG, 0, w);
            }
        } else {
            ui_text(a, a->lay_body, 0, y++, "* no known struct at cursor",
                    TH_FG, TH_BG, LAT_ATTR_DIM, w);
        }
    }
}

int hex_key(struct app *a, struct lat_keyev ev) {
    if (ev.key == KEY_PGDN) {
        a->hex.addr += 256;
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_PGUP) {
        a->hex.addr = a->hex.addr >= 256 ? a->hex.addr - 256 : 0;
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_CHAR && (ev.ch == 'a' || ev.ch == 'A')) {
        a->hex.annotate = !a->hex.annotate;
        a->dirty = 1;
        return 1;
    }
    if (ev.key == KEY_CHAR && (ev.ch == 'g' || ev.ch == 'G')) {
        a->input[0] = '\0';
        a->input_len = 0;
        a->input_cur = 0;
        a->modal = M_GOTO;
        a->dirty = 1;
        return 1;
    }
    return 0;
}

