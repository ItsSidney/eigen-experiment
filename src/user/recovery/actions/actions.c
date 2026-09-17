/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * actions/actions.c - export / reboot / force-mount.
 */

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>

#include "actions/actions.h"
#include "overlays/overlays.h"
#include "ui/ui.h"

/* ── actions ────────────────────────────────────────────────────────── */

void do_export(struct app *a) {
    char path[64];
    FILE *f;
    int lines = 0;
    snprintf(path, sizeof path, "/tmp/recovery-report-%ss.txt",
             a->data.uptime[0] ? a->data.uptime : "0");
    f = fopen(path, "w");
    if (!f) {
        show_message(a, "export failed: cannot write report");
        return;
    }
    fprintf(f, "eigen diagnostic  build=%s uptime=%ss reason=%s\n\n",
            a->data.build, a->data.uptime, a->reason[0] ? a->reason : "-");
    lines += 2;
    fprintf(f, "=== registers ===\n");
    if (a->data.regs_valid) {
        int i;
        for (i = 0; i < NREGS; i++)
            fprintf(f, "%s %s\n", a->data.regname[i], a->data.regval[i]);
        fprintf(f, "%s\n", a->data.rflags_raw);
        lines += NREGS + 2;
    } else {
        fprintf(f, "no exception recorded yet\n");
        lines += 2;
    }
    fprintf(f, "\n=== exceptions (%d) ===\n", a->data.nexc);
    {
        int i;
        for (i = 0; i < a->data.nexc; i++)
            fprintf(f, "ENTRY %lu #%s @%s t+%lu pid=%lu\n",
                    a->data.excs[i].idx, a->data.excs[i].mnem,
                    a->data.excs[i].rip, a->data.excs[i].sec,
                    a->data.excs[i].pid);
        lines += a->data.nexc + 2;
    }
    fprintf(f, "\n=== mounts ===\n");
    {
        int i;
        for (i = 0; i < a->data.nmnt; i++)
            fprintf(f, "%s %s %s\n", a->data.mnts[i].src,
                    a->data.mnts[i].mp, a->data.mnts[i].fs);
        lines += a->data.nmnt + 2;
    }
    fprintf(f, "\n=== pci ===\n");
    {
        int i;
        for (i = 0; i < a->data.npci; i++)
            fprintf(f, "%s %s %s\n", a->data.pcis[i].bdf,
                    a->data.pcis[i].cls, a->data.pcis[i].desc);
        lines += a->data.npci + 2;
    }
    fclose(f);
    {
        char msg[256];
        snprintf(msg, sizeof msg, "wrote %d lines to:\n%s", lines, path);
        show_message(a, msg);
    }
}

void do_reboot(struct app *a) {
    (void)a;
    sync();
    reboot(RB_AUTOBOOT);
    /* if we are still here, it failed */
    show_message(a, "reboot failed: kernel refused");
}

void do_force_mount(struct app *a, const char *src, const char *mp) {
    char cmd[256];
    FILE *p;
    char out[512];
    size_t n = 0;
    int rc;
    (void)src;
    /* A real mount attempt; output (or the error) is the report. */
    snprintf(cmd, sizeof cmd, "mount %s 2>&1", mp);
    p = popen(cmd, "r");
    if (!p) {
        show_message(a, "mount failed: cannot spawn helper");
        return;
    }
    while (n + 1 < sizeof out) {
        size_t r = fread(out + n, 1, sizeof out - 1 - n, p);
        if (r == 0)
            break;
        n += r;
    }
    out[n] = '\0';
    rc = pclose(p);
    {
        char msg[512];
        snprintf(msg, sizeof msg, "mount %s -> rc %d\n%s", mp, rc, out);
        show_message(a, msg);
    }
}
