/* Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * compat/vterm_libc.c - real definitions of the libc surface that
 * third_party/libvterm links against.
 *
 * Built with -fno-builtin (see the Makefile rule) so gcc cannot recognise
 * the copy/zero loops below as memset/memcpy idioms and turn them into
 * calls to themselves.
 */

#include "vterm_libc.h"
#include "mm/kheap.h"
#include "include/serial.h"

struct vterm_compat_FILE { int unused; };

static struct vterm_compat_FILE g_stderr_stub;
FILE *stderr = &g_stderr_stub;

/* ── heap: forwarded to the kernel allocator ───────────────────────────── */

void *malloc(size_t size) {
    return kzalloc(size ? size : 1);
}

void *calloc(size_t n, size_t size) {
    size_t total = n * size;
    void *p = kzalloc(total ? total : 1);
    uint8_t *b = (uint8_t *)p;
    if (!b) return NULL;
    for (size_t i = 0; i < total; i++) b[i] = 0;
    return p;
}

/* No in-place resize in the kernel heap, so grow by copy. libvterm only
 * reallocates its own buffers, always passing the new (larger) size. */
void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (!size) { free(ptr); return NULL; }
    void *q = malloc(size);
    if (!q) return NULL;
    const uint8_t *s = (const uint8_t *)ptr;
    uint8_t *d = (uint8_t *)q;
    for (size_t i = 0; i < size; i++) d[i] = s[i];
    free(ptr);
    return q;
}

void free(void *ptr) {
    kfree(ptr);
}

/* ── memory / strings ──────────────────────────────────────────────────── */

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        s += n;
        d += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n--) {
        if (*a != *b) return (int)(uint8_t)*a - (int)(uint8_t)*b;
        if (!*a) return 0;
        a++; b++;
    }
    return 0;
}

int abs(int v) { return v < 0 ? -v : v; }

/* Standard-conforming: pads up to n bytes with NUL (libvterm relies on the
 * padded buffer when it compares intermed strings with strneq). */
char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

/* ── formatting ──────────────────────────────────────────────────────────
 * libvterm builds every key/sequence emission through snprintf/vsnprintf
 * (vterm_push_output_sprintf[_ctrl]); inert stubs would push ZERO bytes and
 * silently kill every arrow/function key. Real formatter: %c %s %d %i %u
 * %x %X %p %%, '-'/'0' flags, width, h/l/ll/z length modifiers. */

#include <stdarg.h>

struct vfmt_out {
    char  *buf;
    size_t cap;   /* total capacity, 0 = count only */
    size_t len;   /* bytes written (or would-be, capped) */
};

static void vfmt_putc(struct vfmt_out *o, char c) {
    if (o->buf && o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void vfmt_puts(struct vfmt_out *o, const char *s) {
    if (!s) s = "(null)";
    while (*s) vfmt_putc(o, *s++);
}

static void vfmt_putnum(struct vfmt_out *o, unsigned long long u, unsigned base,
                        int upper) {
    char tmp[24];
    int n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[n++] = dig[u % base];
        u /= base;
    } while (u);
    while (n--) vfmt_putc(o, tmp[n]);
}

static int vfmt_dispatch(struct vfmt_out *o, const char **fmtp, va_list ap) {
    const char *f = *fmtp;
    int lcount = 0, hcount = 0, z = 0;

    f++; /* skip '%' */
    for (;; f++) {
        if (*f == '0' || *f == '-') continue; /* flags parsed but unused */
        break;
    }
    while (*f >= '0' && *f <= '9') { f++; } /* width parsed but unused */
    for (;; f++) {
        if (*f == 'l') { lcount++; continue; }
        if (*f == 'h') { hcount++; continue; }
        if (*f == 'z') { z = 1; continue; }
        break;
    }

    switch (*f) {
        case 'c': {
            char c = (char)va_arg(ap, int);
            vfmt_putc(o, c);
            break;
        }
        case 's':
            vfmt_puts(o, va_arg(ap, const char *));
            break;
        case 'd': case 'i': {
            long long in;
            if (lcount >= 2)      in = va_arg(ap, long long);
            else if (lcount == 1) in = va_arg(ap, long);
            else if (hcount == 1) in = (short)va_arg(ap, int);
            else if (hcount >= 2) in = (signed char)va_arg(ap, int);
            else                  in = va_arg(ap, int);
            unsigned long long u;
            if (in < 0) { vfmt_putc(o, '-'); u = (unsigned long long)(-(in + 1)) + 1; }
            else u = (unsigned long long)in;
            vfmt_putnum(o, u, 10, 0);
            break;
        }
        case 'u': case 'x': case 'X': {
            unsigned base = (*f == 'u') ? 10u : 16u;
            unsigned long long uv;
            if (lcount >= 2 || z)      uv = va_arg(ap, unsigned long long);
            else if (lcount == 1)      uv = va_arg(ap, unsigned long);
            else if (hcount == 1)      uv = (unsigned short)va_arg(ap, unsigned int);
            else if (hcount >= 2)      uv = (unsigned char)va_arg(ap, unsigned int);
            else                       uv = va_arg(ap, unsigned int);
            vfmt_putnum(o, uv, base, *f == 'X');
            break;
        }
        case 'p': {
            void *pv = va_arg(ap, void *);
            vfmt_puts(o, "0x");
            vfmt_putnum(o, (unsigned long long)(unsigned long)pv, 16, 0);
            break;
        }
        case '%':
            vfmt_putc(o, '%');
            break;
        case '\0':
            *fmtp = f;
            return 0;
        default:
            vfmt_putc(o, '%');
            vfmt_putc(o, *f);
            break;
    }
    *fmtp = f;
    return 0;
}

static int kvsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    struct vfmt_out o = { buf, n, 0 };
    const char *f = fmt;
    while (*f) {
        if (*f != '%') { vfmt_putc(&o, *f++); continue; }
        vfmt_dispatch(&o, &f, ap);
        f++;
    }
    if (buf && n) {
        size_t end = o.len < n - 1 ? o.len : n - 1;
        buf[end] = '\0';
    }
    return (int)o.len;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    return kvsnprintf(buf, n, fmt, ap);
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = kvsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...) { (void)fmt; return 0; }
int fprintf(FILE *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }

void exit(int status) {
    serial_puts("[VTERM] libvterm called exit(");
    serial_puthex((uint64_t)status);
    serial_puts("); halting\n");
    for (;;) __asm__ volatile("cli; hlt");
}

void abort(void) {
    serial_puts("[VTERM] libvterm called abort(); halting\n");
    for (;;) __asm__ volatile("cli; hlt");
}