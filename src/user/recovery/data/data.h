/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * data.h - /proc snapshot layer for eigen-recovery.
 *
 * Every poll function refreshes one source and bumps gen when anything
 * changed. Widgets bind to these structs plus the generation counter and
 * re-render only on change -- the same handle+generation pattern as the
 * Lattice L2 Stream/Table bindings (kmsg even reuses lat_stream).
 *
 * All parsing is defensive: missing files or stub contents (e.g. the
 * all-zero /proc/stat) degrade to "n/a" rather than fabricated data.
 */

#ifndef EIGEN_RECOVERY_DATA_H
#define EIGEN_RECOVERY_DATA_H

#include <stdint.h>

#include "lattice/lattice.h"

#define KMSG_CAP (48 * 1024)
#define MAX_EXC 16
#define MAX_MNT 16
#define MAX_PCI 32
#define MAX_RISK 16

struct exc_item {
    char mnem[16];
    char rip[20];
    unsigned long sec;
    unsigned long us;
    unsigned long pid;
    unsigned long idx;
};

struct mnt_item {
    char src[64];
    char mp[64];
    char fs[32];
};

struct pci_item {
    char bdf[16];
    char cls[8];
    char desc[96];
};

struct risk_item {
    char dev[64];
    char note[96];
    int bad; /* 0 ok, 1 degraded */
};

/* 12 parsed registers: RIP RSP ERR RAX RBX RCX RDX RSI RDI RBP CR2 CR3 */
#define NREGS 12

struct sysdata {
    uint64_t gen;
    char uptime[16];
    unsigned long upsec;
    char build[48];
    unsigned long mem_total_kb;
    unsigned long mem_free_kb;
    int mem_pct; /* -1 unknown */
    int ncpu;    /* -1 unknown */
    unsigned long irq_total;
    unsigned long irq_rate; /* per second, -1 until two samples */
    unsigned long exc_count;

    struct lat_stream kmsg;
    uint8_t *kmsg_buf;
    uint64_t kmsg_off; /* absolute proc offset already consumed */

    int regs_valid;
    char regs_vec[16];
    char regs_mnem[16];
    unsigned long regs_pid;
    unsigned long regs_ticks;
    char regname[NREGS][6];
    char regval[NREGS][20];
    char rflags_raw[96];
    /* CF PF ZF SF IF DF OF, -1 unknown */
    int flagbit[7];

    struct exc_item excs[MAX_EXC];
    int nexc;
    struct mnt_item mnts[MAX_MNT];
    int nmnt;
    struct pci_item pcis[MAX_PCI];
    int npci;
    struct risk_item risks[MAX_RISK];
    int nrisk;
};

/* Allocate stream/sub storage (heap) and take an initial snapshot. */
int data_init(struct sysdata *d);
/* New kmsg bytes -> stream. Returns 1 when the generation moved. */
int data_poll_kmsg(struct sysdata *d);
/* regs/exceptions/mounts/pci/meminfo/irq/uptime/cpuinfo. */
int data_poll_slow(struct sysdata *d);
void data_free(struct sysdata *d);

/* Severity of a kmsg line: 2 fail/panic, 1 warn, 0 info/other. */
int kmsg_severity(const char *line, size_t len);

#endif /* EIGEN_RECOVERY_DATA_H */
