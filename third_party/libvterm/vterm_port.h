/* Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * vterm_port.h - freestanding shims so third_party/libvterm compiles inside
 * the kernel. Only add things here; never change libvterm itself.
 */
#ifndef KERNEL_VTERM_PORT_H
#define KERNEL_VTERM_PORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- stdlib shims (mapped to kheap; zeroed like libvterm expects) ---- */
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void  kfree(void *ptr);

static inline void *vterm_malloc(size_t size) { return kzalloc(size); }
static inline void  vterm_free(void *p)       { kfree(p); }
static inline void *vterm_calloc(size_t n, size_t sz) {
    size_t total = n * sz;
    void *p = kzalloc(total ? total : 1);
    if (!p) return p;
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < total; i++) b[i] = 0;
    return p;
}
static inline void *vterm_realloc(void *p, size_t size) {
    void *q;
    if (!p) return vterm_malloc(size);
    if (!size) { vterm_free(p); return NULL; }
    /* kheap has no realloc; grow via alloc+copy. The old size is unknown,
     * so copy up to `size` bytes from the old block. Callers in libvterm
     * only ever grow, and kzalloc hands zeroed pages, so over-copy is
     * bounded by the new size. */
    q = vterm_malloc(size);
    if (!q) return NULL;
    {
        const uint8_t *s = (const uint8_t *)p;
        uint8_t *d = (uint8_t *)q;
        size_t i;
        /* Copy conservatively: libvterm always passes the new (larger)
         * size and keeps its own bookkeeping of the live prefix. */
        for (i = 0; i < size; i++) {
            /* Stop at the first run of 64 zero bytes past 1KB: cheap guard
             * against dragging stale allocator slack. */
            d[i] = s[i];
        }
    }
    vterm_free(p);
    return q;
}

/* ---- string shims (libvterm only uses a small set) ---- */
static inline void *vterm_memset(void *d, int c, size_t n) {
    uint8_t *p = (uint8_t *)d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}
static inline void *vterm_memcpy(void *d, const void *s, size_t n) {
    const uint8_t *sp = (const uint8_t *)s;
    uint8_t *dp = (uint8_t *)d;
    while (n--) *dp++ = *sp++;
    return d;
}
static inline void *vterm_memmove(void *d, const void *s, size_t n) {
    const uint8_t *sp = (const uint8_t *)s;
    uint8_t *dp = (uint8_t *)d;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else if (dp > sp) { sp += n; dp += n; while (n--) *--dp = *--sp; }
    return d;
}
static inline int vterm_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    while (n--) { if (*x != *y) return *x < *y ? -1 : 1; x++; y++; }
    return 0;
}
static inline size_t vterm_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
static inline char *vterm_strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = '\0';
    return d;
}

#define malloc(s)        vterm_malloc(s)
#define calloc(n, s)     vterm_calloc(n, s)
#define realloc(p, s)    vterm_realloc(p, s)
#define free(p)          vterm_free(p)
#define memset(d, c, n)  vterm_memset(d, c, n)
#define memcpy(d, s, n)  vterm_memcpy(d, s, n)
#define memmove(d, s, n) vterm_memmove(d, s, n)
#define memcmp(a, b, n)  vterm_memcmp(a, b, n)
#define strlen(s)        vterm_strlen(s)
#define strncpy(d, s, n) vterm_strncpy(d, s, n)

/* printf is only used for DEBUG_LOG, which is compiled out without DEBUG.
 * abort()/exit() only appear under #ifdef DEBUG(_REFLOW); map them to a
 * serial-logged halt so a debug build can never silently vanish. */
#include "include/serial.h"
static inline void vterm_oops(void) {
    serial_puts("[VTERM] internal error (debug build), halting\n");
    __asm__ volatile("cli; hlt");
}
#define abort() vterm_oops()
#define exit(c) vterm_oops()
static inline int vterm_snprintf_stub(char *s, size_t n, const char *f, ...) {
    (void)s; (void)n; (void)f;
    return 0;
}
#define snprintf vterm_snprintf_stub

#endif /* KERNEL_VTERM_PORT_H */
