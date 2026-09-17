/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * shell.c - Kernel debugging shell with line editing, history, command
 *           table registry, and debugging/inspection commands.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "drivers/timer/pit.h"
#include "drivers/pci/pci.h"
#include "drivers/net/e1000.h"
#include "net/net.h"
#include "net/icmp.h"
#include "net/dns.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"
#include "arch/x86_64/cpu/idt.h"
#include "fs/vfs/vfs.h"
#include "proc/process.h"
#include "include/serial.h"
#include "include/limine.h"

/* ── minimal libc-equivalents (freestanding kernel) ────────────── */
static int kstrcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}
static size_t kstrlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void kcpy(char *dst, const char *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}
static int kstrncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    return n ? *(const unsigned char *)s1 - *(const unsigned char *)s2 : 0;
}

static void putdec(uint64_t val) {
    char buf[32]; int i = 0;
    if (val == 0) { console_putc('0'); return; }
    while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; }
    while (i > 0) console_putc(buf[--i]);
}
static void puthex(uint64_t val) {
    static const char hex[] = "0123456789ABCDEF";
    console_puts("0x");
    for (int i = 60; i >= 0; i -= 4) console_putc(hex[(val >> i) & 0xF]);
}

/* Build ANSI cursor-left (\033[<n>D) — out needs >= 7 bytes. */
static void ansi_cursor_left(char *out, size_t sz, uint32_t n) {
    if (sz < 7) { out[0] = '\0'; return; }
    char d[16]; int t = 0;
    if (n == 0) d[t++] = '0';
    while (n > 0 && t < 15) { d[t++] = '0' + (n % 10); n /= 10; }
    size_t i = 0; out[i++] = '\033'; out[i++] = '[';
    while (t > 0 && i < sz - 1) out[i++] = d[--t];
    out[i++] = 'D'; out[i] = '\0';
}

/* The Limine module request lives in kernel.c (non-static there); its
 * response tells us which bootloader modules (e.g. initramfs) exist. */
extern volatile struct limine_module_request mod_req;

/* ── command table (linker-collected from .shell_commands section) ───────── */
extern const struct shell_cmd __shell_commands_start;
extern const struct shell_cmd __shell_commands_end;

const struct shell_cmd *shell_find_cmd(const char *name) {
    for (const struct shell_cmd *c = &__shell_commands_start; c < &__shell_commands_end; c++)
        if (kstrcmp(c->name, name) == 0) return c;
    return NULL;
}
void shell_list_cmds(void) {
    for (const struct shell_cmd *c = &__shell_commands_start; c < &__shell_commands_end; c++) {
        console_puts("  "); console_puts(c->name); console_puts(" - "); console_puts(c->help);
        console_puts("\n");
    }
}

static int shell_parse_args(char *line, char *argv[], int max) {
    int argc = 0; char *p = line;
    while (*p && argc < max - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
    }
    argv[argc] = NULL;
    return argc;
}

/* history (circular buffer) */
static char shell_history[SHELL_HIST_LEN][SHELL_CMD_LEN];
static int  hist_count = 0;
static int  hist_view  = -1;
static void hist_add(const char *cmd) {
    if (cmd[0] == '\0') return;
    for (int i = SHELL_HIST_LEN - 1; i > 0; i--)
        for (size_t k = 0; k < SHELL_CMD_LEN; k++) shell_history[i][k] = shell_history[i-1][k];
    size_t k = 0;
    while (cmd[k] && k < SHELL_CMD_LEN - 1) { shell_history[0][k] = cmd[k]; k++; }
    shell_history[0][k] = '\0';
    hist_count = (hist_count < SHELL_HIST_LEN) ? hist_count + 1 : SHELL_HIST_LEN;
    hist_view = -1;
}

static void shell_redraw(const char *prompt, const char *buf, uint32_t len, uint32_t cursor) {
    console_puts("\r\033[2K"); console_puts(prompt); console_puts(buf);
    if (cursor < len) { char s[16]; ansi_cursor_left(s, sizeof s, len - cursor); console_puts(s); }
}

/* tab-completion of registered command names */
static void shell_complete(char *buf, uint32_t *len) {
    char word[SHELL_CMD_LEN]; size_t wl = 0;
    char *wp = buf; while (*wp == ' ') wp++;
    while (wp[wl] && wp[wl] != ' ' && wl < sizeof(word) - 1) { word[wl] = wp[wl]; wl++; }
    word[wl] = '\0';
    if (wl == 0) {
        console_putc('\n');
        for (const struct shell_cmd *c = &__shell_commands_start; c < &__shell_commands_end; c++)
            { console_puts(c->name); console_putc('\n'); } return;
    }
    const struct shell_cmd *match = NULL; size_t common = (size_t)-1; int mc = 0;
    for (const struct shell_cmd *c = &__shell_commands_start; c < &__shell_commands_end; c++) {
        if (kstrncmp(c->name, word, wl) == 0) {
            if (mc == 0) { match = c; common = kstrlen(c->name); }
            else {
                size_t cl = kstrlen(c->name); size_t m = cl < common ? cl : common; size_t i;
                for (i = 0; i < m; i++) if (match->name[i] != c->name[i]) break;
                common = i;
            }
            mc++;
        }
    }
    if (mc == 0 || (common <= wl && mc > 1)) {
        if (common <= wl && mc > 1) {
            console_putc('\n');
            for (const struct shell_cmd *c = &__shell_commands_start; c < &__shell_commands_end; c++)
                if (kstrncmp(c->name, word, wl)==0) { console_puts(c->name); console_putc('\n'); }
        }
        return;
    }
    if (common > wl && match) {
        const char *src = match->name + wl; size_t extra = common - wl;
        for (size_t i = 0; i < extra && *len + i < SHELL_CMD_LEN - 1; i++) {
            buf[*len + i] = src[i]; console_putc((char)src[i]);
        }
        *len += extra; buf[*len] = '\0';
    }
}

static const char *shell_prompt = "eigen# ";

/* ── command implementations ──────────────────────────────────────────────── */

static int cmd_help(int argc, char **argv) {
    if (argc > 1) {
        const struct shell_cmd *c = shell_find_cmd(argv[1]);
        if (!c) { console_puts("No such command: "); console_puts(argv[1]); console_puts("\n"); return 1; }
        console_puts(c->name); console_puts(": "); console_puts(c->help); console_putc('\n');
        if (c->usage) { console_puts("Usage: "); console_puts(c->usage); console_putc('\n'); }
        return 0;
    }
    console_puts("Available commands:\n");
    shell_list_cmds();
    return 0;
}
SHELL_CMD("help", "Show this help message", "help [cmd]", cmd_help);
SHELL_CMD("?", "Alias for help", "? [cmd]", cmd_help);

static int cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("PID    PPID   STATE   NAME\n");
    for (struct process *p = process_list; p; p = p->next) {
        console_puts("0x"); puthex((uint64_t)p->pid); console_puts(" ");
        puthex((uint64_t)p->ppid); console_puts("   ");
        if (p->exited) console_puts("zomb  ");
        else if (p->stopped) console_puts("stop  ");
        else if (p->started) console_puts("run   ");
        else console_puts("new   ");
        console_puts(p->name); console_putc('\n');
    }
    return 0;
}
SHELL_CMD("ps", "List kernel processes", "ps", cmd_ps);

static int cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("Physical memory:\n  total = "); putdec(pmm_get_total_memory());
    console_puts(" bytes ("); putdec(pmm_get_total_pages()); console_puts(" pages)\n  free  = ");
    putdec(pmm_get_free_memory()); console_puts(" bytes ("); putdec(pmm_get_free_pages());
    console_puts(" pages)\n");
    return 0;
}
SHELL_CMD("mem", "Show physical memory status", "mem", cmd_mem);

static int cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t t = timer_get_ticks();
    uint32_t hz = timer_get_freq();
    if (!hz) hz = 100;
    console_puts("Timer ticks: "); putdec(t);
    console_puts(" @ "); putdec(hz); console_puts(" Hz\nUptime: ");
    putdec(t / hz); console_puts("."); putdec(((t % hz) * 100) / hz);
    console_puts(" s\n");
    return 0;
}
SHELL_CMD("uptime", "Show system uptime", "uptime", cmd_uptime);

static int cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    console_clear();
    console_puts("cleared\n");
    return 0;
}
SHELL_CMD("clear", "Clear the screen", "clear", cmd_clear);

static int cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("Rebooting...\n");
    uint8_t good = 0x02;
    while (good & 0x02) good = inb(0x64);
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile("hlt");
    return 0;
}
SHELL_CMD("reboot", "Reboot the machine", "reboot", cmd_reboot);

static int cmd_halt(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("Powering off...\n");
    extern int acpi_poweroff(void);
    if (acpi_poweroff() != 0) { console_puts("ACPI unavailable; halting.\n"); __asm__ volatile("cli; hlt"); }
    for (;;) __asm__ volatile("hlt");
    return 0;
}
SHELL_CMD("halt", "Power off the machine", "halt", cmd_halt);

static int cmd_exit(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("This is the kernel debugging shell; it cannot exit.\n");
    return 0;
}
SHELL_CMD("exit", "This IS the kernel shell (cannot exit)", "exit", cmd_exit);

static int cmd_ls(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/";
    struct vfs_node *dir = vfs_lookup(path);
    if (!dir) { console_puts("ls: cannot access '"); console_puts(path); console_puts("': No such file\n"); return 1; }
    console_puts(path); console_puts(":\n");
    if (!dir->readdir) { console_puts("ls: not a directory\n"); return 1; }
    uint32_t i = 0;
    for (;;) {
        const struct vfs_dirent *de = dir->readdir(dir, i);
        if (!de || de->name[0] == '\0') break;
        console_puts("  "); console_puts(de->name);
        if (de->type == VFS_DIRECTORY) console_putc('/');
        console_putc('\n');
        i++;
    }
    return 0;
}
SHELL_CMD("ls", "List a directory", "ls [dir]", cmd_ls);

static int cmd_cat(int argc, char **argv) {
    if (argc < 2) { console_puts("Usage: cat <file>\n"); return 1; }
    int fd = vfs_open(argv[1], 0, 0);
    if (fd < 0) { console_puts("cat: cannot open '"); console_puts(argv[1]); console_puts("'\n"); return 1; }
    char buf[256];
    for (;;) { int64_t n = vfs_read(fd, buf, sizeof(buf)); if (n <= 0) break;
        for (int64_t i = 0; i < n; i++) console_putc(buf[i]); }
    vfs_close(fd); console_putc('\n');
    return 0;
}
SHELL_CMD("cat", "Display file contents", "cat <file>", cmd_cat);

static int cmd_run(int argc, char **argv) {
    if (argc < 2) { console_puts("Usage: run <file> [args...]\n"); return 1; }
    const char *path = argv[1];
    const char *app = path;
    for (const char *p = path; *p; p++) if (*p == '/') app = p + 1;
    int nargs = argc - 1;
    char **nargv = (char**)kmalloc(sizeof(char*) * (nargs + 1));
    if (!nargv) { console_puts("run: out of memory\n"); return 1; }
    nargv[0] = (char*)app;
    for (int i = 1; i < nargs; i++) nargv[i] = argv[i + 1];
    nargv[nargs] = NULL;
    struct process *proc = process_create_from_elf(path, app, nargs, (const char**)nargv);
    if (!proc) { console_puts("run: failed to load "); console_puts(path); console_putc('\n'); return 1; }
    console_puts("run: launching '"); console_puts(path); console_puts("' (PID 0x");
    puthex(proc->pid); console_puts(")\n");
    int rc = process_run(proc);
    console_puts("run: child exited, status "); putdec((uint64_t)rc); console_putc('\n');
    return rc ? 1 : 0;
}
SHELL_CMD("run", "Load and execute an ELF binary", "run <file> [args...]", cmd_run);

static int cmd_irqstat(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("IRQ counts (vector -> count):\n");
    for (int i = 0; i < 256; i++) {
        uint64_t c = idt_get_irq_count(i);
        if (c) { console_puts("  ["); putdec(i); console_puts("] = "); putdec(c); console_putc('\n'); }
    }
    return 0;
}
SHELL_CMD("irqstat", "Show interrupt counts", "irqstat", cmd_irqstat);

static int cmd_modules(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!mod_req.response) { console_puts("No module response.\n"); return 1; }
    console_puts("Limine modules:\n");
    for (uint64_t i = 0; i < mod_req.response->module_count; i++) {
        console_puts("  ["); putdec(i); console_puts("] ");
        console_puts(mod_req.response->modules[i]->path);
        console_puts(" size="); putdec(mod_req.response->modules[i]->size); console_putc('\n');
    }
    return 0;
}
SHELL_CMD("modules", "List bootloader modules", "modules", cmd_modules);

static int cmd_pagemap(int argc, char **argv) {
    if (argc < 2) { console_puts("Usage: pagemap <vaddr>\n"); return 1; }
    uint64_t v = 0; const char *p = argv[1];
    if (p[0]=='0'&& (p[1]=='x'||p[1]=='X')) p += 2;
    while (*p) {
        v <<= 4;
        if (*p >= '0' && *p <= '9') v |= *p - '0';
        else if (*p >= 'a' && *p <= 'f') v |= *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v |= *p - 'A' + 10;
        else break;
        p++;
    }
    console_puts("vaddr 0x"); puthex(v);
    if (current_process && current_process->pml4) {
        uint64_t phys = vmm_virt_to_phys(current_process->pml4, v);
        console_puts(" -> phys 0x"); puthex(phys);
        console_puts(phys ? "\n" : " (not mapped)\n");
    }
    return 0;
}
SHELL_CMD("pagemap", "Translate vaddr to physical", "pagemap <vaddr>", cmd_pagemap);

static int cmd_addr2line(int argc, char **argv) {
    (void)argc; (void)argv;
    console_puts("addr2line: symbol table lookup not available in freestanding kernel.\n");
    console_puts("Use serial/debugger for address resolution.\n");
    return 0;
}
SHELL_CMD("addr2line", "Resolve address to symbol (stub)", "addr2line", cmd_addr2line);

static int cmd_mmap(int argc, char **argv) {
    (void)argc; (void)argv;
    if (current_process && current_process->pml4) {
        console_puts("Process virtual mapping: "); puthex((uint64_t)current_process->pml4);
        console_puts("\n  brk = "); puthex(current_process->brk);
        console_puts("\n  mmap_next = "); puthex(current_process->mmap_next);
        console_puts("\n  entry = "); puthex(current_process->entry_point);
        console_puts("\n  stack_top = "); puthex(current_process->user_stack_top);
    } else {
        console_puts("No current process.\n");
    }
    return 0;
}
SHELL_CMD("mmap", "Show current process address space", "mmap", cmd_mmap);

/* ── main shell loop ────────────────────────────────────────────────────── */

void shell_run(void) {
    char buf[SHELL_CMD_LEN];
    uint32_t len = 0;
    uint32_t cursor = 0;

    console_puts("\n======================================================\n");
    console_puts("  Eigen Experimental Kernel v0.1-alpha (debug shell)\n");
    console_puts("  Copyright (C) 2023-2026 Eldrick Sidney\n");
    console_puts("======================================================\n\n");
    console_puts("Type 'help' for available commands.\n\n");

    for (;;) {
        console_puts(shell_prompt);
        len = 0; cursor = 0; buf[0] = '\0';

        for (;;) {
            char c = keyboard_getchar();

            if (c == '\r' || c == '\n') {
                console_putc('\n');
                buf[len] = '\0';
                if (len > 0) hist_add(buf);
                char *argv[SHELL_MAX_ARGS];
                int argc = shell_parse_args(buf, argv, SHELL_MAX_ARGS);
                if (argc > 0) {
                    const struct shell_cmd *cmd = shell_find_cmd(argv[0]);
                    if (cmd) {
                        int rc = cmd->handler(argc, argv);
                        if (rc != 0) console_puts("command returned ");
                            /* putdec for status */ console_putc('0'+rc); console_putc('\n');
                    } else {
                        console_puts(argv[0]); console_puts(": command not found\n");
                    }
                }
                break;  /* reprint prompt */
            } else if (c == '\b' || c == 0x7F) {       /* backspace / del */
                if (cursor > 0) {
                    for (uint32_t i = cursor - 1; i < len; i++) buf[i] = buf[i+1];
                    len--; cursor--;
                    shell_redraw(shell_prompt, buf, len, cursor);
                }
            } else if (c == '\t') {                    /* tab -> complete */
                shell_complete(buf, &len);
                cursor = len;
            } else if (c == 0x01) {                    /* ^A home */
                cursor = 0;
                shell_redraw(shell_prompt, buf, len, cursor);
            } else if (c == 0x05) {                    /* ^E end */
                cursor = len;
            } else if (c == 0x0B) {                    /* ^K kill to end of line */
                len = cursor; buf[len] = '\0';
                console_puts("\r\033[K");
                console_puts(shell_prompt); console_puts(buf);
            } else if (c == 0x15) {                    /* ^U kill line */
                len = 0; cursor = 0; buf[0] = '\0';
                console_puts("\r\033[2K"); console_puts(shell_prompt);
            } else if (c == 0x03) {                    /* ^C */
                if (len > 0) { console_puts("^C\n"); buf[0]='\0'; len=0; cursor=0; break; }
                console_puts("^C\n");
            } else if (c == 0x13) {                    /* ^S pause — just ignore */
                ;
            } else if (c == 0x1C && kstrlen(buf)==0) {  /* ^_ / ^\ (if char is 0x1c) */
                ;
            } else if (c == 0x1B) {                    /* escape sequence */
                char e1 = keyboard_getchar();
                if (e1 == '[') {
                    char e2 = keyboard_getchar();
                    if (e2 == 'A') {       /* up — history */
                        if (hist_count > 0) {
                            if (hist_view < 0) hist_view = 0;
                            else if (hist_view < hist_count - 1) hist_view++;
                            kcpy(buf, shell_history[hist_view], SHELL_CMD_LEN);
                            len = kstrlen(buf); cursor = len;
                            shell_redraw(shell_prompt, buf, len, cursor);
                        }
                    } else if (e2 == 'B') {  /* down */
                        if (hist_view > 0) {
                            hist_view--;
                            if (hist_view < 0) { buf[0]='\0'; len=0; cursor=0; }
                            else { kcpy(buf, shell_history[hist_view], SHELL_CMD_LEN); len = kstrlen(buf); cursor = len; }
                            shell_redraw(shell_prompt, buf, len, cursor);
                        } else if (hist_view == 0) {
                            hist_view = -1; buf[0]='\0'; len=0; cursor=0;
                            console_puts("\r\033[2K"); console_puts(shell_prompt);
                        }
                    } else if (e2 == 'C') {  /* right */
                        if (cursor < len) { cursor++; console_puts("\033[C"); }
                    } else if (e2 == 'D') {  /* left */
                        if (cursor > 0) { cursor--; console_puts("\033[D"); }
                    } else {
                        /* drain other CSI sequences */
                        char x;
                        do { x = keyboard_getchar(); } while (x && x != '~' && x != 'm' && x != '"');
                    }
                } else if (e1 == 'O') {
                    char e2 = keyboard_getchar();
                    /* application keypad — drain */
                    (void)e2;
                } else {
                    /* ESC then something else — drain remaining bytes of seq */
                }
            } else if (c >= 0x20 && c < 0x7F) {        /* printable ASCII */
                if (len < SHELL_CMD_LEN - 1) {
                    for (uint32_t i = len; i > cursor; i--) buf[i] = buf[i-1];
                    buf[cursor] = c; len++; cursor++;
                    if (cursor == len) {
                        console_putc(c);
                    } else {
                        shell_redraw(shell_prompt, buf, len, cursor);
                    }
                }
            }
        }
    }
}
