/* Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * compat/vterm_libc.h - the tiny libc surface third_party/libvterm needs.
 *
 * This is a *declaration* header: the definitions live in vterm_libc.c and
 * are pulled into the kernel link as real symbols. They have to be real
 * symbols rather than static inlines because several libvterm translation
 * units call memmove/memcpy/snprintf without including the matching system
 * header, so the call site never sees an inline body.
 *
 * The <stdlib.h>/<string.h>/<stdio.h> stubs next to this file just pull
 * these declarations in, so libvterm's own #includes resolve too.
 */
#ifndef KERNEL_COMPAT_VTERM_LIBC_H
#define KERNEL_COMPAT_VTERM_LIBC_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* Opaque FILE: only ever passed to fprintf, which discards it. */
typedef struct vterm_compat_FILE FILE;
extern FILE *stderr;

/* memory / strings */
void  *malloc(size_t size);
void  *calloc(size_t n, size_t size);
void  *realloc(void *ptr, size_t size);
void   free(void *ptr);

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strncmp(const char *a, const char *b, size_t n);
char  *strncpy(char *dst, const char *src, size_t n);
int    abs(int v);

/* formatting: the kernel has no printf engine, and libvterm only reaches
 * for these on its version-mismatch path or in DEBUG builds, so they are
 * inert. */
int    snprintf(char *buf, size_t n, const char *fmt, ...);
int    vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int    printf(const char *fmt, ...);
int    fprintf(FILE *f, const char *fmt, ...);

/* process control (version-mismatch path only) */
void   exit(int status);
void   abort(void);

#endif /* KERNEL_COMPAT_VTERM_LIBC_H */