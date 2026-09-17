/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * acpi.c - ACPI static table parser for modern x86_64 discovery.
 */

#include <stdint.h>
#include <stddef.h>
#include "acpi/acpi.h"
#include "mm/vmm.h"
#include "include/serial.h"

struct rsdp {
    char     sig[8];
    uint8_t  cksum;
    char     oem[6];
    uint8_t  rev;
    uint32_t rsdt;
} __attribute__((packed));

struct rsdp2 {
    struct rsdp v1;
    uint32_t   len;
    uint64_t   xsdt;
    uint8_t    xcksum;
    uint8_t    rsv[3];
} __attribute__((packed));

struct sdt {
    char     sig[4];
    uint32_t len;
    uint8_t  rev;
    uint8_t  cksum;
    char     oem[6];
    char     oemtab[8];
    uint32_t oemrev;
    char     creator[4];
    uint32_t crev;
} __attribute__((packed));

struct gas {
    uint8_t  space;
    uint8_t  width;
    uint8_t  offset;
    uint8_t  access;
    uint64_t addr;
} __attribute__((packed));

struct fadt {
    struct sdt hdr;
    uint32_t fw_ctrl;
    uint32_t dsdt;
    uint8_t  rsv0;
    uint8_t  profile;
    uint16_t sci;
    uint32_t smi;
    uint8_t  acpi_en;
    uint8_t  acpi_dis;
    uint8_t  s4bios;
    uint8_t  pstate;
    uint32_t pm1a_evt;
    uint32_t pm1b_evt;
    uint32_t pm1a_cnt;
    uint32_t pm1b_cnt;
    uint32_t pm2_cnt;
    uint32_t pm_tmr;
    uint32_t gpe0;
    uint32_t gpe1;
    uint8_t  gpe1_base;
    uint8_t  cst;
    uint16_t lvl2_lat;
    uint16_t lvl3_lat;
    uint16_t flush_sz;
    uint16_t flush_stride;
    uint8_t  duty_off;
    uint8_t  duty_w;
    uint8_t  day;
    uint8_t  mon;
    uint8_t  cent;
    uint16_t boot_arch;
    uint8_t  rsv1;
    uint32_t flags;
    struct gas reset_reg;
    uint8_t  reset_val;
} __attribute__((packed));

struct madt {
    struct sdt hdr;
    uint32_t lapic;
    uint32_t flags;
} __attribute__((packed));

struct madt_ent {
    uint8_t type;
    uint8_t len;
} __attribute__((packed));

struct mcfg {
    struct sdt hdr;
    uint64_t rsv;
} __attribute__((packed));

struct mcfg_ent {
    uint64_t base;
    uint16_t seg;
    uint8_t  bus0;
    uint8_t  bus1;
    uint32_t rsv;
} __attribute__((packed));

struct hpet {
    struct sdt hdr;
    uint32_t evt_blk;
    struct gas base;
    uint8_t  num;
    uint16_t min_tick;
    uint8_t  prot;
} __attribute__((packed));

#define MAX_CPUS  64
#define MAX_ISO   16
#define MAX_TABLES 16

static int inited;
static uint64_t hhdm;
static struct sdt *tables[MAX_TABLES];
static int ntab;

static uint64_t lapic_base = 0xFEE00000;
static uint8_t lapic_ids[MAX_CPUS];
static uint32_t ncpu;
static uint64_t ioapic_base;
static uint32_t iso_gsi[MAX_ISO];
static uint16_t iso_flags[MAX_ISO];
static uint8_t iso_valid[MAX_ISO];
static uint64_t hpet_base;
static uint64_t mcfg_base;
static struct gas reset_reg;
static uint8_t reset_val;

static int sig_eq(const char *a, const char *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static uint8_t sum_bytes(const void *p, uint32_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint8_t s = 0;
    for (uint32_t i = 0; i < n; i++)
        s += b[i];
    return s;
}

static void *p2v(uint64_t phys) {
    return (void *)(phys + hhdm);
}

static struct sdt *map_table(uint64_t phys) {
    struct sdt *t = (struct sdt *)p2v(phys);
    if (t->len < sizeof(struct sdt) || t->len > 0x100000)
        return NULL;
    if (sum_bytes(t, t->len) != 0)
        return NULL;
    return t;
}

static void parse_madt(struct sdt *t) {
    struct madt *m = (struct madt *)t;
    lapic_base = m->lapic;
    uint8_t *p = (uint8_t *)t + sizeof(struct madt);
    uint8_t *end = (uint8_t *)t + t->len;
    while (p + 2 <= end) {
        struct madt_ent *e = (struct madt_ent *)p;
        if (e->len < 2 || p + e->len > end)
            break;
        if (e->type == 0 && e->len == 8) {
            uint32_t fl = *(uint32_t *)(p + 4);
            if ((fl & 1) && ncpu < MAX_CPUS)
                lapic_ids[ncpu++] = p[3];
        } else if (e->type == 1 && e->len == 12) {
            if (!ioapic_base)
                ioapic_base = *(uint32_t *)(p + 4);
        } else if (e->type == 2 && e->len == 10) {
            uint8_t irq = p[3];
            if (irq < MAX_ISO) {
                iso_valid[irq] = 1;
                iso_gsi[irq] = *(uint32_t *)(p + 4);
                iso_flags[irq] = *(uint16_t *)(p + 8);
            }
        } else if (e->type == 5 && e->len == 12) {
            lapic_base = *(uint64_t *)(p + 4);
        }
        p += e->len;
    }
    serial_puts("[ACPI] MADT cpus=");
    serial_puthex(ncpu);
    serial_puts(" lapic=");
    serial_puthex(lapic_base);
    serial_puts(" ioapic=");
    serial_puthex(ioapic_base);
    serial_puts("\n");
}

static uint32_t pm1a_cnt_port;
static uint32_t pm1b_cnt_port;

static void parse_fadt(struct sdt *t) {
    if (t->len < sizeof(struct fadt))
        return;
    struct fadt *f = (struct fadt *)t;
    reset_reg = f->reset_reg;
    reset_val = f->reset_val;
    pm1a_cnt_port = f->pm1a_cnt;
    pm1b_cnt_port = f->pm1b_cnt;
    serial_puts("[ACPI] FACP reset=");
    serial_puthex(reset_reg.addr);
    serial_puts(" val=");
    serial_puthex(reset_val);
    serial_puts(" pm1a=");
    serial_puthex(pm1a_cnt_port);
    serial_puts("\n");
}

static void parse_mcfg(struct sdt *t) {
    if (t->len < sizeof(struct mcfg) + sizeof(struct mcfg_ent))
        return;
    struct mcfg_ent *e = (struct mcfg_ent *)((uint8_t *)t + sizeof(struct mcfg));
    mcfg_base = e->base;
    serial_puts("[ACPI] MCFG base=");
    serial_puthex(mcfg_base);
    serial_puts(" seg=");
    serial_puthex(e->seg);
    serial_puts(" buses ");
    serial_puthex(e->bus0);
    serial_puts("-");
    serial_puthex(e->bus1);
    serial_puts("\n");
}

static void parse_hpet(struct sdt *t) {
    if (t->len < sizeof(struct hpet))
        return;
    struct hpet *h = (struct hpet *)t;
    if (h->base.space == 0)
        hpet_base = h->base.addr;
    else
        hpet_base = 0;
    serial_puts("[ACPI] HPET base=");
    serial_puthex(hpet_base);
    serial_puts("\n");
}

static void add_table(struct sdt *t) {
    if (ntab < MAX_TABLES)
        tables[ntab++] = t;
    if (sig_eq(t->sig, "APIC"))
        parse_madt(t);
    else if (sig_eq(t->sig, "FACP"))
        parse_fadt(t);
    else if (sig_eq(t->sig, "MCFG"))
        parse_mcfg(t);
    else if (sig_eq(t->sig, "HPET"))
        parse_hpet(t);
    serial_puts("[ACPI] ");
    for (int i = 0; i < 4; i++)
        serial_putc(t->sig[i]);
    serial_puts(" len=");
    serial_puthex(t->len);
    serial_puts("\n");
}

void acpi_init(void *rsdp_ptr) {
    hhdm = vmm_get_hhdm_offset();
    if (!rsdp_ptr) {
        serial_puts("[ACPI] no RSDP from Limine\n");
        return;
    }
    struct rsdp *r = (struct rsdp *)rsdp_ptr;
    if (r->sig[0] != 'R' || r->sig[7] != ' ') {
        serial_puts("[ACPI] bad RSDP signature\n");
        return;
    }
    if (sum_bytes(r, sizeof(struct rsdp)) != 0) {
        serial_puts("[ACPI] bad RSDP checksum\n");
        return;
    }
    serial_puts("[ACPI] RSDP rev=");
    serial_puthex(r->rev);
    serial_puts("\n");

    inited = 1;
    if (r->rev >= 2) {
        struct rsdp2 *r2 = (struct rsdp2 *)r;
        if (sum_bytes(r2, r2->len) != 0) {
            serial_puts("[ACPI] bad XSDP checksum\n");
            return;
        }
        struct sdt *xsdt = (struct sdt *)p2v(r2->xsdt);
        if (xsdt->len < sizeof(struct sdt) || sum_bytes(xsdt, xsdt->len) != 0)
            return;
        uint64_t *ents = (uint64_t *)((uint8_t *)xsdt + sizeof(struct sdt));
        uint32_t n = (xsdt->len - sizeof(struct sdt)) / 8;
        for (uint32_t i = 0; i < n; i++) {
            struct sdt *t = map_table(ents[i]);
            if (t)
                add_table(t);
        }
    } else {
        struct sdt *rsdt = (struct sdt *)p2v(r->rsdt);
        if (rsdt->len < sizeof(struct sdt) || sum_bytes(rsdt, rsdt->len) != 0)
            return;
        uint32_t *ents = (uint32_t *)((uint8_t *)rsdt + sizeof(struct sdt));
        uint32_t n = (rsdt->len - sizeof(struct sdt)) / 4;
        for (uint32_t i = 0; i < n; i++) {
            struct sdt *t = map_table(ents[i]);
            if (t)
                add_table(t);
        }
    }
    if (!ncpu)
        ncpu = 1;
    serial_puts("[ACPI] done, tables=");
    serial_puthex(ntab);
    serial_puts("\n");
}

int acpi_available(void) {
    return inited && ntab > 0;
}

uint32_t acpi_cpu_count(void) {
    return ncpu ? ncpu : 1;
}

uint8_t acpi_cpu_apic_id(uint32_t idx) {
    if (idx < ncpu)
        return lapic_ids[idx];
    return 0;
}

uint64_t acpi_lapic_base(void) {
    return lapic_base;
}

uint64_t acpi_ioapic_addr(void) {
    return ioapic_base;
}

int acpi_iso_get(uint8_t irq, uint32_t *gsi, uint16_t *flags) {
    if (irq < MAX_ISO && iso_valid[irq]) {
        if (gsi)
            *gsi = iso_gsi[irq];
        if (flags)
            *flags = iso_flags[irq];
        return 0;
    }
    if (gsi)
        *gsi = irq;
    if (flags)
        *flags = 0;
    return -1;
}

uint64_t acpi_hpet_addr(void) {
    return hpet_base;
}

uint64_t acpi_mcfg_base(void) {
    return mcfg_base;
}

const void *acpi_get_table(const char sig[4], uint32_t *len_out) {
    for (int i = 0; i < ntab; i++) {
        if (sig_eq(tables[i]->sig, sig)) {
            if (len_out)
                *len_out = tables[i]->len;
            return tables[i];
        }
    }
    return NULL;
}

int acpi_reset(void) {
    if (!reset_reg.addr || !reset_val)
        return -1;
    if (reset_reg.space == 1 && reset_reg.width == 8) {
        outb((uint16_t)reset_reg.addr, reset_val);
        return 0;
    }
    if (reset_reg.space == 0) {
        volatile uint8_t *p = (volatile uint8_t *)p2v(reset_reg.addr);
        *p = reset_val;
        return 0;
    }
    return -1;
}

int acpi_poweroff(void) {
    serial_puts("[ACPI] Initiating poweroff...\n");
    if (pm1a_cnt_port) {
        outw((uint16_t)pm1a_cnt_port, (0 << 10) | 0x2000);
        outw((uint16_t)pm1a_cnt_port, (5 << 10) | 0x2000);
        outw((uint16_t)pm1a_cnt_port, (7 << 10) | 0x2000);
        if (pm1b_cnt_port) {
            outw((uint16_t)pm1b_cnt_port, (0 << 10) | 0x2000);
            outw((uint16_t)pm1b_cnt_port, (5 << 10) | 0x2000);
        }
    }
    /* Standard hypervisor shutdown I/O ports */
    outw(0x604, 0x2000);   /* QEMU piix4 */
    outw(0x4004, 0x3400);  /* VirtualBox */
    outw(0xB004, 0x2000);  /* Bochs */
    outw(0x600, 0x34);     /* Cloud Hypervisor */
    return -1;
}
