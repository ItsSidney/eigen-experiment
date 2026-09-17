/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * data.c - /proc snapshot implementation. Userspace (musl OK).
 */

#define _DEFAULT_SOURCE /* pread, strtok_r */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "data/data.h"

/* Read a whole proc file (they are small). Returns bytes, -1 on error. */
static int read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    size_t n = 0;
    ssize_t r;
    if (fd < 0)
        return -1;
    while (n + 1 < cap) {
        r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = '\0';
    return (int)n;
}

static unsigned long parse_kb_after(const char *line) {
    /* "MemTotal:       12345 kB" -> number */
    const char *p = line;
    while (*p && (*p < '0' || *p > '9'))
        p++;
    return strtoul(p, NULL, 10);
}

static int contains_ci(const char *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    size_t i;
    size_t j;
    if (nlen == 0 || hlen < nlen)
        return 0;
    for (i = 0; i + nlen <= hlen; i++) {
        for (j = 0; j < nlen; j++) {
            char a = hay[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (b >= 'A' && b <= 'Z')
                b += 32;
            if (a != b)
                break;
        }
        if (j == nlen)
            return 1;
    }
    return 0;
}

int kmsg_severity(const char *line, size_t len) {
    if (contains_ci(line, len, "panic") || contains_ci(line, len, "fail") ||
        contains_ci(line, len, "error") || contains_ci(line, len, "timeout") ||
        contains_ci(line, len, "unable"))
        return 2;
    if (contains_ci(line, len, "warn") || contains_ci(line, len, "retry") ||
        contains_ci(line, len, "retried"))
        return 1;
    return 0;
}

int data_init(struct sysdata *d) {
    struct lat_sub *subs;
    memset(d, 0, sizeof *d);
    d->mem_pct = -1;
    d->ncpu = -1;
    d->irq_rate = (unsigned long)-1;
    d->kmsg_buf = malloc(KMSG_CAP);
    subs = malloc(sizeof(struct lat_sub) * 4);
    if (!d->kmsg_buf || !subs) {
        free(d->kmsg_buf);
        free(subs);
        return -1;
    }
    lat_stream_init(&d->kmsg, d->kmsg_buf, KMSG_CAP, subs, 4);
    d->kmsg_off = 0;
    data_poll_kmsg(d);
    data_poll_slow(d);
    return 0;
}

void data_free(struct sysdata *d) {
    free(d->kmsg_buf);
    free(d->kmsg.subs);
}

/* ── kmsg tail into the stream ──────────────────────────────────────── */

int data_poll_kmsg(struct sysdata *d) {
    int fd = open("/proc/kmsg", O_RDONLY);
    int moved = 0;
    if (fd < 0)
        return 0;
    for (;;) {
        static uint8_t chunk[4096];
        ssize_t n = pread(fd, chunk, sizeof chunk, (off_t)d->kmsg_off);
        if (n <= 0)
            break;
        lat_stream_push(&d->kmsg, chunk, (size_t)n);
        d->kmsg_off += (uint64_t)n;
        moved = 1;
        if ((size_t)n < sizeof chunk)
            break;
    }
    close(fd);
    if (moved)
        d->gen++;
    return moved;
}

/* ── slow sources ───────────────────────────────────────────────────── */

static void poll_uptime_build(struct sysdata *d, int *changed) {
    char buf[128];
    if (read_file("/proc/uptime", buf, sizeof buf) > 0) {
        char up[16];
        size_t i = 0;
        while (buf[i] && buf[i] != '.' && i < 10)
            i++;
        if (i >= sizeof up - 4)
            i = sizeof up - 5;
        memcpy(up, buf, i);
        up[i] = '\0';
        if (strcmp(up, d->uptime) != 0) {
            strncpy(d->uptime, up, sizeof d->uptime - 1);
            d->upsec = strtoul(up, NULL, 10);
            *changed = 1;
        }
    }
    if (!d->build[0] && read_file("/proc/version", buf, sizeof buf) > 0) {
        /* "Linux version 0.1 Eigen Experimental ..." -> "0.1 Eigen Exp." */
        char *p = strstr(buf, "version ");
        if (p) {
            p += 8;
            snprintf(d->build, sizeof d->build, "%s", p);
            d->build[strcspn(d->build, "\n")] = '\0';
            if (strlen(d->build) > 30)
                d->build[30] = '\0';
            *changed = 1;
        }
    }
}

static void poll_mem(struct sysdata *d, int *changed) {
    char buf[512];
    int pct = d->mem_pct;
    if (read_file("/proc/meminfo", buf, sizeof buf) <= 0)
        return;
    {
        char *line = strtok(buf, "\n");
        while (line) {
            if (strncmp(line, "MemTotal:", 9) == 0)
                d->mem_total_kb = parse_kb_after(line);
            else if (strncmp(line, "MemFree:", 8) == 0)
                d->mem_free_kb = parse_kb_after(line);
            line = strtok(NULL, "\n");
        }
    }
    if (d->mem_total_kb > 0)
        pct = (int)((d->mem_total_kb - d->mem_free_kb) * 100 /
                    d->mem_total_kb);
    if (pct != d->mem_pct) {
        d->mem_pct = pct;
        *changed = 1;
    }
}

static void poll_cpu(struct sysdata *d, int *changed) {
    char buf[1024];
    if (d->ncpu >= 0)
        return;
    if (read_file("/proc/cpuinfo", buf, sizeof buf) <= 0)
        return;
    {
        /* count "processor" lines; fall back to 1 when format differs */
        int n = 0;
        char *p = buf;
        while ((p = strstr(p, "processor")) != NULL) {
            n++;
            p += 9;
        }
        d->ncpu = n > 0 ? n : 1;
        *changed = 1;
    }
}

static void poll_irq(struct sysdata *d, int *changed) {
    char buf[1024];
    static unsigned long prev_total = 0;
    static struct timespec prev_ts = { 0, 0 };
    struct timespec now;
    unsigned long total = 0;
    if (read_file("/proc/interrupts", buf, sizeof buf) <= 0)
        return;
    {
        char *line = strtok(buf, "\n");
        while (line) {
            /* "  N: COUNT ..." */
            char *c = strchr(line, ':');
            if (c)
                total += strtoul(c + 1, NULL, 10);
            line = strtok(NULL, "\n");
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (d->irq_total != total) {
        unsigned long rate = d->irq_rate;
        if (prev_ts.tv_sec != 0) {
            double dt = (double)(now.tv_sec - prev_ts.tv_sec) +
                        (double)(now.tv_nsec - prev_ts.tv_nsec) / 1e9;
            if (dt > 0.2 && total >= prev_total)
                rate = (unsigned long)((double)(total - prev_total) / dt);
        }
        prev_total = total;
        prev_ts = now;
        d->irq_total = total;
        /* Only the displayed rate dirties the screen; the raw total
         * climbs with every timer tick and must not repaint. */
        if (rate != d->irq_rate) {
            d->irq_rate = rate;
            *changed = 1;
        }
    }
}

/* "vector 6 (#UD) pid 13 ticks 2690" header + "NAME <16hex>" rows */
static void poll_regs(struct sysdata *d, int *changed) {
    char buf[1024];
    int had = d->regs_valid;
    if (read_file("/proc/panic_regs", buf, sizeof buf) <= 0)
        return;
    if (strstr(buf, "no exception recorded")) {
        if (had) {
            d->regs_valid = 0;
            *changed = 1;
        }
        return;
    }
    {
        char *line = strtok(buf, "\n");
        int got = 0;
        static const char *names[NREGS] = {
            "RIP", "RSP", "ERR", "RAX", "RBX", "RCX",
            "RDX", "RSI", "RDI", "RBP", "CR2", "CR3"
        };
        int i;
        char newval[NREGS][20];
        memset(newval, 0, sizeof newval);
        if (line && strncmp(line, "vector ", 7) == 0) {
            char *p = line + 7;
            char *sp = strchr(p, ' ');
            snprintf(d->regs_vec, sizeof d->regs_vec, "%.*s",
                     (int)(sp ? sp - p : strlen(p)), p);
            /* mnemonic inside parens */
            {
                char *o = strchr(line, '(');
                char *c = o ? strchr(o, ')') : NULL;
                if (o && c && c - o - 1 < (int)sizeof d->regs_mnem) {
                    memcpy(d->regs_mnem, o + 1, (size_t)(c - o - 1));
                    d->regs_mnem[c - o - 1] = '\0';
                }
            }
            {
                char *pp = strstr(line, "pid ");
                if (pp)
                    d->regs_pid = strtoul(pp + 4, NULL, 10);
                pp = strstr(line, "ticks ");
                if (pp)
                    d->regs_ticks = strtoul(pp + 6, NULL, 10);
            }
            got = 1;
        }
        while ((line = strtok(NULL, "\n")) != NULL) {
            if (line[0] == 'R' && strstr(line, "FLAGS")) {
                strncpy(d->rflags_raw, line, sizeof d->rflags_raw - 1);
                /* tokens like "CF:0" */
                {
                    static const char *bits[7] = {
                        "CF", "PF", "ZF", "SF", "IF", "DF", "OF"
                    };
                    for (i = 0; i < 7; i++) {
                        char *f = strstr(line, bits[i]);
                        if (f && f[strlen(bits[i])] == ':')
                            d->flagbit[i] =
                                f[strlen(bits[i]) + 1] == '1' ? 1 : 0;
                        else
                            d->flagbit[i] = -1;
                    }
                }
                continue;
            }
            /* "NAME <hex>  NAME <hex>" pairs */
            {
                char *tok = line;
                while (*tok) {
                    char name[8] = "";
                    char val[20] = "";
                    int ni = 0;
                    while (*tok == ' ')
                        tok++;
                    while (*tok && *tok != ' ' && ni < 7)
                        name[ni++] = *tok++;
                    name[ni] = '\0';
                    while (*tok == ' ')
                        tok++;
                    ni = 0;
                    while (*tok && *tok != ' ' && ni < 19)
                        val[ni++] = *tok++;
                    val[ni] = '\0';
                    for (i = 0; i < NREGS; i++) {
                        if (strcmp(name, names[i]) == 0) {
                            strncpy(newval[i], val, 19);
                            break;
                        }
                    }
                }
            }
        }
        if (got) {
            static const char *names2[NREGS] = {
                "RIP", "RSP", "ERR", "RAX", "RBX", "RCX",
                "RDX", "RSI", "RDI", "RBP", "CR2", "CR3"
            };
            if (!had || memcmp(newval, d->regval, sizeof newval) != 0) {
                memcpy(d->regval, newval, sizeof newval);
                for (i = 0; i < NREGS; i++)
                    strncpy(d->regname[i], names2[i], 5);
                d->regs_valid = 1;
                *changed = 1;
            } else if (!had) {
                for (i = 0; i < NREGS; i++)
                    strncpy(d->regname[i], names2[i], 5);
                d->regs_valid = 1;
                *changed = 1;
            }
        }
    }
}

static void poll_exceptions(struct sysdata *d, int *changed) {
    char buf[4096];
    unsigned long count = 0;
    if (read_file("/proc/exceptions", buf, sizeof buf) <= 0)
        return;
    if (strstr(buf, "no exceptions recorded")) {
        if (d->exc_count != 0 || d->nexc != 0) {
            d->exc_count = 0;
            d->nexc = 0;
            *changed = 1;
        }
        return;
    }
    {
        /* count ENTRY lines for the total */
        char *p = buf;
        while ((p = strstr(p, "ENTRY ")) != NULL) {
            count++;
            p += 6;
        }
    }
    {
        /* parse newest-first, keep MAX_EXC */
        char *save = NULL;
        char *line = strtok_r(buf, "\n", &save);
        int n = 0;
        struct exc_item fresh[MAX_EXC];
        memset(fresh, 0, sizeof fresh);
        while (line && n < MAX_EXC) {
            if (strncmp(line, "ENTRY ", 6) == 0) {
                /* ENTRY <idx> <mnem> @<rip> t+<sec>.<us> pid=<pid> */
                unsigned long idx = 0, sec = 0, us = 0, pid = 0;
                char mnem[16] = "";
                char rip[20] = "";
                char *q = line + 6;
                idx = strtoul(q, &q, 10);
                while (*q == ' ')
                    q++;
                {
                    int mi = 0;
                    while (*q && *q != ' ' && mi < 15)
                        mnem[mi++] = *q++;
                    mnem[mi] = '\0';
                }
                while (*q == ' ')
                    q++;
                if (*q == '@') {
                    int ri = 0;
                    q++;
                    while (*q && *q != ' ' && ri < 19)
                        rip[ri++] = *q++;
                    rip[ri] = '\0';
                }
                {
                    char *tp = strstr(line, "t+");
                    if (tp)
                        sscanf(tp + 2, "%lu.%lu", &sec, &us);
                    tp = strstr(line, "pid=");
                    if (tp)
                        pid = strtoul(tp + 4, NULL, 10);
                }
                strncpy(fresh[n].mnem, mnem, 15);
                strncpy(fresh[n].rip, rip, 19);
                fresh[n].sec = sec;
                fresh[n].us = us;
                fresh[n].pid = pid;
                fresh[n].idx = idx;
                n++;
            }
            line = strtok_r(NULL, "\n", &save);
        }
        if (count != d->exc_count || n != d->nexc ||
            memcmp(fresh, d->excs, sizeof fresh) != 0) {
            d->exc_count = count;
            d->nexc = n;
            memcpy(d->excs, fresh, sizeof fresh);
            *changed = 1;
        }
    }
}

static void poll_mounts(struct sysdata *d, int *changed) {
    char buf[1024];
    struct mnt_item fresh[MAX_MNT];
    int n = 0;
    if (read_file("/proc/mounts", buf, sizeof buf) <= 0)
        return;
    memset(fresh, 0, sizeof fresh);
    {
        char *save = NULL;
        char *line = strtok_r(buf, "\n", &save);
        while (line && n < MAX_MNT) {
            /* "src mp fstype rw 0 0" */
            char src[64] = "", mp[64] = "", fs[32] = "";
            if (sscanf(line, "%63s %63s %31s", src, mp, fs) == 3) {
                strncpy(fresh[n].src, src, 63);
                strncpy(fresh[n].mp, mp, 63);
                strncpy(fresh[n].fs, fs, 31);
                n++;
            }
            line = strtok_r(NULL, "\n", &save);
        }
    }
    if (n != d->nmnt || memcmp(fresh, d->mnts, sizeof fresh) != 0) {
        d->nmnt = n;
        memcpy(d->mnts, fresh, sizeof fresh);
        *changed = 1;
    }
}

static void poll_pci(struct sysdata *d, int *changed) {
    char buf[2048];
    struct pci_item fresh[MAX_PCI];
    int n = 0;
    if (d->npci > 0)
        return; /* static per boot */
    if (read_file("/proc/bus/pci/devices", buf, sizeof buf) <= 0)
        return;
    memset(fresh, 0, sizeof fresh);
    {
        char *save = NULL;
        char *line = strtok_r(buf, "\n", &save);
        while (line && n < MAX_PCI) {
            /* "BB:DD.F VVVV:DDDD class=CCCC ..." */
            char bdf[16] = "", cls[8] = "";
            if (sscanf(line, "%15s %*s class=%7s", bdf, cls) >= 1) {
                char *desc = strstr(line, "\"");
                strncpy(fresh[n].bdf, bdf, 15);
                strncpy(fresh[n].cls, cls, 7);
                if (desc) {
                    strncpy(fresh[n].desc, desc, 95);
                    fresh[n].desc[95] = '\0';
                }
                n++;
            }
            line = strtok_r(NULL, "\n", &save);
        }
    }
    if (n > 0) {
        d->npci = n;
        memcpy(d->pcis, fresh, sizeof fresh);
        *changed = 1;
    }
}

/* Devices at risk: mounts are ok unless failure keywords in kmsg name
 * storage/vfs, or any exception pairs with a storage-ish RIP context.
 * Honest and simple: keyword scan over the retained kmsg window. */
static void poll_risk(struct sysdata *d, int *changed) {
    struct risk_item fresh[MAX_RISK];
    int n = 0;
    int i;
    size_t kept;
    memset(fresh, 0, sizeof fresh);
    kept = lat_stream_retained(&d->kmsg);
    {
        /* scan last 8KB for failure keywords */
        char win[8192];
        size_t scan = kept > sizeof win ? sizeof win : kept;
        uint64_t base = 0;
        int bad_storage = 0;
        int bad_net = 0;
        if (scan > 0) {
            base = d->kmsg.head - scan;
            if (lat_stream_read(&d->kmsg, base, scan, win) > 0) {
                if (contains_ci(win, scan, "mount fail") ||
                    contains_ci(win, scan, "unable to mount") ||
                    contains_ci(win, scan, "link timeout") ||
                    contains_ci(win, scan, "ahci"))
                    bad_storage = 1;
                if (contains_ci(win, scan, "dhcp") &&
                    (contains_ci(win, scan, "fail") ||
                     contains_ci(win, scan, "timeout")))
                    bad_net = 1;
            }
        }
        for (i = 0; i < d->nmnt && n < MAX_RISK; i++) {
            int bad = 0;
            const char *note = "ok";
            if (bad_storage &&
                (strcmp(d->mnts[i].mp, "/") == 0 ||
                 strstr(d->mnts[i].fs, "ext") != NULL)) {
                bad = 1;
                note = "check kmsg";
            }
            if (bad_net && strstr(d->mnts[i].mp, "net") != NULL) {
                bad = 1;
                note = "dhcp issue?";
            }
            strncpy(fresh[n].dev, d->mnts[i].mp, 63);
            strncpy(fresh[n].note, note, 95);
            fresh[n].bad = bad;
            n++;
        }
        if (d->exc_count > 0 && n < MAX_RISK) {
            snprintf(fresh[n].dev, sizeof fresh[n].dev, "cpu/exceptions");
            snprintf(fresh[n].note, sizeof fresh[n].note, "%lu recorded",
                     d->exc_count);
            fresh[n].bad = 1;
            n++;
        }
    }
    if (n != d->nrisk || memcmp(fresh, d->risks, sizeof fresh) != 0) {
        d->nrisk = n;
        memcpy(d->risks, fresh, sizeof fresh);
        *changed = 1;
    }
}

int data_poll_slow(struct sysdata *d) {
    int changed = 0;
    poll_uptime_build(d, &changed);
    poll_mem(d, &changed);
    poll_cpu(d, &changed);
    poll_irq(d, &changed);
    poll_regs(d, &changed);
    poll_exceptions(d, &changed);
    poll_mounts(d, &changed);
    poll_pci(d, &changed);
    poll_risk(d, &changed);
    if (changed)
        d->gen++;
    return changed;
}
