/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * gdt.c - Global Descriptor Table setup.
 *
 * We define 5 segments + a 64-bit TSS descriptor:
 *   0x00  null
 *   0x08  kernel code  (ring 0, 64-bit)
 *   0x10  kernel data  (ring 0)
 *   0x18  user data    (ring 3)
 *   0x20  user code    (ring 3, 64-bit)
 *   0x28  TSS          (128-bit system descriptor)
 *
 * In 64-bit mode the base/limit fields of code and data segments are
 * ignored by the CPU — only the descriptor type and DPL matter.
 * The TSS still needs a real base address for the kernel stack pointer
 * that the CPU loads on privilege-level switches (RSP0).
 */

#include <stdint.h>
#include "gdt.h"
#include "include/serial.h"

/* A standard 64-bit GDT entry is 8 bytes. */
struct gdt_entry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity; /* flags[7:4] | limit_hi[3:0] */
    uint8_t  base_hi;
} __attribute__((packed));

/*
 * The TSS descriptor in 64-bit mode is 16 bytes (two consecutive GDT slots).
 * The high quadword holds the upper 32 bits of the base address.
 */
struct gdt_tss_entry {
    uint16_t len;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  flags1;
    uint8_t  flags2;
    uint8_t  base_hi;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

/* 64-bit Task State Segment. We only need RSP0 (ring-0 stack on interrupt). */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;          /* kernel stack pointer for ring-3 -> ring-0 */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];        /* interrupt stack table (unused for now) */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* The actual GDT: 5 normal entries + 1 double-wide TSS entry (7 total slots). */
static struct gdt_entry gdt[7];
static struct gdt_tss_entry *tss_desc = (struct gdt_tss_entry *)&gdt[5];
static struct tss ktss;
static struct gdtr gdtr;

/* 4K kernel stack for initial interrupt handling. */
static uint8_t kstack[8192] __attribute__((aligned(16)));

static void gdt_set(int i, uint8_t access, uint8_t gran) {
    gdt[i].limit_lo   = 0xFFFF;
    gdt[i].base_lo    = 0;
    gdt[i].base_mid   = 0;
    gdt[i].access     = access;
    gdt[i].granularity = gran | 0x0F;
    gdt[i].base_hi    = 0;
}

/* Load GDT and do a far return to reload CS, then update data segments. */
extern void gdt_load(struct gdtr *gdtr, uint16_t kcode, uint16_t kdata);
extern void tss_load(uint16_t selector);

static void enable_sse(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2); /* Clear EM (Emulation) */
    cr0 |= (1ULL << 1);  /* Set MP (Monitor Coprocessor) */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);  /* Set OSFXSR (Enables FXSAVE/FXRSTOR and SSE) */
    cr4 |= (1ULL << 10); /* Set OSXMMEXCPT (Enables unmasked SSE exceptions) */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

void gdt_init(void) {
    /* null */
    gdt[0] = (struct gdt_entry){0};

    /* 0x08  kernel code: present, ring-0, code, readable, 64-bit (L=1) */
    gdt_set(1, 0x9A, 0xA0);

    /* 0x10  kernel data: present, ring-0, data, writable */
    gdt_set(2, 0x92, 0xC0);

    /* 0x18  user data: present, ring-3, data, writable */
    gdt_set(3, 0xF2, 0xC0);

    /* 0x20  user code: present, ring-3, code, readable, 64-bit */
    gdt_set(4, 0xFA, 0xA0);

    /* 0x28  TSS descriptor (16-byte system descriptor) */
    uint64_t tss_base = (uint64_t)&ktss;
    uint16_t tss_limit = sizeof(ktss) - 1;

    tss_desc->len        = tss_limit;
    tss_desc->base_lo    = tss_base & 0xFFFF;
    tss_desc->base_mid   = (tss_base >> 16) & 0xFF;
    tss_desc->flags1     = 0x89; /* present, type=0x9 (64-bit TSS available) */
    tss_desc->flags2     = 0x00;
    tss_desc->base_hi    = (tss_base >> 24) & 0xFF;
    tss_desc->base_upper = (tss_base >> 32) & 0xFFFFFFFF;
    tss_desc->reserved   = 0;

    /* RSP0: where the CPU switches to on a ring-3 interrupt. */
    ktss.rsp0       = (uint64_t)kstack + sizeof(kstack);
    ktss.iopb_offset = sizeof(ktss);

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)gdt;

    gdt_load(&gdtr, GDT_SEG_KCODE, GDT_SEG_KDATA);
    tss_load(GDT_SEG_TSS);
    enable_sse();
    serial_puts("[GDT] Loaded & SSE enabled (kcode=0x08 kdata=0x10 ucode=0x20 udata=0x18 tss=0x28)\n");
}
