/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * vdso.c - Minimal vDSO image (ET_DYN, SysV hash, 4 trampolines).
 * Each routine issues its real syscall; musl uses them when it finds
 * AT_SYSINFO_EHDR and falls back to raw syscalls otherwise.
 */

#include <stdint.h>
#include <stddef.h>
#include "vdso.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

#define VDSO_SYMS 4

static const char *vdso_names[VDSO_SYMS] = {
    "__vdso_clock_gettime",
    "__vdso_gettimeofday",
    "__vdso_time",
    "__vdso_getcpu",
};

/* mov eax,imm32; syscall; ret */
static const uint8_t vdso_code[VDSO_SYMS][8] = {
    { 0xB8, 0xE4, 0x00, 0x00, 0x00, 0x0F, 0x05, 0xC3 }, /* 228 clock_gettime */
    { 0xB8, 0x60, 0x00, 0x00, 0x00, 0x0F, 0x05, 0xC3 }, /* 96  gettimeofday */
    { 0xB8, 0xC9, 0x00, 0x00, 0x00, 0x0F, 0x05, 0xC3 }, /* 201 time */
    { 0xB8, 0x35, 0x01, 0x00, 0x00, 0x0F, 0x05, 0xC3 }, /* 309 getcpu */
};

static uint8_t vdso_page[4096];

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t elf_hash(const char *s) {
    uint32_t h = 0;
    while (*s) {
        h = (h << 4) + (uint8_t)*s++;
        uint32_t g = h & 0xF0000000U;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

void vdso_init(void) {
    for (size_t i = 0; i < sizeof(vdso_page); i++) vdso_page[i] = 0;

    uint64_t hash_off = 64 + 2 * 56;             /* 176 */
    uint64_t sym_off  = hash_off + 8 + 4 * 4 + 5 * 4; /* 240 */
    uint64_t str_off  = sym_off + 5 * 24;        /* 360 */

    uint32_t name_off[VDSO_SYMS];
    uint64_t str_pos = 1; /* leading NUL */
    for (int i = 0; i < VDSO_SYMS; i++) {
        name_off[i] = (uint32_t)str_pos;
        const char *s = vdso_names[i];
        while (*s) vdso_page[str_off + str_pos++] = (uint8_t)*s++;
        vdso_page[str_off + str_pos++] = 0;
    }
    uint64_t str_size = str_pos;
    uint64_t text_off = (str_off + str_size + 15) & ~15ULL;
    uint64_t dyn_off  = text_off + VDSO_SYMS * 8;

    /* ELF header */
    uint8_t *e = vdso_page;
    e[0] = 0x7F; e[1] = 'E'; e[2] = 'L'; e[3] = 'F';
    e[4] = 2; e[5] = 1; e[6] = 1;
    put16(e + 16, 3);    /* ET_DYN */
    put16(e + 18, 62);   /* EM_X86_64 */
    put32(e + 20, 1);    /* version */
    put64(e + 24, 0);    /* entry */
    put64(e + 32, 64);   /* phoff */
    put64(e + 56, 64);   /* ehsize */
    put16(e + 54, 56);   /* phentsize */
    put16(e + 56, 2);    /* phnum */

    /* PT_LOAD covering the whole page, R+X */
    uint8_t *p = e + 64;
    put32(p + 0, 1);     /* PT_LOAD */
    put32(p + 4, 5);     /* R|X */
    put64(p + 8, 0);     /* offset */
    put64(p + 16, 0);    /* vaddr (base-relative) */
    put64(p + 32, 4096);
    put64(p + 40, 4096);
    put64(p + 48, 4096);

    /* PT_DYNAMIC */
    p += 56;
    put32(p + 0, 2);     /* PT_DYNAMIC */
    put32(p + 4, 4);     /* R */
    put64(p + 8, dyn_off);
    put64(p + 16, dyn_off);
    put64(p + 32, 6 * 16);
    put64(p + 40, 6 * 16);
    put64(p + 48, 8);

    /* SysV hash: 4 buckets, 5 chains */
    uint8_t *h = vdso_page + hash_off;
    put32(h + 0, 4);
    put32(h + 4, 5);
    uint32_t *bucket = (uint32_t *)(h + 8);
    uint32_t *chain = (uint32_t *)(h + 8 + 16);
    for (int i = 0; i < 4; i++) bucket[i] = 0;
    for (int i = 0; i < 5; i++) chain[i] = 0;
    for (int i = 0; i < VDSO_SYMS; i++) {
        uint32_t b = elf_hash(vdso_names[i]) % 4;
        chain[i + 1] = bucket[b];
        bucket[b] = (uint32_t)(i + 1);
    }

    /* dynsym: null + 4 globals */
    uint8_t *s = vdso_page + sym_off;
    for (int i = 0; i < VDSO_SYMS; i++) {
        uint8_t *sym = s + (i + 1) * 24;
        put32(sym + 0, name_off[i]);
        sym[4] = 0x12; /* GLOBAL + FUNC */
        sym[5] = 0;
        put16(sym + 6, 1); /* shndx */
        put64(sym + 8, text_off + (uint64_t)i * 8);
        put64(sym + 16, 8);
    }

    /* text */
    for (int i = 0; i < VDSO_SYMS; i++) {
        for (int b = 0; b < 8; b++) {
            vdso_page[text_off + (uint64_t)i * 8 + (uint64_t)b] = vdso_code[i][b];
        }
    }

    /* dynamic */
    uint8_t *d = vdso_page + dyn_off;
    put64(d + 0, 4);  put64(d + 8, hash_off);  /* DT_HASH */
    put64(d + 16, 5); put64(d + 24, str_off);  /* DT_STRTAB */
    put64(d + 32, 6); put64(d + 40, sym_off);  /* DT_SYMTAB */
    put64(d + 48, 10); put64(d + 56, str_size);/* DT_STRSZ */
    put64(d + 64, 11); put64(d + 72, 24);      /* DT_SYMENT */
    put64(d + 80, 0); put64(d + 88, 0);        /* DT_NULL */
}

int vdso_map(void *pml4) {
    void *phys = pmm_alloc_page();
    if (!phys) return -12; /* -ENOMEM */
    uint8_t *dst = (uint8_t *)(0xFFFF800000000000ULL + (uint64_t)phys);
    for (size_t i = 0; i < sizeof(vdso_page); i++) dst[i] = vdso_page[i];
    vmm_map_page((pml4_t *)pml4, VDSO_BASE, (uint64_t)phys, PAGE_PRESENT | PAGE_USER);
    return 0;
}
