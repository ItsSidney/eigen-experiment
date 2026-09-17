/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * lattice_reactive.h - L2 reactive data bindings.
 *
 * Widgets bind to live sources, not static strings:
 *   Signal<T>  single value (config item, progress, toggle). set() bumps
 *              version and notifies subscribers.
 *   Stream<T>  append-only byte ring (kmsg log, exception history).
 *              head is the generation counter: a log pane holds its reader
 *              offset and re-renders only when head moved. Layout mirrors
 *              the kernel klog (serial.c) so binding is a direct port.
 *   Table<T>   keyed rows (service states, mounts, PCI devices). upsert()
 *              and remove() bump the table version and the row version.
 *
 * Allocation policy: the core never allocates. Value buffers, ring
 * buffers, row arrays and subscriber slots are caller-provided, so the
 * same code runs heap-backed (malloc/kmalloc the storage) and
 * arena-only (slice a static buffer, no heap at all).
 *
 * Callbacks run synchronously inside set/push/upsert. They must not
 * subscribe/unsubscribe (the sub list is being iterated); mutating
 * values from a callback is allowed.
 *
 * Freestanding: only <stdint.h>/<stddef.h>.
 */

#ifndef LATTICE_REACTIVE_H
#define LATTICE_REACTIVE_H

#include <stdint.h>
#include <stddef.h>

typedef void (*lat_notify_fn)(void *ctx, uint64_t version);

struct lat_sub {
    lat_notify_fn fn;
    void *ctx;
};

/* ── Signal ─────────────────────────────────────────────────────────── */

struct lat_signal {
    uint8_t *buf;
    size_t cap;
    size_t len;
    uint64_t version;
    struct lat_sub *subs;
    int maxsubs;
    int nsubs;
};

void lat_signal_init(struct lat_signal *s, uint8_t *buf, size_t cap,
                     struct lat_sub *subs, int maxsubs);
/* 0 on success, -1 when len exceeds cap (state unchanged). */
int lat_signal_set(struct lat_signal *s, const void *data, size_t len);
/* Copies min(len,cap) bytes out; *len_out receives stored length. */
int lat_signal_get(const struct lat_signal *s, void *out, size_t cap,
                   size_t *len_out);
int lat_signal_subscribe(struct lat_signal *s, lat_notify_fn fn, void *ctx);

/* ── Stream ─────────────────────────────────────────────────────────── */

struct lat_stream {
    uint8_t *buf;
    size_t cap;
    uint64_t head;    /* total bytes ever pushed = generation counter */
    uint64_t version; /* == head */
    struct lat_sub *subs;
    int maxsubs;
    int nsubs;
};

void lat_stream_init(struct lat_stream *s, uint8_t *buf, size_t cap,
                     struct lat_sub *subs, int maxsubs);
/* Appends len bytes (oldest overwritten first). len > cap keeps the
 * tail. Always succeeds unless cap == 0. Bumps version, notifies. */
int lat_stream_push(struct lat_stream *s, const void *data, size_t len);
/* Bytes currently retained: min(head, cap). */
size_t lat_stream_retained(const struct lat_stream *s);
/* Absolute offset of the oldest retained byte: head - retained. */
uint64_t lat_stream_base(const struct lat_stream *s);
/* Copy up to size bytes from absolute offset off. Clamps off to base,
 * returns 0 once off reaches head. Mirrors klog_read(). */
int64_t lat_stream_read(const struct lat_stream *s, uint64_t off,
                        size_t size, void *out);
int lat_stream_subscribe(struct lat_stream *s, lat_notify_fn fn, void *ctx);

/* ── Table ──────────────────────────────────────────────────────────── */

#define LAT_KEY_LEN 48
#define LAT_VAL_LEN 128

struct lat_row {
    int used;
    char key[LAT_KEY_LEN];
    uint8_t val[LAT_VAL_LEN];
    size_t vlen;
    uint64_t version;
};

struct lat_table {
    struct lat_row *rows;
    int maxrows;
    int count;
    uint64_t version;
    struct lat_sub *subs;
    int maxsubs;
    int nsubs;
};

void lat_table_init(struct lat_table *t, struct lat_row *rows, int maxrows,
                    struct lat_sub *subs, int maxsubs);
/* Insert or replace. -1 when vlen exceeds LAT_VAL_LEN or table is full. */
int lat_table_upsert(struct lat_table *t, const char *key,
                     const void *val, size_t vlen);
int lat_table_remove(struct lat_table *t, const char *key);
struct lat_row *lat_table_find(struct lat_table *t, const char *key);
int lat_table_subscribe(struct lat_table *t, lat_notify_fn fn, void *ctx);

#endif /* LATTICE_REACTIVE_H */
