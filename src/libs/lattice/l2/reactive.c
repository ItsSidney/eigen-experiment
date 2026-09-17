/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * lattice_reactive.c - L2 pub/sub core. Alloc-free, no libc.
 */

#include "l2/reactive.h"

static int sub_add(struct lat_sub *subs, int maxsubs, int *nsubs,
                   lat_notify_fn fn, void *ctx) {
    int i;

    if (!subs || !fn)
        return -1;
    for (i = 0; i < *nsubs; i++) {
        if (subs[i].fn == fn && subs[i].ctx == ctx)
            return 0; /* idempotent re-subscribe */
    }
    if (*nsubs >= maxsubs)
        return -1;
    subs[*nsubs].fn = fn;
    subs[*nsubs].ctx = ctx;
    (*nsubs)++;
    return 0;
}

static void sub_fire(struct lat_sub *subs, int nsubs, uint64_t version) {
    int i;

    for (i = 0; i < nsubs; i++) {
        if (subs[i].fn)
            subs[i].fn(subs[i].ctx, version);
    }
}

static size_t byte_copy(uint8_t *dst, const uint8_t *src, size_t n) {
    size_t i;

    for (i = 0; i < n; i++)
        dst[i] = src[i];
    return n;
}

/* ── Signal ─────────────────────────────────────────────────────────── */

void lat_signal_init(struct lat_signal *s, uint8_t *buf, size_t cap,
                     struct lat_sub *subs, int maxsubs) {
    s->buf = buf;
    s->cap = cap;
    s->len = 0;
    s->version = 0;
    s->subs = subs;
    s->maxsubs = maxsubs;
    s->nsubs = 0;
}

int lat_signal_set(struct lat_signal *s, const void *data, size_t len) {
    if (!s || (!data && len))
        return -1;
    if (len > s->cap)
        return -1;
    byte_copy(s->buf, (const uint8_t *)data, len);
    s->len = len;
    s->version++;
    sub_fire(s->subs, s->nsubs, s->version);
    return 0;
}

int lat_signal_get(const struct lat_signal *s, void *out, size_t cap,
                   size_t *len_out) {
    size_t n;

    if (!s)
        return -1;
    if (len_out)
        *len_out = s->len;
    if (!out)
        return 0;
    n = s->len < cap ? s->len : cap;
    byte_copy((uint8_t *)out, s->buf, n);
    return 0;
}

int lat_signal_subscribe(struct lat_signal *s, lat_notify_fn fn, void *ctx) {
    if (!s)
        return -1;
    return sub_add(s->subs, s->maxsubs, &s->nsubs, fn, ctx);
}

/* ── Stream ─────────────────────────────────────────────────────────── */

void lat_stream_init(struct lat_stream *s, uint8_t *buf, size_t cap,
                     struct lat_sub *subs, int maxsubs) {
    s->buf = buf;
    s->cap = cap;
    s->head = 0;
    s->version = 0;
    s->subs = subs;
    s->maxsubs = maxsubs;
    s->nsubs = 0;
}

int lat_stream_push(struct lat_stream *s, const void *data, size_t len) {
    const uint8_t *src = (const uint8_t *)data;
    size_t i;

    if (!s || (!src && len) || s->cap == 0)
        return -1;
    if (len > s->cap) {
        /* Keep only the tail that fits. */
        src += len - s->cap;
        /* head still counts every byte ever pushed. */
        for (i = 0; i < s->cap; i++)
            s->buf[i] = src[i];
        s->head += len;
    } else {
        for (i = 0; i < len; i++)
            s->buf[(s->head + i) % s->cap] = src[i];
        s->head += len;
    }
    s->version = s->head;
    sub_fire(s->subs, s->nsubs, s->version);
    return 0;
}

size_t lat_stream_retained(const struct lat_stream *s) {
    if (!s)
        return 0;
    return s->head < (uint64_t)s->cap ? (size_t)s->head : s->cap;
}

uint64_t lat_stream_base(const struct lat_stream *s) {
    if (!s)
        return 0;
    return s->head - lat_stream_retained(s);
}

int64_t lat_stream_read(const struct lat_stream *s, uint64_t off,
                        size_t size, void *out) {
    uint64_t base;
    uint8_t *dst = (uint8_t *)out;
    size_t i;

    if (!s)
        return -1;
    base = lat_stream_base(s);
    if (off < base)
        off = base;
    if (off >= s->head)
        return 0;
    if (off + size > s->head)
        size = (size_t)(s->head - off);
    if (dst) {
        for (i = 0; i < size; i++)
            dst[i] = s->buf[(off + i) % s->cap];
    }
    return (int64_t)size;
}

int lat_stream_subscribe(struct lat_stream *s, lat_notify_fn fn, void *ctx) {
    if (!s)
        return -1;
    return sub_add(s->subs, s->maxsubs, &s->nsubs, fn, ctx);
}

/* ── Table ──────────────────────────────────────────────────────────── */

static int key_cmp(const char *a, const char *b) {
    size_t i;

    for (i = 0; i < LAT_KEY_LEN; i++) {
        if (a[i] != b[i])
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

static void key_copy(char *dst, const char *src) {
    size_t i;

    for (i = 0; i < LAT_KEY_LEN - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

void lat_table_init(struct lat_table *t, struct lat_row *rows, int maxrows,
                    struct lat_sub *subs, int maxsubs) {
    int i;

    t->rows = rows;
    t->maxrows = maxrows;
    t->count = 0;
    t->version = 0;
    t->subs = subs;
    t->maxsubs = maxsubs;
    t->nsubs = 0;
    for (i = 0; i < maxrows; i++) {
        rows[i].used = 0;
        rows[i].key[0] = '\0';
        rows[i].vlen = 0;
        rows[i].version = 0;
    }
}

int lat_table_upsert(struct lat_table *t, const char *key,
                     const void *val, size_t vlen) {
    int i;
    int free_slot = -1;
    struct lat_row *row;

    if (!t || !key || (!val && vlen))
        return -1;
    if (vlen > LAT_VAL_LEN)
        return -1;
    for (i = 0; i < t->maxrows; i++) {
        if (t->rows[i].used) {
            if (key_cmp(t->rows[i].key, key) == 0) {
                row = &t->rows[i];
                byte_copy(row->val, (const uint8_t *)val, vlen);
                row->vlen = vlen;
                t->version++;
                row->version = t->version;
                sub_fire(t->subs, t->nsubs, t->version);
                return 0;
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0)
        return -1;
    row = &t->rows[free_slot];
    row->used = 1;
    key_copy(row->key, key);
    byte_copy(row->val, (const uint8_t *)val, vlen);
    row->vlen = vlen;
    t->count++;
    t->version++;
    row->version = t->version;
    sub_fire(t->subs, t->nsubs, t->version);
    return 0;
}

int lat_table_remove(struct lat_table *t, const char *key) {
    int i;

    if (!t || !key)
        return -1;
    for (i = 0; i < t->maxrows; i++) {
        if (t->rows[i].used && key_cmp(t->rows[i].key, key) == 0) {
            t->rows[i].used = 0;
            t->rows[i].key[0] = '\0';
            t->rows[i].vlen = 0;
            t->count--;
            t->version++;
            sub_fire(t->subs, t->nsubs, t->version);
            return 0;
        }
    }
    return -1;
}

struct lat_row *lat_table_find(struct lat_table *t, const char *key) {
    int i;

    if (!t || !key)
        return 0;
    for (i = 0; i < t->maxrows; i++) {
        if (t->rows[i].used && key_cmp(t->rows[i].key, key) == 0)
            return &t->rows[i];
    }
    return 0;
}

int lat_table_subscribe(struct lat_table *t, lat_notify_fn fn, void *ctx) {
    if (!t)
        return -1;
    return sub_add(t->subs, t->maxsubs, &t->nsubs, fn, ctx);
}
