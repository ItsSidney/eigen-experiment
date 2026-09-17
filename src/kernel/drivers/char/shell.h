/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * shell.h - Kernel debugging shell interface.
 */

#ifndef KERNEL_SHELL_H
#define KERNEL_SHELL_H

#include <stdint.h>
#include <stddef.h>

/* Maximum number of arguments parsed from the input line. */
#define SHELL_MAX_ARGS  32
#define SHELL_CMD_LEN   512   /* max command-line length           */
#define SHELL_HIST_LEN  32    /* command history slots             */
#define SHELL_MAX_CMDS  32    /* compile-time command table limit  */

/*
 * A single shell command.
 *   name        - command verb (e.g. "ps")
 *   handler     - argc/argv callback; return 0 on success
 *   help        - one-line help string
 *   usage       - optional usage string (may be NULL)
 *
 * Commands are registered via SHELL_CMD() so the table is populated
 * at link time and new commands can be added from any subsystem.
 */
struct shell_cmd {
    const char *name;
    int        (*handler)(int argc, char **argv);
    const char *help;
    const char *usage;
};

/*
 * Registration macro — emits a const struct placed in .shell_commands so the
 * linker can collect them all into a single contiguous table without a shared
 * array that every caller must update.
 *
 * NOTE: the symbol name incorporates __LINE__ so the same handler function
 * (e.g. cmd_help) can be registered under multiple names ("help", "?")
 * without redefinition errors.
 */
#define SHELL_CMD_JOIN2(a, b) a##b
#define SHELL_CMD_JOIN(a, b) SHELL_CMD_JOIN2(a, b)
#define SHELL_CMD(name_, help_, usage_, fn_)                        \
    static const struct shell_cmd SHELL_CMD_JOIN(cmd_##fn_##_line_, __LINE__) \
        __attribute__((used, section(".shell_commands"))) = {       \
        .name   = name_,                                           \
        .help   = help_,                                          \
        .usage  = usage_,                                         \
        .handler = fn_                                            \
    }

/* Public entry point invoked from kernel.c after the user shell exits. */
void shell_run(void);

/* Lookup a command by name; returns handler or NULL. Exposed for scripting. */
const struct shell_cmd *shell_find_cmd(const char *name);

/* Print the list of registered commands (used by `help` and `help <cmd>`). */
void shell_list_cmds(void);

#endif /* KERNEL_SHELL_H */
