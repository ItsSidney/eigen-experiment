/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * test_main.c - host proof harness for Lattice L0-L2. NOT part of the
 * kernel build (uses stdio/assert freely). Compiled by `make lattice-test`
 * with the system gcc against the freestanding core sources.
 *
 * Proves: arena bump semantics, Signal/Stream/Table pub/sub, surface
 * dirty-rect diffing, compositor z-order + transparency + dim-behind
 * modals, and the kmsg binding model (Stream head as generation counter
 * driving a log pane that re-renders only on change).
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lattice.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name)                                                  \
    do {                                                                   \
        if (cond) {                                                        \
            g_pass++;                                                      \
            printf("PASS %s\n", name);                                     \
        } else {                                                           \
            g_fail++;                                                      \
            printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__);          \
        }                                                                  \
    } while (0)

/* ── counting backend: records presents, no real output ─────────────── */

struct count_be {
    struct lat_backend be;
    unsigned presents;
    unsigned flushes;
};

static void count_present(void *ctx, uint32_t x, uint32_t y,
                          const struct lat_cell *cell) {
    struct count_be *b = (struct count_be *)ctx;
    (void)x;
    (void)y;
    (void)cell;
    b->presents++;
}

static void count_flush(void *ctx) {
    ((struct count_be *)ctx)->flushes++;
}

static void count_be_init(struct count_be *b) {
    b->presents = 0;
    b->flushes = 0;
    b->be.ctx = b;
    b->be.present = count_present;
    b->be.flush = count_flush;
    b->be.cursor = 0;
}

/* ── notify counters ────────────────────────────────────────────────── */

struct notify_count {
    int calls;
    uint64_t last_version;
};

static void on_notify(void *ctx, uint64_t version) {
    struct notify_count *n = (struct notify_count *)ctx;
    n->calls++;
    n->last_version = version;
}

/* ── ANSI emitter: the userspace-terminal backend proof ───────────────
 * Minimal version of the future L3 terminal glue: walks the staged grid
 * and emits cursor-addressed SGR text. Enough to show the same surface
 * drives a real terminal, not just the null backend. */

struct ansi_buf {
    char *p;
    size_t cap;
    size_t len;
};

static void ansi_putc(void *ctx, char c) {
    struct ansi_buf *b = (struct ansi_buf *)ctx;
    if (b->len + 1 < b->cap) {
        b->p[b->len++] = c;
        b->p[b->len] = '\0';
    }
}

static void emit_ansi(const struct lat_surface *s, struct ansi_buf *out) {
    static const char *fgcode[8] = { "30", "31", "32", "33",
                                     "34", "35", "36", "37" };
    char tmp[64];
    uint32_t y;
    uint32_t x;
    int cur_fg = -1;

    out->len = 0;
    if (out->cap)
        out->p[0] = '\0';
    for (y = 0; y < s->rows; y++) {
        int n = snprintf(tmp, sizeof tmp, "\033[%u;1H", y + 1);
        for (int i = 0; i < n; i++)
            ansi_putc(out, tmp[i]);
        for (x = 0; x < s->cols; x++) {
            const struct lat_cell *c = &s->back[y * s->cols + x];
            uint8_t ch = c->ch ? c->ch : ' ';
            int fg = c->fg & 7;
            if (fg != cur_fg) {
                int n2 = snprintf(tmp, sizeof tmp, "\033[%sm", fgcode[fg]);
                for (int i = 0; i < n2; i++)
                    ansi_putc(out, tmp[i]);
                cur_fg = fg;
            }
            ansi_putc(out, (char)ch);
        }
    }
    {
        const char *rst = "\033[0m";
        for (int i = 0; rst[i]; i++)
            ansi_putc(out, rst[i]);
    }
}

/* ── tests ──────────────────────────────────────────────────────────── */

static void test_arena(void) {
    uint8_t mem[256];
    struct lat_arena a;
    void *p1;
    void *p2;
    void *p3;

    lat_arena_init(&a, mem, sizeof mem);
    p1 = lat_arena_alloc(&a, 16, 8);
    CHECK(p1 != 0, "arena/alloc-ok");
    CHECK(((uintptr_t)p1 & 7) == 0, "arena/align-8");
    p2 = lat_arena_alloc(&a, 32, 8);
    CHECK(p2 != 0 && p2 != p1, "arena/second-alloc");
    CHECK(lat_arena_used(&a) >= 48, "arena/used-grows");
    p3 = lat_arena_alloc(&a, 1024, 1);
    CHECK(p3 == 0, "arena/oom-null");
    lat_arena_reset(&a);
    CHECK(lat_arena_used(&a) == 0, "arena/reset");
    CHECK(lat_arena_free(&a) == sizeof mem, "arena/free-full");
    p1 = lat_arena_alloc(&a, sizeof mem + 1, 1);
    CHECK(p1 == 0, "arena/oversize-null");
}

static void test_signal(void) {
    uint8_t buf[32];
    struct lat_sub subs[4];
    struct lat_signal s;
    struct notify_count n = { 0, 0 };
    uint8_t out[32];
    size_t len = 0;
    const char *msg = "82%";
    int rc;

    lat_signal_init(&s, buf, sizeof buf, subs, 4);
    rc = lat_signal_subscribe(&s, on_notify, &n);
    CHECK(rc == 0, "signal/subscribe");
    rc = lat_signal_subscribe(&s, on_notify, &n);
    CHECK(rc == 0 && s.nsubs == 1, "signal/resubscribe-idempotent");
    CHECK(lat_signal_set(&s, msg, 3) == 0, "signal/set");
    CHECK(s.version == 1, "signal/version-bump");
    CHECK(n.calls == 1 && n.last_version == 1, "signal/notify");
    CHECK(lat_signal_get(&s, out, sizeof out, &len) == 0 && len == 3 &&
              memcmp(out, msg, 3) == 0,
          "signal/get-roundtrip");
    CHECK(lat_signal_set(&s, msg, sizeof buf + 1) == -1, "signal/overflow-fails");
    CHECK(s.version == 1 && n.calls == 1, "signal/overflow-no-side-effect");
}

static void test_signal_subs_full(void) {
    uint8_t buf[8];
    struct lat_sub subs[1];
    struct lat_signal s;
    struct notify_count a = { 0, 0 };
    struct notify_count b = { 0, 0 };

    lat_signal_init(&s, buf, sizeof buf, subs, 1);
    CHECK(lat_signal_subscribe(&s, on_notify, &a) == 0, "signal/subs-first");
    CHECK(lat_signal_subscribe(&s, on_notify, &b) == -1, "signal/subs-full");
}

static void test_stream_kmsg_model(void) {
    /* Mirrors the kernel klog: head is the generation counter, readers
     * hold absolute offsets and clamp like klog_read(). */
    uint8_t ring[64];
    struct lat_sub subs[4];
    struct lat_stream st;
    struct notify_count n = { 0, 0 };
    char out[128];
    int64_t r;

    lat_stream_init(&st, ring, sizeof ring, subs, 4);
    CHECK(lat_stream_subscribe(&st, on_notify, &n) == 0, "stream/subscribe");
    CHECK(lat_stream_push(&st, "boot ok\n", 8) == 0, "stream/push");
    CHECK(st.head == 8 && st.version == 8, "stream/head-is-generation");
    CHECK(n.calls == 1 && n.last_version == 8, "stream/notify-head");
    r = lat_stream_read(&st, 0, sizeof out - 1, out);
    CHECK(r == 8, "stream/read-all");
    out[r] = '\0';
    CHECK(strcmp(out, "boot ok\n") == 0, "stream/read-content");
    /* Clamp: stale offset snaps to base, past-head reads 0. */
    r = lat_stream_read(&st, 0, 4, out);
    CHECK(r == 4, "stream/read-partial");
    CHECK(lat_stream_read(&st, 1000, 8, out) == 0, "stream/read-past-head-zero");
    /* Overwrite path: push more than cap, tail survives, head counts all. */
    {
        char big[100];
        memset(big, 'A', sizeof big);
        CHECK(lat_stream_push(&st, big, sizeof big) == 0, "stream/push-overflow");
        CHECK(st.head == 108, "stream/head-counts-all-bytes");
        CHECK(lat_stream_retained(&st) == sizeof ring, "stream/retained-capped");
        r = lat_stream_read(&st, lat_stream_base(&st), sizeof out - 1, out);
        CHECK(r == 64, "stream/read-after-wrap");
    }
}

static void test_table(void) {
    struct lat_row rows[8];
    struct lat_sub subs[4];
    struct lat_table t;
    struct notify_count n = { 0, 0 };
    struct lat_row *row;

    lat_table_init(&t, rows, 8, subs, 4);
    CHECK(lat_table_subscribe(&t, on_notify, &n) == 0, "table/subscribe");
    CHECK(lat_table_upsert(&t, "network", "online", 6) == 0, "table/upsert");
    CHECK(t.count == 1 && t.version == 1, "table/count-version");
    CHECK(n.calls == 1, "table/notify");
    row = lat_table_find(&t, "network");
    CHECK(row && row->vlen == 6 && memcmp(row->val, "online", 6) == 0,
          "table/find");
    CHECK(lat_table_upsert(&t, "network", "degraded", 8) == 0, "table/replace");
    CHECK(t.count == 1 && t.version == 2, "table/replace-no-growth");
    row = lat_table_find(&t, "network");
    CHECK(row && row->version == 2, "table/row-version");
    CHECK(lat_table_upsert(&t, "logging", "online", 6) == 0, "table/second-row");
    CHECK(t.count == 2, "table/two-rows");
    CHECK(lat_table_remove(&t, "network") == 0, "table/remove");
    CHECK(t.count == 1 && lat_table_find(&t, "network") == 0, "table/gone");
    CHECK(lat_table_remove(&t, "network") == -1, "table/remove-missing");
    {
        char big[LAT_VAL_LEN + 1];
        memset(big, 'x', sizeof big);
        CHECK(lat_table_upsert(&t, "big", big, sizeof big) == -1,
              "table/value-overflow-fails");
    }
}

static void test_surface_diff(void) {
    struct lat_cell front[20 * 5];
    struct lat_cell back[20 * 5];
    struct lat_surface s;
    struct count_be be;
    uint32_t changed;

    count_be_init(&be);
    lat_surface_init(&s, 20, 5, front, back, &be.be);
    changed = lat_surface_flush(&s);
    CHECK(changed == 0, "surface/clean-flush-zero");
    CHECK(be.flushes == 1 && be.presents == 0, "surface/flush-called");
    lat_surface_text(&s, 0, 0, "hello", 7, 0, 0, 20);
    changed = lat_surface_flush(&s);
    CHECK(changed == 5, "surface/diff-counts-changes");
    CHECK(be.presents == 5, "surface/backend-got-diffs");
    changed = lat_surface_flush(&s);
    CHECK(changed == 0, "surface/no-change-no-traffic");
    lat_surface_put(&s, 19, 4, 'X', 1, 0, 0);
    CHECK(lat_surface_flush(&s) == 1, "surface/single-cell");
    lat_surface_put(&s, 99, 99, 'Y', 7, 0, 0);
    CHECK(lat_surface_flush(&s) == 0, "surface/oob-ignored");
    /* Transparent staging cells never reach the backend. */
    back[2 * 20 + 2].ch = 0;
    CHECK(lat_surface_flush(&s) == 0, "surface/transparent-skipped");
}

static void test_surface_dump(void) {
    struct lat_cell front[10 * 3];
    struct lat_cell back[10 * 3];
    struct lat_surface s;
    struct count_be be;
    char out[128];
    struct ansi_buf ab;

    count_be_init(&be);
    lat_surface_init(&s, 10, 3, front, back, &be.be);
    lat_surface_text(&s, 0, 0, "hi", 7, 0, 0, 10);
    lat_surface_text(&s, 0, 2, "bye", 7, 0, 0, 10);
    ab.p = out;
    ab.cap = sizeof out;
    ab.len = 0;
    lat_surface_dump_text(&s, ansi_putc, &ab);
    CHECK(strcmp(out, "hi\n\nbye\n") == 0, "surface/dump-text-trims");
}

static void test_compositor_modal(void) {
    /* The signature L5 scenario at L1 level: a dashboard layer keeps
     * updating while a modal floats above it with dim-behind. */
    struct lat_cell front[30 * 10];
    struct lat_cell back[30 * 10];
    struct lat_cell base_cells[30 * 10];
    struct lat_cell modal_cells[16 * 5];
    struct lat_surface s;
    struct lat_comp c;
    struct count_be be;
    int base;
    int modal;

    count_be_init(&be);
    lat_surface_init(&s, 30, 10, front, back, &be.be);
    lat_comp_init(&c, &s);
    base = lat_layer_create(&c, 0, 0, 30, 10, base_cells, 0);
    modal = lat_layer_create(&c, 7, 2, 16, 5, modal_cells, 10);
    CHECK(base >= 0 && modal >= 0, "comp/create-layers");
    lat_layer_fill(&c, base, 0, 0, 30, 10, '.', 2, 0, 0);
    lat_layer_text(&c, base, 1, 1, "svc: online", 7, 0, 0, 30);
    lat_layer_set_visible(&c, modal, 0);
    CHECK(lat_comp_present(&c) > 0, "comp/first-present");
    CHECK(c.frames == 1, "comp/frames");
    CHECK(lat_comp_present(&c) == 0, "comp/steady-zero");
    /* Dashboard updates behind a hidden modal: all 11 rewritten cells
     * differ (6 new glyphs + 5 shared-prefix cells with a new fg). */
    lat_layer_text(&c, base, 1, 1, "svc: FAILED", 1, 0, 0, 30);
    CHECK(lat_comp_present(&c) == 11, "comp/live-update-delta");
    /* Push the modal: border + label only, interior left transparent so
     * the live dashboard shows through dimmed behind it. */
    lat_layer_set_visible(&c, modal, 1);
    lat_layer_set_dim_behind(&c, modal, 1);
    lat_layer_fill(&c, modal, 0, 0, 16, 1, '-', 0, 7, 0);
    lat_layer_fill(&c, modal, 0, 4, 16, 1, '-', 0, 7, 0);
    for (int k = 1; k < 4; k++) {
        lat_layer_put(&c, modal, 0, k, '|', 0, 7, 0);
        lat_layer_put(&c, modal, 15, k, '|', 0, 7, 0);
    }
    lat_layer_text(&c, modal, 4, 2, "RESTART?", 0, 7, 0, 12);
    CHECK(lat_comp_present(&c) > 0, "comp/modal-present");
    {
        /* A dashboard cell showing through the transparent modal
         * interior (global 8,3 = local 1,1) is dimmed. */
        const struct lat_cell *shown = &back[3 * 30 + 8];
        CHECK(shown->ch == '.' && (shown->attr & LAT_ATTR_DIM) != 0,
              "comp/dim-behind");
        /* The modal's own opaque cell (global 11,4 = "RESTART?" R)
         * stays crisp. */
        const struct lat_cell *m = &back[4 * 30 + 11];
        CHECK(m->ch == 'R' && (m->attr & LAT_ATTR_DIM) == 0, "comp/modal-crisp");
        /* Outside the modal, no dim. */
        CHECK((back[1 * 30 + 1].attr & LAT_ATTR_DIM) == 0, "comp/outside-clean");
    }
    /* Modal torn down: dashboard underneath intact, one more delta. */
    lat_layer_set_visible(&c, modal, 0);
    CHECK(lat_comp_present(&c) > 0, "comp/modal-teardown");
    CHECK(back[1 * 30 + 1].ch == 's', "comp/dashboard-survives-modal");
}

static void test_kmsg_binding(void) {
    /* L2 proof for the recovery console: the log pane holds a Stream
     * handle + reader offset and re-renders only when the generation
     * counter (head) moved. */
    uint8_t ring[256];
    struct lat_sub subs[4];
    struct lat_stream kmsg;
    struct lat_cell front[40 * 6];
    struct lat_cell back[40 * 6];
    struct lat_surface s;
    struct lat_comp c;
    struct lat_cell logcells[40 * 6];
    struct count_be be;
    int log;
    uint64_t seen = 0;
    int renders = 0;

    count_be_init(&be);
    lat_stream_init(&kmsg, ring, sizeof ring, subs, 4);
    lat_surface_init(&s, 40, 6, front, back, &be.be);
    lat_comp_init(&c, &s);
    log = lat_layer_create(&c, 0, 0, 40, 6, logcells, 0);

    lat_stream_push(&kmsg, "[ 0.000] boot\n", 13);
    lat_stream_push(&kmsg, "[ 1.000] net up\n", 15);
    /* Poll 1: generation moved -> render tail. */
    if (kmsg.version != seen) {
        char tail[240];
        int64_t n = lat_stream_read(&kmsg, lat_stream_base(&kmsg),
                                    sizeof tail - 1, tail);
        tail[n] = '\0';
        lat_layer_clear(&c, log);
        lat_layer_text(&c, log, 0, 0, tail, 7, 0, 0, 40);
        lat_comp_present(&c);
        seen = kmsg.version;
        renders++;
    }
    CHECK(renders == 1, "kmsg/first-render");
    /* Poll 2: no new data -> no render, no backend traffic. */
    {
        uint32_t changed = 0;
        if (kmsg.version != seen) {
            renders++;
            seen = kmsg.version;
        } else {
            changed = lat_comp_present(&c);
        }
        CHECK(renders == 1 && changed == 0, "kmsg/no-pollution-when-idle");
    }
    /* Poll 3: new line -> exactly one more render. */
    lat_stream_push(&kmsg, "[ 2.000] panic?\n", 15);
    if (kmsg.version != seen) {
        renders++;
        seen = kmsg.version;
        lat_comp_present(&c);
    }
    CHECK(renders == 2 && seen == kmsg.head, "kmsg/rerender-on-generation");
}

static void test_ansi_backend(void) {
    struct lat_cell front[8 * 2];
    struct lat_cell back[8 * 2];
    struct lat_surface s;
    struct count_be be;
    char out[512];
    struct ansi_buf ab;

    count_be_init(&be);
    lat_surface_init(&s, 8, 2, front, back, &be.be);
    lat_surface_text(&s, 0, 0, "OK", 2, 0, 0, 8);
    ab.p = out;
    ab.cap = sizeof out;
    ab.len = 0;
    emit_ansi(&s, &ab);
    CHECK(strstr(out, "OK") != 0, "ansi/emits-content");
    CHECK(strstr(out, "\033[") != 0, "ansi/uses-addressing");
}

int main(void) {
    test_arena();
    test_signal();
    test_signal_subs_full();
    test_stream_kmsg_model();
    test_table();
    test_surface_diff();
    test_surface_dump();
    test_compositor_modal();
    test_kmsg_binding();
    test_ansi_backend();
    printf("RESULT: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
