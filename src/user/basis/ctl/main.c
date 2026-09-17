/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * main.c - basisctl command dispatch.
 */

#include <stdio.h>
#include <string.h>
#include "ctl.h"
#include "basis.h"

static void usage(void) {
    fprintf(stderr,
        "usage: basisctl <command> [args]\n"
        "  status [service]      service overview or single-service detail\n"
        "  deps <service>        dependency tree\n"
        "  enable|disable <svc>  (takes effect next boot)\n"
        "  clear <service>       clear maintenance flag (retries next boot)\n"
        "  apply <file.toml>     validate and install a manifest generation\n"
        "  rollback <svc> [gen]  restore an older generation\n"
        "  validate [dir]        check manifests for errors\n"
        "  boot-until <milestone>\n"
        "  tzset <Zone/City>     set the system timezone\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    if (strcmp(argv[1], "status") == 0) return cmd_status(argc - 2, argv + 2);
    if (strcmp(argv[1], "deps") == 0) {
        if (argc < 3) { usage(); return 1; }
        return cmd_deps(argv[2]);
    }
    if (strcmp(argv[1], "enable") == 0) {
        if (argc < 3) { usage(); return 1; }
        return write_override(argv[2], 1);
    }
    if (strcmp(argv[1], "disable") == 0) {
        if (argc < 3) { usage(); return 1; }
        return write_override(argv[2], 0);
    }
    if (strcmp(argv[1], "clear") == 0) {
        if (argc < 3) { usage(); return 1; }
        return cmd_clear(argv[2]);
    }
    if (strcmp(argv[1], "apply") == 0) {
        if (argc < 3) { usage(); return 1; }
        return cmd_apply(argv[2]);
    }
    if (strcmp(argv[1], "rollback") == 0) {
        if (argc < 3) { usage(); return 1; }
        return cmd_rollback(argv[2], argc >= 4 ? argv[3] : NULL);
    }
    if (strcmp(argv[1], "validate") == 0) {
        int e = validate_dir(BASIS_ETC_DIR, 1);
        e += validate_dir(BASIS_OVERLAY, 0);
        if (e == 0) printf("basisctl: all manifests valid\n");
        return e != 0;
    }
    if (strcmp(argv[1], "boot-until") == 0) {
        if (argc < 3) { usage(); return 1; }
        return cmd_boot_until(argv[2]);
    }
    if (strcmp(argv[1], "tzset") == 0) {
        if (argc < 3) { usage(); return 1; }
        return cmd_tzset(argv[2]);
    }
    usage();
    return 1;
}
