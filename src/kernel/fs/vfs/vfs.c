/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * vfs.c - Virtual File System implementation.
 */

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"
#include "drivers/char/vterm_glue.h"
#include "mm/kheap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "drivers/pci/pci.h"
#include "net/socket.h"
#include "net/net.h"
#include "drivers/char/console.h"
#include "drivers/char/keyboard.h"
#include "dev/char/pty.h"
#include "dev/char/pts.h"
#include "dev/input/evdev.h"
#include "drivers/timer/pit.h"
#include "proc/process.h"
#include "acpi/acpi.h"
#include "arch/x86_64/cpu/idt.h"
#include "arch/x86_64/cpu/apic.h"
#include "include/serial.h"

static struct vfs_node *vfs_root = NULL;

/* Kernel shell descriptor table (used while current_process == NULL).
 * Every user process owns a private copy instead. */
static struct file_descriptor kfds[MAX_FD_COUNT];

static struct file_descriptor *cur_fds(void) {
    if (current_process) return current_process->fds;
    return kfds;
}

/* Status flags of the descriptor whose read op is running (see vfs_read). */
static int g_read_flags = 0;

/* Minimal tty line discipline (shared console): ISIG+ICANON/ECHO + VMIN/VTIME. */
static uint32_t tty_lflag = TTY_LFLAG_ISIG | TTY_LFLAG_ICANON | TTY_LFLAG_ECHO;
static uint8_t tty_vmin = 1;
static uint8_t tty_vtime = 0;

static size_t kstrlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static void *kmemcpy(void *dest, const void *src, size_t n) {
    char *d = dest;
    const char *s = src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

static void kstrncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
}

static int kstrcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

/* Helper: true when current process has an unblocked pending signal. */
int has_unblocked_pending(void) {
    if (!current_process) return 0;
    uint64_t pending = current_process->sig_pending & ~current_process->sig_blocked;
    /* SIGKILL/SIGSTOP are always unblocked */
    pending |= current_process->sig_pending & ((1ULL << 8) | (1ULL << 18));
    return pending != 0;
}

/* TTY input queue: bytes the keyboard path decoded go here, never back
 * through the PS/2 ring. stdin_read drains this; vterm_keyboard_feed fills
 * it via the vterm glue's tty_put callback. Bounded: oldest byte dropped
 * when full (a terminal drops rather than blocks the IRQ path). */
#define TTY_QSIZE 1024
static volatile char tty_queue[TTY_QSIZE];
static volatile uint32_t tty_qhead = 0;
static volatile uint32_t tty_qtail = 0;

static void tty_qput(char c) {
    uint32_t nx = (tty_qhead + 1) % TTY_QSIZE;
    if (nx == tty_qtail) tty_qtail = (tty_qtail + 1) % TTY_QSIZE; /* drop oldest */
    tty_queue[tty_qhead] = c;
    tty_qhead = nx;
}

static int tty_qget(void) {
    if (tty_qhead == tty_qtail) return -1;
    unsigned char c = tty_queue[tty_qtail];
    tty_qtail = (tty_qtail + 1) % TTY_QSIZE;
    return (int)c;
}

static int tty_qready(void) {
    return tty_qhead != tty_qtail;
}

/* Stdin Node Operations (Console/Keyboard) */
static int64_t stdin_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    (void)offset;
    char *cbuf = (char *)buf;
    size_t read_bytes = 0;
    int echo = (tty_lflag & TTY_LFLAG_ECHO) != 0;
    int isig = (tty_lflag & TTY_LFLAG_ISIG) != 0;

    /* Canonical mode: line-buffered with echo, backspace-erase, classic tty. */
    if (tty_lflag & TTY_LFLAG_ICANON) {
        while (read_bytes < size) {
            int ch;
            /* Interruptible wait. tty_poll_signal() pumps the serial path
             * (and PS/2 pre-IRQ) so remote input reaches the queue. */
            for (;;) {
                if (has_unblocked_pending()) return -4; /* -EINTR */
                ch = tty_qget();
                if (ch >= 0) break;
                tty_poll_signal();
                __asm__ volatile("pause");
            }
            /* ISIG mapping: VINTR=3 ^C, VQUIT=28 ^\, VSUSP=26 ^Z */
            if (isig) {
                if (ch == 3) { /* VINTR */
                    if (current_process) process_raise(2); /* SIGINT */
                    return read_bytes ? (int64_t)read_bytes : -4;
                }
                if (ch == 28) { /* VQUIT */
                    if (current_process) process_raise(3); /* SIGQUIT */
                    return read_bytes ? (int64_t)read_bytes : -4;
                }
                if (ch == 26) { /* VSUSP */
                    if (current_process) process_raise(20); /* SIGTSTP */
                    return read_bytes ? (int64_t)read_bytes : -4;
                }
            }
            /* Backspace (DEL 0x7F or BS 0x08): erase the last char. */
            if (ch == 0x7F || ch == 0x08) {
                if (read_bytes > 0) {
                    read_bytes--;
                    if (echo) {
                        const char *bs = "\b \b";
                        vterm_console_write(bs, 3);
                        for (int i = 0; i < 3; i++) serial_putc_raw(bs[i]);
                    }
                }
                continue;
            }
            /* Ctrl-U (NAK 0x15): erase the entire line. */
            if (ch == 0x15) {
                if (echo) {
                    for (size_t i = 0; i < read_bytes; i++) {
                        const char *bs = "\b \b";
                        vterm_console_write(bs, 3);
                        for (int k = 0; k < 3; k++) serial_putc_raw(bs[k]);
                    }
                }
                read_bytes = 0;
                continue;
            }
            cbuf[read_bytes++] = (char)ch;
            if (echo) {
                /* Echo goes through the same OPOST translation as output. */
                if (ch == '\n') {
                    vterm_console_write("\r\n", 2);
                    serial_putc_raw('\r');
                    serial_putc_raw('\n');
                } else {
                    char ech = (char)ch;
                    vterm_console_write(&ech, 1);
                    serial_putc_raw(ech);
                }
            }
            if (ch == '\n' || ch == '\r') break;
        }
        return (int64_t)read_bytes;
    }

    /* Raw mode: first byte blocks unless VMIN==0, the rest never blocks. */
    if (size == 0) return 0;
    int c;
    if (tty_vmin > 0) {
        for (;;) {
            if (has_unblocked_pending()) return -4;
            c = tty_qget();
            if (c >= 0) break;
            tty_poll_signal();
            __asm__ volatile("pause");
        }
    } else {
        c = tty_qget();
        if (c < 0) {
            if (has_unblocked_pending()) return -4;
            return 0;
        }
    }
    if (isig) {
        if (c == 3) { if (current_process) process_raise(2); return -4; }
        if (c == 28) { if (current_process) process_raise(3); return -4; }
        if (c == 26) { if (current_process) process_raise(20); return -4; }
    }
    cbuf[read_bytes++] = (char)c;
    if (echo) {
        /* Echo goes through the same OPOST translation as program output. */
        if (c == '\n') {
            vterm_console_write("\r\n", 2);
            serial_putc_raw('\r');
            serial_putc_raw('\n');
        } else {
            char ech = (char)c;
            vterm_console_write(&ech, 1);
            serial_putc_raw(ech);
        }
    }
    while (read_bytes < size) {
        if (has_unblocked_pending()) break;
        c = tty_qget();
        if (c < 0) break;
        tty_poll_signal();
        if (isig && (c == 3 || c == 28 || c == 26)) {
            /* Push back signal char handling: raise and stop filling */
            if (c == 3) process_raise(2);
            else if (c == 28) process_raise(3);
            else process_raise(20);
            break;
        }
        cbuf[read_bytes++] = (char)c;
        if (echo) {
            if (c == '\n') {
                vterm_console_write("\r\n", 2);
                serial_putc_raw('\r');
                serial_putc_raw('\n');
            } else {
                char ech = (char)c;
                vterm_console_write(&ech, 1);
                serial_putc_raw(ech);
            }
        }
    }
    return (int64_t)read_bytes;
}

/* TTY poll: readable only while the decoded queue holds bytes. Every input
 * path (PS/2, serial) funnels through kbd_push_char -> libvterm -> queue.
 * evdev readiness is NOT consulted here: the console fd and /dev/input/event*
 * are separate devices, and a mouse streaming motion events must never make
 * the shell's poll(stdin) claim readable. */
int vfs_tty_poll(int events) {
    int mask = 0;
    if ((events & 0x001) && tty_qready()) mask |= 0x001;
    if (events & 0x004) mask |= 0x004; /* a tty is always writable */
    return mask;
}

/* tty output post-processing (OPOST|ONLCR): bare LF must become CR+LF or
 * libvterm (which treats LF as index-only, column preserved) leaves the
 * cursor mid-line and the prompt walks right after every Enter. */
static void tty_output_write(const char *buf, size_t size) {
    size_t start = 0;
    for (size_t i = 0; i < size; i++) {
        if (buf[i] != '\n') continue;
        if (i > start) vterm_console_write(buf + start, i - start);
        vterm_console_write("\r\n", 2);
        start = i + 1;
    }
    if (size > start) vterm_console_write(buf + start, size - start);
    for (size_t i = 0; i < size; i++) serial_putc_raw(buf[i]);
}

/* Stdout / Stderr Node Operations (Console/Serial) */
static int64_t stdout_write(struct vfs_node *node, uint64_t offset, size_t size, const void *buf) {
    (void)node;
    (void)offset;
    if (buf && size) tty_output_write((const char *)buf, size);
    return (int64_t)size;
}

static struct vfs_node stdin_node;
static struct vfs_node stdout_node;
static struct vfs_node stderr_node;

uint32_t tty_get_lflag(void) {
    return tty_lflag;
}

uint8_t tty_get_vmin(void) {
    return tty_vmin;
}

uint8_t tty_get_vtime(void) {
    return tty_vtime;
}

void tty_set_termios(uint32_t lflag, uint8_t vmin, uint8_t vtime) {
    tty_lflag = lflag;
    tty_vmin = vmin;
    tty_vtime = vtime;
}

void tty_handle_signal(char c) {
    if (!(tty_lflag & TTY_LFLAG_ISIG)) return;
    if (!current_process || !current_process->has_ctty) return;
    if ((uint8_t)c == 3) process_raise(2);  /* VINTR -> SIGINT */
    else if ((uint8_t)c == 28) process_raise(3); /* VQUIT -> SIGQUIT */
    else if ((uint8_t)c == 26) process_raise(20); /* VSUSP -> SIGTSTP */
}

/* libvterm keyboard path: same queue the tty drains. Registered once via
 * vterm_console_set_tty_put() so decoded bytes never loop through PS/2. */
void vfs_tty_qput(char c) { tty_qput(c); }

/* Map a file-backed range into the caller's address space. A node with a
 * mmap hook (fb0) maps its own pages; anything else is -ENODEV. */
int vfs_mmap_fd(int fd, uint64_t vaddr, uint64_t size, uint64_t offset,
                uint64_t pte_flags) {
    struct file_descriptor *fd_table = cur_fds();
    if (fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used) return -9;
    struct vfs_node *node = fd_table[fd].node;
    if (node && node->mmap)
        return (int)node->mmap(node, vaddr, size, offset, pte_flags);
    return -19; /* -ENODEV: plain VFS files are not directly mappable */
}

void vfs_install_stdio(struct file_descriptor *table) {
    if (!table) return;

    /* Callers pass zeroed tables (kzalloc): pty_num/sock_alias would read as
     * 0 and route fds to PTY 0. Initialize every slot defensively. */
    for (int i = 0; i < MAX_FD_COUNT; i++) {
        table[i].node = NULL;
        table[i].offset = 0;
        table[i].flags = 0;
        table[i].is_used = 0;
        table[i].sock_alias = -1;
        table[i].pty_num = -1;
    }

    kstrncpy(stdin_node.name, "stdin", sizeof(stdin_node.name));
    stdin_node.type = VFS_CHARDEVICE;
    stdin_node.permissions = 0620;
    stdin_node.inode = 8; /* match devfs tty so stat/fstat agree for ttyname */
    stdin_node.read = stdin_read;
    table[0].node = &stdin_node;
    table[0].offset = 0;
    table[0].flags = O_RDONLY;
    table[0].is_used = 1;
    table[0].sock_alias = -1;
    table[0].pty_num = -1;

    kstrncpy(stdout_node.name, "stdout", sizeof(stdout_node.name));
    stdout_node.type = VFS_CHARDEVICE;
    stdout_node.permissions = 0620;
    stdout_node.inode = 8;
    stdout_node.write = stdout_write;
    table[1].node = &stdout_node;
    table[1].offset = 0;
    table[1].flags = O_WRONLY;
    table[1].is_used = 1;
    table[1].sock_alias = -1;
    table[1].pty_num = -1;

    kstrncpy(stderr_node.name, "stderr", sizeof(stderr_node.name));
    stderr_node.type = VFS_CHARDEVICE;
    stderr_node.permissions = 0620;
    stderr_node.inode = 8;
    stderr_node.write = stdout_write;
    table[2].node = &stderr_node;
    table[2].offset = 0;
    table[2].flags = O_WRONLY;
    table[2].is_used = 1;
    table[2].sock_alias = -1;
    table[2].pty_num = -1;

    /* Fresh stdio on a session leader (login shell) owns the tty. */
    if (current_process && !current_process->has_ctty &&
        (current_process->sid == 0 || current_process->sid == current_process->pid)) {
        current_process->has_ctty = 1;
    }
}

void vfs_init(void) {
    struct file_descriptor *fd_table = kfds;
    for (int i = 0; i < MAX_FD_COUNT; i++) {
        fd_table[i].node = NULL;
        fd_table[i].offset = 0;
        fd_table[i].flags = 0;
        fd_table[i].is_used = 0;
        fd_table[i].sock_alias = -1;
        fd_table[i].pty_num = -1;
    }

    vfs_install_stdio(fd_table);

    pty_init();

    serial_puts("[VFS] Virtual File System initialized (stdin/stdout/stderr ready).\n");
}

/* Mount table for /proc/mounts and /proc/partitions. */
#define MOUNT_MAX 8

static struct {
    char mp[64];
    char fstype[16];
    char src[32];
} mount_tab[MOUNT_MAX];
static int nmounts = 0;

void vfs_mount_root(struct vfs_node *root) {
    vfs_root = root;
    kstrncpy(mount_tab[0].mp, "/", sizeof(mount_tab[0].mp));
    kstrncpy(mount_tab[0].fstype, "tarfs", sizeof(mount_tab[0].fstype));
    kstrncpy(mount_tab[0].src, "initramfs", sizeof(mount_tab[0].src));
    nmounts = 1;
    serial_puts("[VFS] Root filesystem mounted at '/'\n");
}

int vfs_mount_named(const char *mountpoint, struct vfs_node *fs_root,
                    const char *fstype, const char *src) {
    int r = vfs_mount(mountpoint, fs_root);
    if (r == 0 && nmounts < MOUNT_MAX) {
        kstrncpy(mount_tab[nmounts].mp, mountpoint, sizeof(mount_tab[0].mp));
        kstrncpy(mount_tab[nmounts].fstype, fstype ? fstype : "?", sizeof(mount_tab[0].fstype));
        kstrncpy(mount_tab[nmounts].src, src ? src : "?", sizeof(mount_tab[0].src));
        nmounts++;
    }
    return r;
}

static struct vfs_node *vfs_lookup_full(const char *path, int follow, int depth);
static struct vfs_node *vfs_lookup_nofollow(const char *path);

/* Collapse ".", "..", duplicate and trailing slashes lexically so the
 * walk below only ever sees real components. */
void vfs_normalize(const char *path, char *out, size_t out_size) {
    char comps[16][128];
    int ncomps = 0;
    int absolute = (path[0] == '/');

    const char *p = path;
    while (*p && ncomps < 16) {
        while (*p == '/') p++;
        if (*p == '\0') break;
        int i = 0;
        while (*p && *p != '/' && i < 127) comps[ncomps][i++] = *p++;
        comps[ncomps][i] = '\0';
        if (kstrcmp(comps[ncomps], ".") == 0) continue;
        if (kstrcmp(comps[ncomps], "..") == 0) {
            if (ncomps > 0) ncomps--;
            continue;
        }
        ncomps++;
    }

    size_t pos = 0;
    if (absolute && pos + 1 < out_size) out[pos++] = '/';
    for (int c = 0; c < ncomps && pos + 1 < out_size; c++) {
        if (c > 0 && pos + 1 < out_size) out[pos++] = '/';
        for (int i = 0; comps[c][i] && pos + 1 < out_size; i++) {
            out[pos++] = comps[c][i];
        }
    }
    if (pos == 0 && pos + 1 < out_size) out[pos++] = '/';
    out[pos] = '\0';
}

/* Join a possibly-relative path against the process cwd so parent-dir
 * splits below see an absolute path. */
static void vfs_abspath(const char *path, char *out, size_t out_size) {
    if (!path || !out || out_size == 0) return;
    if (path[0] == '/') {
        size_t i = 0;
        while (path[i] && i + 1 < out_size) {
            out[i] = path[i];
            i++;
        }
        out[i] = '\0';
        return;
    }
    const char *cwd = "/";
    if (current_process && current_process->cwd[0] != '\0') {
        cwd = current_process->cwd;
    }
    size_t pos = 0;
    while (cwd[pos] && pos + 1 < out_size) {
        out[pos] = cwd[pos];
        pos++;
    }
    if (pos == 0 || out[pos - 1] != '/') {
        if (pos + 1 >= out_size) {
            out[0] = '\0';
            return;
        }
        out[pos++] = '/';
    }
    for (size_t i = 0; path[i] && pos + 1 < out_size; i++) {
        out[pos++] = path[i];
    }
    out[pos] = '\0';
}

int vfs_mount(const char *mountpoint, struct vfs_node *fs_root) {
    if (!mountpoint || !fs_root) return -22; /* -EINVAL */
    struct vfs_node *mp = vfs_lookup_nofollow(mountpoint);
    if (!mp) return -2; /* -ENOENT */
    mp->type = VFS_MOUNTPOINT;
    mp->mount_target = fs_root;
    return 0;
}

struct vfs_node *vfs_lookup(const char *path) {
    return vfs_lookup_full(path, 1, 0);
}

static struct vfs_node *vfs_lookup_nofollow(const char *path) {
    return vfs_lookup_full(path, 0, 0);
}

static struct vfs_node *vfs_lookup_full(const char *path, int follow, int depth) {
    if (!path || path[0] == '\0') return NULL;
    if (!vfs_root) return NULL;

    /* Relative paths resolve against the process working directory. */
    char abs[512];
    if (path[0] != '/') {
        const char *cwd = "/";
        if (current_process && current_process->cwd[0] != '\0') {
            cwd = current_process->cwd;
        }
        size_t pos = 0;
        while (cwd[pos] && pos < sizeof(abs) - 1) {
            abs[pos] = cwd[pos];
            pos++;
        }
        if (pos == 0 || abs[pos - 1] != '/') {
            if (pos + 1 >= sizeof(abs)) return NULL;
            abs[pos++] = '/';
        }
        for (size_t i = 0; path[i] && pos + 1 < sizeof(abs); i++) {
            abs[pos++] = path[i];
        }
        abs[pos] = '\0';
        path = abs;
    }

    char norm[256];
    vfs_normalize(path, norm, sizeof(norm));
    path = norm;

    if (kstrcmp(path, "/") == 0) return vfs_root;

    /* Start from root */
    struct vfs_node *current = vfs_root;
    if (current->type == VFS_MOUNTPOINT && current->mount_target) {
        current = current->mount_target;
    }

    const char *p = path;
    if (*p == '/') p++; /* Skip leading slash */

    char component[128];

    while (*p) {
        const char *comp_start = p;
        int i = 0;
        while (*p && *p != '/' && i < 127) {
            component[i++] = *p++;
        }
        component[i] = '\0';
        const char *rest = p; /* points at '/' or '\0' */

        if (*p == '/') p++;

        if (kstrcmp(component, ".") == 0) continue;
        if (kstrcmp(component, "..") == 0) {
            /* Root's parent is root itself */
            continue;
        }

        if (!current->finddir) return NULL;

        struct vfs_node *next = current->finddir(current, component);
        if (!next) return NULL;

        if (next->type == VFS_MOUNTPOINT && next->mount_target) {
            current = next->mount_target;
        } else if ((follow || *rest) && next->type == VFS_SYMLINK) {
            /* Resolve link target against the containing directory,
             * then continue with any remaining path components. */
            if (depth >= 8) return NULL;
            char resolved[256];
            int ri = 0;
            if (next->linktarget[0] != '/') {
                size_t prefix = (size_t)(comp_start - path);
                if (prefix > sizeof(resolved) - 1) return NULL;
                for (size_t k = 0; k < prefix; k++) resolved[ri++] = path[k];
            }
            for (int k = 0; next->linktarget[k] && ri < 200; k++) {
                resolved[ri++] = next->linktarget[k];
            }
            for (int k = 0; rest[k] && ri < 255; k++) {
                resolved[ri++] = rest[k];
            }
            resolved[ri] = '\0';
            return vfs_lookup_full(resolved, follow, depth + 1);
        } else {
            current = next;
        }
    }

    return current;
}

int vfs_open(const char *path, int flags, int mode) {
    struct file_descriptor *fd_table = cur_fds();
    struct vfs_node *node = vfs_lookup(path);

    if (node) {
        /* O_EXCL fails when the file already existed beforehand */
        if ((flags & O_CREAT) && (flags & O_EXCL)) return -17; /* -EEXIST */
        if (node->type == VFS_SOCKET) return -6; /* -ENXIO: no open() on sockets */
    } else {
        if (!(flags & O_CREAT)) return -2; /* -ENOENT */
        /* Split parent directory and leaf name (against cwd when relative) */
        char abs[512];
        vfs_abspath(path, abs, sizeof(abs));
        if (abs[0] == '\0') return -36; /* -ENAMETOOLONG */
        path = abs;
        char dir[256];
        const char *leaf = path;
        for (const char *s = path; *s; s++) {
            if (*s == '/' && *(s + 1) != '\0') leaf = s + 1;
        }
        size_t dirlen = (size_t)(leaf - path);
        if (dirlen <= 1) {
            dir[0] = '/';
            dir[1] = '\0';
        } else {
            if (dirlen > sizeof(dir)) return -36; /* -ENAMETOOLONG */
            for (size_t i = 0; i < dirlen - 1; i++) dir[i] = path[i];
            dir[dirlen - 1] = '\0';
        }
        char base[128];
        size_t blen = 0;
        while (leaf[blen] && blen < sizeof(base) - 1) {
            base[blen] = leaf[blen];
            blen++;
        }
        base[blen] = '\0';
        if (base[0] == '\0') return -2; /* -ENOENT */

        struct vfs_node *parent = vfs_lookup(dir);
        if (!parent || parent->type != VFS_DIRECTORY) return -2;
        if (!parent->create) return -30; /* -EROFS */
        int r = parent->create(parent, base, (uint16_t)mode);
        if (r < 0) return r;
        node = vfs_lookup(path);
        if (!node) return -2;
    }

    /* Lowest free descriptor (Unix contract: closed 0/1/2 are reusable) */
    int fd = -1;
    for (int i = 0; i < MAX_FD_COUNT; i++) {
        if (!fd_table[i].is_used) {
            fd = i;
            break;
        }
    }

    if (fd < 0) return -24; /* -EMFILE */

    /* Default: not a PTY master. ptmx open below overrides. */
    fd_table[fd].pty_num = -1;
    /* For ptmx: the open handler returns 200 + pty number */
    if (node->type == VFS_CHARDEVICE && node->open) {
        int res = node->open(node, flags);
        if (res < 0) return res;
        if (res >= 200) fd_table[fd].pty_num = res - 200;
    }

    fd_table[fd].node = node;
    fd_table[fd].offset = 0;
    fd_table[fd].flags = flags;
    fd_table[fd].is_used = 1;
    fd_table[fd].sock_alias = -1;

    /* POSIX: session leader opening a tty acquires it as controlling terminal. */
    if (current_process && !current_process->has_ctty &&
        (current_process->sid == 0 || current_process->sid == current_process->pid) &&
        node && node->type == VFS_CHARDEVICE &&
        (node->read == stdin_read || node->write == stdout_write)) {
        current_process->has_ctty = 1;
    }

    if ((flags & O_TRUNC) && node->type == VFS_FILE) {
        if (node->truncate) {
            node->truncate(node, 0);
        } else {
            fd_table[fd].node = NULL;
            fd_table[fd].is_used = 0;
            fd_table[fd].pty_num = -1;
            return -30; /* -EROFS */
        }
    }
    if (flags & O_APPEND) {
        fd_table[fd].offset = node->size;
    }

    return fd;
}

/* Install a socket alias in a free VFS slot. Returns the slot or negative errno. */
static int vfs_alloc_sock_alias(int sock_idx) {
    struct file_descriptor *fd_table = cur_fds();
    for (int i = 0; i < MAX_FD_COUNT; i++) {
        if (!fd_table[i].is_used) {
            fd_table[i].node = NULL;
            fd_table[i].offset = 0;
            fd_table[i].flags = O_RDWR;
            fd_table[i].is_used = 1;
            fd_table[i].sock_alias = sock_idx;
            fd_table[i].pty_num = -1;
            sys_sock_incref(sock_idx);
            return i;
        }
    }
    return -24; /* -EMFILE */
}

/* Allocate a user fd naming kernel socket `sock_idx`. This is what makes
 * sockets ordinary file descriptors: dup2 can move them to fd 0 (busybox
 * ping does exactly that), close frees the alias, and read/write/poll
 * resolve through the fd table like every other descriptor. */
int vfs_install_socket(int sock_idx) {
    return vfs_alloc_sock_alias(sock_idx);
}

/* Install an already-built node as a new fd. Virtual filesystems that create
 * inodes without a path (memfd/shmem, epoll instances) need this: the node
 * exists, it just has no directory entry to look it up by. */
int vfs_install_node(struct vfs_node *node) {
    if (!node) return -22; /* -EINVAL */
    struct file_descriptor *fd_table = cur_fds();
    for (int i = 0; i < MAX_FD_COUNT; i++) {
        if (!fd_table[i].is_used) {
            fd_table[i].node       = node;
            fd_table[i].offset     = 0;
            fd_table[i].flags      = O_RDWR;
            fd_table[i].is_used    = 1;
            fd_table[i].sock_alias = -1;
            fd_table[i].pty_num    = -1;
            return i;
        }
    }
    return -24; /* -EMFILE */
}

/* Resolve a user fd to the numeric kernel socket handle it names, or -1.
 * Both spellings are accepted: the raw socket handle and the fd-table alias
 * (what dup/dup2/stdio redirection produce). */
int vfs_sock_fd(int fd) {
    if (is_socket_fd(fd)) return fd;
    struct file_descriptor *fd_table = cur_fds();
    if (fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used) return -1;
    if (fd_table[fd].node || fd_table[fd].sock_alias < 0) return -1;
    int sfd = SOCKET_FD_BASE + fd_table[fd].sock_alias;
    return is_socket_fd(sfd) ? sfd : -1;
}

int64_t vfs_read(int fd, void *buf, size_t count) {
    struct file_descriptor *fd_table = cur_fds();
    if (fd < 0 || fd >= MAX_FD_COUNT) return -9; /* -EBADF */
    /* Raw kernel socket fd (socket()/accept() numbers, not VFS aliases):
     * route to the socket layer — unless this slot holds a live VFS entry,
     * which always wins so a regular file at fd >= 64 is never hijacked. */
    if (!fd_table[fd].is_used && is_socket_fd(fd))
        return sys_sock_recv(fd, buf, count, 0);
    if (!fd_table[fd].is_used) return -1;
    struct file_descriptor *f = &fd_table[fd];

    /* PTY master: dispatch through pty_master_read/write with pty_num */
    if (f->pty_num >= 0 && f->pty_num < PTY_MAX_NUM) {
        return pty_master_read(f->pty_num, 0, count, buf);
    }

    if (!f->node) {
        int sfd = vfs_sock_fd(fd);
        if (sfd < 0) return -9;
        return sys_sock_recv(sfd, buf, count, 0);
    }
    if (!f->node->read) return -1;

    /* Publish the descriptor's status flags for the duration of the call.
     * A node op receives only (node, offset, size, buf), but some devices
     * must honour O_NONBLOCK (evdev: return -EAGAIN instead of parking),
     * and that flag lives on the descriptor, not the node. */
    g_read_flags = f->flags;
    int64_t bytes = f->node->read(f->node, f->offset, count, buf);
    g_read_flags = 0;
    if (bytes > 0) {
        f->offset += bytes;
    }
    return bytes;
}

/* Status flags of the descriptor currently being read (0 outside a read).
 * See the note in vfs_read. */
int vfs_read_flags(void) {
    return g_read_flags;
}

int64_t vfs_write(int fd, const void *buf, size_t count) {
    struct file_descriptor *fd_table = cur_fds();
    if (fd < 0 || fd >= MAX_FD_COUNT) return -9; /* -EBADF */
    /* Same raw-socket routing as vfs_read (live VFS entry wins). */
    if (!fd_table[fd].is_used && is_socket_fd(fd))
        return sys_sock_send(fd, buf, count, 0);
    if (!fd_table[fd].is_used) return -1;
    struct file_descriptor *f = &fd_table[fd];

    /* PTY master: dispatch through pty_master_read/write with pty_num */
    if (f->pty_num >= 0 && f->pty_num < PTY_MAX_NUM) {
        return pty_master_write(f->pty_num, 0, count, buf);
    }

    if (!f->node) {
        int sfd = vfs_sock_fd(fd);
        if (sfd < 0) return -9;
        return sys_sock_send(sfd, buf, count, 0);
    }
    if (!f->node->write) return -1;

    int64_t bytes = f->node->write(f->node, f->offset, count, buf);
    if (bytes > 0) {
        f->offset += bytes;
    }
    return bytes;
}

int vfs_close(int fd) {
    struct file_descriptor *fd_table = cur_fds();
    if (fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used) return -1;
    struct file_descriptor *f = &fd_table[fd];

    if (f->pty_num >= 0) {
        pty_master_close(f->pty_num);
        f->pty_num = -1;
    }
    if (!f->node) {
        /* Socket alias: shared entry dies on the last close only. */
        if (f->sock_alias >= 0) {
            sys_sock_close(SOCKET_FD_BASE + f->sock_alias);
        }
    } else if (f->node->close) {
        f->node->close(f->node);
    }

    f->node = NULL;
    f->offset = 0;
    f->flags = 0;
    f->is_used = 0;
    f->sock_alias = -1;
    f->pty_num = -1;
    return 0;
}

int64_t vfs_lseek(int fd, int64_t offset, int whence) {
    struct file_descriptor *fd_table = cur_fds();
    if (fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used) return -1;
    struct file_descriptor *f = &fd_table[fd];

    int64_t new_offset = f->offset;
    if (whence == SEEK_SET) {
        new_offset = offset;
    } else if (whence == SEEK_CUR) {
        new_offset += offset;
    } else if (whence == SEEK_END) {
        if (!f->node) return -1;
        new_offset = (int64_t)f->node->size + offset;
    } else {
        return -1;
    }

    if (new_offset < 0) return -1;
    f->offset = (uint64_t)new_offset;
    return new_offset;
}

static void populate_stat(struct vfs_node *node, struct vfs_stat *st) {
    uint8_t *p = (uint8_t *)st;
    for (size_t i = 0; i < sizeof(struct vfs_stat); i++) p[i] = 0;

    uint32_t mode = node->permissions ? node->permissions : 0755;
    if (node->type == VFS_DIRECTORY) {
        mode |= 0040000; /* S_IFDIR */
    } else if (node->type == VFS_CHARDEVICE) {
        mode |= 0020000; /* S_IFCHR */
    } else if (node->type == VFS_SYMLINK) {
        mode |= 0120000; /* S_IFLNK */
    } else if (node->type == VFS_SOCKET) {
        mode |= 0140000; /* S_IFSOCK */
    } else {
        mode |= 0100000; /* S_IFREG */
    }

    st->st_dev     = 1;
    st->st_ino     = node->inode ? node->inode : 1;
    st->st_nlink   = (node->type == VFS_DIRECTORY) ? 2 : 1;
    st->st_mode    = mode;
    st->st_uid     = node->uid;
    st->st_gid     = node->gid;
    st->st_rdev    = 0;
    st->st_size    = (int64_t)node->size;
    st->st_blksize = 4096;
    st->st_blocks  = (int64_t)((node->size + 511) / 512);
}

int vfs_stat(const char *path, struct vfs_stat *st) {
    if (!st) return -14; /* -EFAULT */
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -2; /* -ENOENT */
    populate_stat(node, st);
    return 0;
}

int vfs_lstat(const char *path, struct vfs_stat *st) {
    if (!st) return -14; /* -EFAULT */
    struct vfs_node *node = vfs_lookup_nofollow(path);
    if (!node) return -2; /* -ENOENT */
    populate_stat(node, st);
    return 0;
}

int64_t vfs_readlink(const char *path, char *buf, size_t size) {
    if (!buf || size == 0) return -14; /* -EFAULT */
    struct vfs_node *node = vfs_lookup_nofollow(path);
    if (!node) return -2; /* -ENOENT */
    if (node->type != VFS_SYMLINK) return -22; /* -EINVAL */

    size_t len = 0;
    while (node->linktarget[len]) len++;
    if (len > size) len = size;
    for (size_t i = 0; i < len; i++) buf[i] = node->linktarget[i];
    return (int64_t)len;
}

int vfs_fstat(int fd, struct vfs_stat *st) {
    struct file_descriptor *fd_table = cur_fds();
    if (fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used) return -9; /* -EBADF */
    if (!st) return -14; /* -EFAULT */
    struct vfs_node *node = fd_table[fd].node;
    if (!node) return -2; /* -ENOENT */
    populate_stat(node, st);
    return 0;
}

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
} __attribute__((packed));

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

int64_t vfs_getdents64(int fd, void *dirp, size_t count) {
    struct file_descriptor *fd_table = cur_fds();
    if (fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used || !dirp) return -9; /* -EBADF */
    struct file_descriptor *f = &fd_table[fd];
    if (!f->node || f->node->type != VFS_DIRECTORY || !f->node->readdir) return -20; /* -ENOTDIR */

    size_t bytes_written = 0;

    while (bytes_written < count) {
        char name[128];
        uint64_t ino = 0;
        uint8_t dtype = DT_REG;

        if (f->offset == 0) {
            name[0] = '.'; name[1] = '\0';
            ino = f->node->inode ? f->node->inode : 1;
            dtype = DT_DIR;
        } else if (f->offset == 1) {
            name[0] = '.'; name[1] = '.'; name[2] = '\0';
            ino = 1;
            dtype = DT_DIR;
        } else {
            struct vfs_dirent *de = f->node->readdir(f->node, (uint32_t)(f->offset - 2));
            if (!de) break; /* End of directory */

            int ni = 0;
            while (de->name[ni] && ni < 127) {
                name[ni] = de->name[ni];
                ni++;
            }
            name[ni] = '\0';
            ino = de->ino ? de->ino : (f->offset + 10);
            if (de->type == VFS_DIRECTORY)   dtype = DT_DIR;
            else if (de->type == VFS_CHARDEVICE) dtype = DT_CHR;
            else if (de->type == VFS_SYMLINK)    dtype = DT_LNK;
            else if (de->type == VFS_SOCKET)     dtype = DT_SOCK;
            else dtype = DT_REG;
        }

        size_t name_len = 0;
        while (name[name_len]) name_len++;

        /* 19 bytes fixed header: 8 (ino) + 8 (off) + 2 (reclen) + 1 (type) + name_len + 1 (null) */
        size_t rec_len = (19 + name_len + 1 + 7) & ~7ULL;
        if (bytes_written + rec_len > count) {
            if (bytes_written == 0) return -22; /* -EINVAL: buffer too small */
            break;
        }

        struct linux_dirent64 *d = (struct linux_dirent64 *)((uint8_t *)dirp + bytes_written);
        d->d_ino    = ino;
        d->d_off    = (int64_t)(f->offset + 1);
        d->d_reclen = (uint16_t)rec_len;
        d->d_type   = dtype;

        for (size_t i = 0; i <= name_len; i++) {
            d->d_name[i] = name[i];
        }

        /* Zero out any remaining padding */
        for (size_t i = 19 + name_len + 1; i < rec_len; i++) {
            ((uint8_t *)d)[i] = 0;
        }

        bytes_written += rec_len;
        f->offset++;
    }

    return (int64_t)bytes_written;
}

#define PIPE_BUF_SIZE (64 * 1024)

struct pipe {
    uint8_t *buffer;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    int      readers;
    int      writers;
    struct vfs_node read_node;
    struct vfs_node write_node;
};

/* fcntl(F_DUPFD): duplicate oldfd onto the lowest free fd >= min_fd. */
int vfs_fcntl_dupfd(int oldfd, int min_fd) {
    struct file_descriptor *fd_table = cur_fds();
    if (min_fd < 0) min_fd = 0;
    if (oldfd < 0 || oldfd >= MAX_FD_COUNT || !fd_table[oldfd].is_used) return -1;
    for (int i = min_fd; i < MAX_FD_COUNT; i++) {
        if (!fd_table[i].is_used) {
            return vfs_dup2(oldfd, i);
        }
    }
    return -24; /* -EMFILE */
}

/* fcntl(F_GETFL): return the open flags of fd. */
int vfs_fcntl_getfl(int fd) {
    struct file_descriptor *fd_table = cur_fds();
    if (fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used) return -1;
    return fd_table[fd].flags;
}

/* fcntl(F_SETFL): record non-block-request for console fds; always succeeds. */
int vfs_fcntl_setfl(int fd, int flags) {
    struct file_descriptor *fd_table = cur_fds();
    if (fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used) return -1;
    /* Preserve access mode, update status flags (O_NONBLOCK etc. tolerated) */
    fd_table[fd].flags = flags;
    return 0;
}

int vfs_dup(int oldfd) {
    /* Numeric socket fds dup into VFS slots as aliases. */
    if (is_socket_fd(oldfd)) {
        return vfs_alloc_sock_alias(oldfd - SOCKET_FD_BASE);
    }
    struct file_descriptor *fd_table = cur_fds();
    if (oldfd < 0 || oldfd >= MAX_FD_COUNT || !fd_table[oldfd].is_used) return -1;
    for (int i = 0; i < MAX_FD_COUNT; i++) {
        if (!fd_table[i].is_used) {
            fd_table[i] = fd_table[oldfd];
            if (fd_table[i].pty_num >= 0) {
                pty_master_dup(fd_table[i].pty_num);
            } else if (fd_table[i].node && fd_table[i].node->read == pty_slave_read) {
                pty_slave_open(fd_table[i].node, fd_table[i].flags);
            }
            if (fd_table[i].node && fd_table[i].node->type == VFS_PIPE) {
                struct pipe *p = (struct pipe *)fd_table[i].node->device;
                if (p) {
                    if (fd_table[i].node == &p->read_node) p->readers++;
                    else if (fd_table[i].node == &p->write_node) p->writers++;
                }
            }
            if (!fd_table[i].node && fd_table[i].sock_alias >= 0) {
                sys_sock_incref(fd_table[i].sock_alias);
            }
            return i;
        }
    }
    return -1;
}

int vfs_dup2(int oldfd, int newfd) {
    /* dup2 from a numeric socket fd installs an alias. */
    if (is_socket_fd(oldfd)) {
        struct file_descriptor *fd_table = cur_fds();
        if (newfd < 0 || newfd >= MAX_FD_COUNT) return -1;
        if (oldfd == newfd) return newfd;
        if (fd_table[newfd].is_used) {
            vfs_close(newfd);
        }
        fd_table[newfd].node = NULL;
        fd_table[newfd].offset = 0;
        fd_table[newfd].flags = O_RDWR;
        fd_table[newfd].is_used = 1;
        fd_table[newfd].sock_alias = oldfd - SOCKET_FD_BASE;
        fd_table[newfd].pty_num = -1;
        sys_sock_incref(oldfd - SOCKET_FD_BASE);
        return newfd;
    }
    struct file_descriptor *fd_table = cur_fds();
    if (oldfd < 0 || oldfd >= MAX_FD_COUNT || !fd_table[oldfd].is_used) return -1;
    if (newfd < 0 || newfd >= MAX_FD_COUNT) return -1;
    if (oldfd == newfd) return newfd;
    if (fd_table[newfd].is_used) {
        vfs_close(newfd);
    }
    fd_table[newfd] = fd_table[oldfd];
    if (fd_table[newfd].pty_num >= 0) {
        pty_master_dup(fd_table[newfd].pty_num);
    } else if (fd_table[newfd].node && fd_table[newfd].node->read == pty_slave_read) {
        pty_slave_open(fd_table[newfd].node, fd_table[newfd].flags);
    }
    if (fd_table[newfd].node && fd_table[newfd].node->type == VFS_PIPE) {
        struct pipe *p = (struct pipe *)fd_table[newfd].node->device;
        if (p) {
            if (fd_table[newfd].node == &p->read_node) p->readers++;
            else if (fd_table[newfd].node == &p->write_node) p->writers++;
        }
    }
    if (!fd_table[newfd].node && fd_table[newfd].sock_alias >= 0) {
        sys_sock_incref(fd_table[newfd].sock_alias);
    }
    return newfd;
}

/* True when fd refers to a character device (our tty surface). */
int vfs_is_tty(int fd) {
    struct file_descriptor *fd_table = cur_fds();
    if (fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used) return 0;
    if (fd_table[fd].pty_num >= 0) return 1;
    if (!fd_table[fd].node) return 0;
    return fd_table[fd].node->type == VFS_CHARDEVICE;
}

int vfs_pty_master_num(int fd) {
    struct file_descriptor *fd_table = cur_fds();
    if (fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used) return -1;
    return fd_table[fd].pty_num;
}

int vfs_pty_slave_num(int fd) {
    struct file_descriptor *fd_table = cur_fds();
    if (fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used) return -1;
    struct vfs_node *n = fd_table[fd].node;
    if (!n || n->read != pty_slave_read) return -1;
    struct pty_pair *p = (struct pty_pair *)n->device;
    if (!p) return -1;
    return (int)p->num;
}

/* Positional I/O (pread/pwrite) only makes sense on seekable objects.
 * Pipes, sockets, the console tty and PTY ends have no position: Linux
 * answers ESPIPE instead of blocking or moving a phantom offset. */
int vfs_is_seekable(int fd) {
    struct file_descriptor *fd_table = cur_fds();
    struct file_descriptor *f;
    if (fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used)
        return 0;
    f = &fd_table[fd];
    if (f->pty_num >= 0)
        return 0; /* PTY master */
    if (f->sock_alias >= 0)
        return 0;
    if (!f->node)
        return 0; /* raw socket fd or similar */
    if (f->node->type == VFS_PIPE)
        return 0;
    if (f->node->read == stdin_read)
        return 0; /* console tty */
    if (f->node->read == pty_slave_read)
        return 0;
    return 1;
}

/* Readiness for poll/select on non-socket descriptors. */
int vfs_poll(int fd, int events) {    struct file_descriptor *fd_table = cur_fds();    if (fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used) {
        return POLLNVAL;
    }
    struct file_descriptor *f = &fd_table[fd];
    if (f->pty_num >= 0) {
        struct pty_pair *pp = pty_get(f->pty_num);
        if (!pp) return POLLNVAL;
        int re = 0;
        if ((events & POLLIN) && (pp->s2m_count > 0 || pp->s2m_writers <= 0)) re |= POLLIN;
        if (pp->s2m_writers <= 0) re |= POLLHUP;
        if ((events & POLLOUT) && pp->m2s_readers > 0 && pp->m2s_count < PTY_BUF_SIZE) re |= POLLOUT;
        if (pp->m2s_readers <= 0) re |= POLLERR;
        return re & events;
    }
    if (f->node && f->node->read == pty_slave_read) {
        struct pty_pair *pp = (struct pty_pair *)f->node->device;
        if (!pp) return POLLNVAL;
        int re = 0;
        if ((events & POLLIN) && (pp->m2s_count > 0 || pp->m2s_writers <= 0)) re |= POLLIN;
        if (pp->m2s_writers <= 0) re |= POLLHUP;
        if ((events & POLLOUT) && pp->s2m_readers > 0 && pp->s2m_count < PTY_BUF_SIZE) re |= POLLOUT;
        if (pp->s2m_readers <= 0) re |= POLLERR;
        return re & events;
    }
    if (!f->node) {
        int sfd = vfs_sock_fd(fd);
        if (sfd < 0) return POLLNVAL;
        return sys_sock_poll(sfd, events);
    }

    /* Pipes: readable with data (or EOF), writable with space. */
    if (f->node->type == VFS_PIPE) {
        struct pipe *p = (struct pipe *)f->node->device;
        int revents = 0;
        if (p) {
            if (f->node == &p->read_node) {
                if (p->count > 0 || p->writers <= 0) revents |= POLLIN;
            } else {
                if (p->readers <= 0) revents |= POLLERR;
                else if (p->count < PIPE_BUF_SIZE) revents |= POLLOUT;
            }
        }
        return revents & events;
    }

    /* evdev nodes: readable while their own ring holds events. Routed here so
     * a compositor polling /dev/input/event* wakes for motion but the console
     * fd never confuses mouse traffic with keystrokes. */
    if (f->node->read == evdev_read) {
        return evdev_poll(f->node, events);
    }

    /* Console stdin: readable only while a keystroke is waiting. */
    if (f->node->read == stdin_read) {
        return vfs_tty_poll(events);
    }

    /* Regular files, directories, stdout/stderr: always ready. */
    return events & (POLLIN | POLLOUT);
}

static int64_t pipe_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)offset;
    if (!node || !buf) return -14; /* -EFAULT */
    struct pipe *p = (struct pipe *)node->device;
    if (!p) return -1;

    /* Blocking read: writers still open means more data may come.
     * Spins on pause like console stdin; ^C escapes via -EINTR. */
    while (p->count == 0) {
        if (p->writers <= 0) return 0; /* EOF */
        tty_poll_signal();
        if (has_unblocked_pending()) return -4; /* -EINTR */
        __asm__ volatile("pause");
    }

    uint8_t *dst = (uint8_t *)buf;
    size_t to_read = size < p->count ? size : p->count;
    for (size_t i = 0; i < to_read; i++) {
        dst[i] = p->buffer[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
    }
    p->count -= (uint32_t)to_read;
    return (int64_t)to_read;
}

static int64_t pipe_write(struct vfs_node *node, uint64_t offset, size_t size,
                           const void *buf) {
    (void)offset;
    if (!node || !buf) return -14;
    struct pipe *p = (struct pipe *)node->device;
    if (!p) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;
    while (done < size) {
        if (p->readers <= 0)
            return done ? (int64_t)done : -32; /* -EPIPE */
        while (p->count >= PIPE_BUF_SIZE) {
            if (p->readers <= 0) return done ? (int64_t)done : -32;
            tty_poll_signal();
            if (has_unblocked_pending()) return done ? (int64_t)done : -4;
            __asm__ volatile("pause");
        }
        size_t space = PIPE_BUF_SIZE - p->count;
        size_t chunk = (size - done) < space ? (size - done) : space;
        for (size_t i = 0; i < chunk; i++) {
            p->buffer[p->head] = src[done + i];
            p->head = (p->head + 1) % PIPE_BUF_SIZE;
        }
        p->count += (uint32_t)chunk;
        done += chunk;
    }
    return (int64_t)done;
}


static int pipe_close(struct vfs_node *node) {
    if (!node) return 0;
    struct pipe *p = (struct pipe *)node->device;
    if (!p) return 0;

    if (node == &p->read_node) {
        p->readers--;
    } else if (node == &p->write_node) {
        p->writers--;
    }

    if (p->readers <= 0 && p->writers <= 0) {
        if (p->buffer) kfree(p->buffer);
        kfree(p);
    }
    return 0;
}

int vfs_pipe(int pipefd[2]) {
    struct file_descriptor *fd_table = cur_fds();
    if (!pipefd) return -14; /* -EFAULT */

    int rfd = -1, wfd = -1;
    for (int i = 0; i < MAX_FD_COUNT; i++) {
        if (!fd_table[i].is_used) {
            if (rfd == -1) {
                rfd = i;
            } else if (wfd == -1) {
                wfd = i;
                break;
            }
        }
    }

    if (rfd == -1 || wfd == -1) return -24; /* -EMFILE */

    struct pipe *p = (struct pipe *)kzalloc(sizeof(struct pipe));
    if (!p) return -12; /* -ENOMEM */

    p->buffer = (uint8_t *)kmalloc(PIPE_BUF_SIZE);
    if (!p->buffer) {
        kfree(p);
        return -12;
    }

    p->head = 0;
    p->tail = 0;
    p->count = 0;
    p->readers = 1;
    p->writers = 1;

    kstrncpy(p->read_node.name, "pipe:[read]", sizeof(p->read_node.name));
    p->read_node.type = VFS_PIPE;
    p->read_node.flags = O_RDONLY;
    p->read_node.device = p;
    p->read_node.read = pipe_read;
    p->read_node.write = NULL;
    p->read_node.close = pipe_close;

    kstrncpy(p->write_node.name, "pipe:[write]", sizeof(p->write_node.name));
    p->write_node.type = VFS_PIPE;
    p->write_node.flags = O_WRONLY;
    p->write_node.device = p;
    p->write_node.read = NULL;
    p->write_node.write = pipe_write;
    p->write_node.close = pipe_close;

    fd_table[rfd].node = &p->read_node;
    fd_table[rfd].offset = 0;
    fd_table[rfd].flags = O_RDONLY;
    fd_table[rfd].is_used = 1;
    fd_table[rfd].sock_alias = -1;
    fd_table[rfd].pty_num = -1;

    fd_table[wfd].node = &p->write_node;
    fd_table[wfd].offset = 0;
    fd_table[wfd].flags = O_WRONLY;
    fd_table[wfd].is_used = 1;
    fd_table[wfd].sock_alias = -1;
    fd_table[wfd].pty_num = -1;

    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}

/* Duplicate a descriptor table for a fork child. Shared pipe ends gain an
 * extra reader/writer reference so one side closing never breaks the other. */
void vfs_clone_table(struct file_descriptor *dst, struct file_descriptor *src) {
    if (!dst || !src) return;
    for (int i = 0; i < MAX_FD_COUNT; i++) {
        dst[i] = src[i];
        if (dst[i].is_used && dst[i].pty_num >= 0) {
            pty_master_dup(dst[i].pty_num);
        } else if (dst[i].is_used && dst[i].node && dst[i].node->read == pty_slave_read) {
            pty_slave_open(dst[i].node, dst[i].flags);
        }
        if (dst[i].is_used && dst[i].node && dst[i].node->type == VFS_PIPE) {
            struct pipe *p = (struct pipe *)dst[i].node->device;
            if (p) {
                if (dst[i].node == &p->read_node) p->readers++;
                else if (dst[i].node == &p->write_node) p->writers++;
            }
        }
        if (dst[i].is_used && !dst[i].node && dst[i].sock_alias >= 0) {
            sys_sock_incref(dst[i].sock_alias);
        }
    }
}

/* Release every descriptor in a table (process exit path). */
void vfs_close_all(struct file_descriptor *table) {
    if (!table) return;
    for (int i = 0; i < MAX_FD_COUNT; i++) {
        if (!table[i].is_used) continue;
        if (table[i].pty_num >= 0) {
            pty_master_close(table[i].pty_num);
            table[i].pty_num = -1;
        }
        if (!table[i].node) {
            if (table[i].sock_alias >= 0) {
                sys_sock_close(SOCKET_FD_BASE + table[i].sock_alias);
            }
        } else if (table[i].node->close) {
            table[i].node->close(table[i].node);
        }
        table[i].node = NULL;
        table[i].offset = 0;
        table[i].flags = 0;
        table[i].is_used = 0;
        table[i].sock_alias = -1;
        table[i].pty_num = -1;
    }
}

/* ── Minimal devfs (null/zero/full/random/urandom/tty), mounted at /dev ── */

static int64_t devnull_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    (void)offset;
    (void)size;
    (void)buf;
    return 0; /* EOF */
}

static int64_t devnull_write(struct vfs_node *node, uint64_t offset, size_t size, const void *buf) {
    (void)node;
    (void)offset;
    (void)buf;
    return (int64_t)size; /* discard */
}

static int64_t devzero_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node; (void)offset;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < size; i++) dst[i] = 0;
    return (int64_t)size;
}

static int64_t devfull_write(struct vfs_node *node, uint64_t offset, size_t size, const void *buf) {
    (void)node;
    (void)offset;
    (void)size;
    (void)buf;
    return -28; /* -ENOSPC */
}

/* Fallback RNG (xorshift64, seeded from TSC). RDRAND is preferred below. */
static uint64_t rng_state;

static void rng_seed(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    rng_state = (((uint64_t)hi << 32) | lo) ^ 0x9E3779B97F4A7C15ULL;
    if (!rng_state) rng_state = 0x9E3779B97F4A7C15ULL;
}

static uint64_t rng_next(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static int rdrand_supported(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    (void)a;
    (void)b;
    (void)d;
    return (c & (1U << 30)) != 0;
}

static int use_rdrand = 0;

static uint64_t rng_word(void) {
    uint64_t v = rng_next();
    if (use_rdrand) {
        uint64_t hw = 0;
        uint8_t ok = 0;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(hw), "=qm"(ok));
        if (ok) v = hw;
    }
    return v;
}

/* Fill a buffer with random bytes (RDRAND, xorshift fallback). */
void rng_fill(void *buf, size_t len) {
    uint8_t *dst = (uint8_t *)buf;
    while (len >= 8) {
        uint64_t v = rng_word();
        for (int i = 0; i < 8; i++) dst[i] = (uint8_t)(v >> (i * 8));
        dst += 8;
        len -= 8;
    }
    if (len > 0) {
        uint64_t v = rng_word();
        for (size_t i = 0; i < len; i++) dst[i] = (uint8_t)(v >> (i * 8));
    }
}

static int64_t devrand_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node; (void)offset;
    rng_fill(buf, size);
    return (int64_t)size;
}

/* /dev/mem: read-only physical memory window for diagnostics.
 * offset == physical address, capped at total RAM. Each page is
 * present-checked through the current tables (HHDM lives in the shared
 * higher half), so probing reserved holes stops the read instead of
 * faulting the kernel. */
static int devmem_page_present(uint64_t phys) {
    uint64_t hhdm = vmm_get_hhdm_offset();
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t virt = hhdm + (phys & ~(uint64_t)0xFFFULL);
    uint64_t *tab = (uint64_t *)(hhdm + (cr3 & PAGE_ADDR_MASK));
    const uint64_t shifts[4] = { 39, 30, 21, 12 };
    for (int level = 0; level < 4; level++) {
        uint64_t e = tab[(virt >> shifts[level]) & 0x1FF];
        if (!(e & PAGE_PRESENT)) return 0;
        if (level < 3 && (e & PAGE_HUGE)) return 1; /* 1GB / 2MB page */
        if (level == 3) return 1;
        tab = (uint64_t *)(hhdm + (e & PAGE_ADDR_MASK));
    }
    return 0;
}

static int64_t devmem_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    uint64_t total = pmm_get_total_memory();
    if (offset >= total) return 0;
    if (offset + size > total) size = (size_t)(total - offset);
    uint64_t hhdm = vmm_get_hhdm_offset();
    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0;
    while (done < size) {
        uint64_t phys = offset + done;
        uint64_t page_end = (phys & ~(uint64_t)0xFFFULL) + 0x1000;
        size_t chunk = size - done;
        if (phys + chunk > page_end) chunk = (size_t)(page_end - phys);
        if (!devmem_page_present(phys)) break;
        const uint8_t *src = (const uint8_t *)(hhdm + phys);
        for (size_t i = 0; i < chunk; i++) dst[done + i] = src[i];
        done += chunk;
    }
    return (int64_t)done;
}

/* PTY master open handler */
static int ptmx_open(struct vfs_node *node, int flags) {
    (void)node; (void)flags;
    int num;
    int ret = pty_master_open(&num);
    if (ret < 0) return ret;
    /* Return 200 + pty_num to signal vfs_open this is a PTY master fd */
    return 200 + num;
}

static int64_t ptmx_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node; (void)offset; (void)size; (void)buf;
    return -1;
}

static int64_t ptmx_write(struct vfs_node *node, uint64_t offset, size_t size, const void *buf) {
    (void)node; (void)offset; (void)size; (void)buf;
    return -1;
}

#define DEVFS_COUNT 9

static struct vfs_node devfs_root_node;
static struct vfs_node devfs_nodes[DEVFS_COUNT];
static struct vfs_node devfs_pts_dir;

/* Externally-owned device nodes (fb0, evdev). devfs holds POINTERS rather
 * than copies so the driver's own object stays the live node: drivers that
 * route behaviour on the node pointer (evdev has two) keep working. */
#define DEVFS_EXT_COUNT 8
static struct vfs_node *devfs_ext[DEVFS_EXT_COUNT];

int devfs_attach(struct vfs_node *node) {
    if (!node) return -22; /* -EINVAL */
    if (!devfs_root_node.readdir) return -19; /* devfs_root() has not run */
    for (int i = 0; i < DEVFS_EXT_COUNT; i++) {
        if (devfs_ext[i] == node) return 0;  /* already published */
        if (!devfs_ext[i]) { devfs_ext[i] = node; return 0; }
    }
    return -28; /* -ENOSPC */
}

static struct vfs_node *devfs_finddir(struct vfs_node *node, const char *name) {
    (void)node;
    if (kstrcmp(name, "pts") == 0) return &devfs_pts_dir;
    for (int i = 0; i < DEVFS_COUNT; i++) {
        if (kstrcmp(devfs_nodes[i].name, name) == 0) return &devfs_nodes[i];
    }
    for (int i = 0; i < DEVFS_EXT_COUNT; i++) {
        if (devfs_ext[i] && kstrcmp(devfs_ext[i]->name, name) == 0)
            return devfs_ext[i];
    }
    return NULL;
}

static struct vfs_dirent *devfs_readdir(struct vfs_node *node, uint32_t index) {
    static struct vfs_dirent dent;
    (void)node;
    if (index < DEVFS_COUNT) {
        kstrncpy(dent.name, devfs_nodes[index].name, sizeof(dent.name));
        dent.ino = devfs_nodes[index].inode;
        dent.type = VFS_CHARDEVICE;
        dent.size = 0;
        return &dent;
    }
    if (index == DEVFS_COUNT) {
        kstrncpy(dent.name, "pts", sizeof(dent.name));
        dent.ino = 11;
        dent.type = VFS_DIRECTORY;
        dent.size = 0;
        return &dent;
    }
    uint32_t ei = index - (DEVFS_COUNT + 1);
    if (ei < DEVFS_EXT_COUNT && devfs_ext[ei]) {
        kstrncpy(dent.name, devfs_ext[ei]->name, sizeof(dent.name));
        dent.ino = devfs_ext[ei]->inode;
        dent.type = devfs_ext[ei]->type;
        dent.size = devfs_ext[ei]->size;
        return &dent;
    }
    return NULL;
}

static struct vfs_node *devfs_add(const char *name, uint64_t ino,
        int64_t (*read)(struct vfs_node *, uint64_t, size_t, void *),
        int64_t (*write)(struct vfs_node *, uint64_t, size_t, const void *)) {
    for (int i = 0; i < DEVFS_COUNT; i++) {
        if (devfs_nodes[i].name[0] == '\0') {
            kstrncpy(devfs_nodes[i].name, name, sizeof(devfs_nodes[i].name));
            devfs_nodes[i].type = VFS_CHARDEVICE;
            devfs_nodes[i].permissions = 0666;
            devfs_nodes[i].inode = ino;
            devfs_nodes[i].read = read;
            devfs_nodes[i].write = write;
            return &devfs_nodes[i];
        }
    }
    return NULL;
}

struct vfs_node *devfs_root(void) {
    static int done = 0;
    if (done) return &devfs_root_node;
    done = 1;

    rng_seed();
    use_rdrand = rdrand_supported();

    kstrncpy(devfs_root_node.name, "dev", sizeof(devfs_root_node.name));
    devfs_root_node.type = VFS_DIRECTORY;
    devfs_root_node.permissions = 0755;
    devfs_root_node.inode = 2;
    devfs_root_node.finddir = devfs_finddir;
    devfs_root_node.readdir = devfs_readdir;

    devfs_add("null", 3, devnull_read, devnull_write);
    devfs_add("zero", 4, devzero_read, devnull_write);
    devfs_add("full", 5, devzero_read, devfull_write);
    devfs_add("random", 6, devrand_read, devnull_write);
    devfs_add("urandom", 7, devrand_read, devnull_write);
    devfs_add("tty", 8, stdin_read, stdout_write);
    devfs_add("console", 9, stdin_read, stdout_write);
    struct vfs_node *ptmx = devfs_add("ptmx", 10, ptmx_read, ptmx_write);
    if (ptmx) ptmx->open = ptmx_open;
    struct vfs_node *mem = devfs_add("mem", 12, devmem_read, NULL);
    if (mem) mem->permissions = 0400;

    kstrncpy(devfs_pts_dir.name, "pts", sizeof(devfs_pts_dir.name));
    devfs_pts_dir.type = VFS_DIRECTORY;
    devfs_pts_dir.permissions = 0755;
    devfs_pts_dir.inode = 11;

    serial_puts("[DEVFS] Ready (null zero full random urandom tty console ptmx pts mem)");
    serial_puts(use_rdrand ? " [RDRAND]\n" : " [xorshift]\n");
    return &devfs_root_node;
}

/* ── Minimal tmpfs (writable RAM filesystem, mounted at /tmp) ──────────── */

#define TMPFS_MAX_FILE (1024 * 1024)

struct tmpfs_file {
    uint8_t *data;
    size_t   cap;
};

struct tmpfs_dir_data {
    struct vfs_node **children;
    uint32_t count;
    uint32_t capacity;
};

static int64_t tmpfs_file_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    struct tmpfs_file *f = (struct tmpfs_file *)node->device;
    if (!f || offset >= node->size) return 0;
    if (offset + size > node->size) size = (size_t)(node->size - offset);
    const uint8_t *src = f->data + offset;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < size; i++) dst[i] = src[i];
    return (int64_t)size;
}

static int64_t tmpfs_file_write(struct vfs_node *node, uint64_t offset, size_t size, const void *buf) {
    struct tmpfs_file *f = (struct tmpfs_file *)node->device;
    if (!f || !buf) return -14; /* -EFAULT */
    if (offset + size > TMPFS_MAX_FILE) return -27; /* -EFBIG */
    if (offset + size > f->cap) {
        size_t ncap = f->cap ? f->cap : 4096;
        while (ncap < offset + size) ncap *= 2;
        if (ncap > TMPFS_MAX_FILE) ncap = TMPFS_MAX_FILE;
        uint8_t *ndata = (uint8_t *)kmalloc(ncap);
        if (!ndata) return -12; /* -ENOMEM */
        for (size_t i = 0; i < node->size; i++) ndata[i] = f->data[i];
        if (f->data) kfree(f->data);
        f->data = ndata;
        f->cap = ncap;
    }
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < size; i++) f->data[offset + i] = src[i];
    if (offset + size > node->size) node->size = offset + size;
    return (int64_t)size;
}

static int tmpfs_truncate(struct vfs_node *node, int64_t length) {
    (void)length;
    struct tmpfs_file *f = (struct tmpfs_file *)node->device;
    if (!f) return -22;
    if (f->data) kfree(f->data);
    f->data = NULL;
    f->cap  = 0;
    node->size = 0;
    return 0;
}

static struct vfs_node *tmpfs_dir_finddir(struct vfs_node *node, const char *name) {
    struct tmpfs_dir_data *dir = (struct tmpfs_dir_data *)node->device;
    if (!dir) return NULL;
    for (uint32_t i = 0; i < dir->count; i++) {
        if (kstrcmp(dir->children[i]->name, name) == 0) {
            return dir->children[i];
        }
    }
    return NULL;
}

static struct vfs_dirent *tmpfs_dir_readdir(struct vfs_node *node, uint32_t index) {
    static struct vfs_dirent dent;
    struct tmpfs_dir_data *dir = (struct tmpfs_dir_data *)node->device;
    if (!dir || index >= dir->count) return NULL;
    struct vfs_node *child = dir->children[index];
    kstrncpy(dent.name, child->name, sizeof(dent.name));
    dent.ino = child->inode;
    dent.type = child->type;
    dent.size = child->size;
    return &dent;
}

static void tmpfs_dir_add(struct vfs_node *parent, struct vfs_node *child) {
    struct tmpfs_dir_data *dir = (struct tmpfs_dir_data *)parent->device;
    if (!dir) return;
    if (dir->count >= dir->capacity) {
        uint32_t ncap = dir->capacity == 0 ? 8 : dir->capacity * 2;
        struct vfs_node **nch = (struct vfs_node **)kmalloc(ncap * sizeof(struct vfs_node *));
        if (!nch) return;
        for (uint32_t i = 0; i < dir->count; i++) nch[i] = dir->children[i];
        if (dir->children) kfree(dir->children);
        dir->children = nch;
        dir->capacity = ncap;
    }
    dir->children[dir->count++] = child;
}

static uint64_t tmpfs_inode = 1000;

static int tmpfs_unlink(struct vfs_node *parent, const char *name);
static int tmpfs_rmdir(struct vfs_node *parent, const char *name);
static int tmpfs_mksock(struct vfs_node *parent, const char *name, uint16_t mode);

static int tmpfs_create(struct vfs_node *parent, const char *name, uint16_t mode) {
    if (!parent || !name || parent->type != VFS_DIRECTORY) return -22;
    if (tmpfs_dir_finddir(parent, name)) return -17; /* -EEXIST */
    struct vfs_node *f = (struct vfs_node *)kzalloc(sizeof(struct vfs_node));
    if (!f) return -12;
    struct tmpfs_file *tf = (struct tmpfs_file *)kzalloc(sizeof(struct tmpfs_file));
    if (!tf) {
        kfree(f);
        return -12;
    }
    kstrncpy(f->name, name, sizeof(f->name));
    f->type = VFS_FILE;
    f->permissions = mode ? (mode & 0777) : 0644;
    f->inode = tmpfs_inode++;
    f->device = tf;
    f->read = tmpfs_file_read;
    f->write = tmpfs_file_write;
    f->truncate = tmpfs_truncate;
    tmpfs_dir_add(parent, f);
    return 0;
}

static int tmpfs_mkdir(struct vfs_node *parent, const char *name, uint16_t mode) {
    if (!parent || !name || parent->type != VFS_DIRECTORY) return -22;
    if (tmpfs_dir_finddir(parent, name)) return -17; /* -EEXIST */
    struct vfs_node *d = (struct vfs_node *)kzalloc(sizeof(struct vfs_node));
    if (!d) return -12;
    struct tmpfs_dir_data *dd = (struct tmpfs_dir_data *)kzalloc(sizeof(struct tmpfs_dir_data));
    if (!dd) {
        kfree(d);
        return -12;
    }
    kstrncpy(d->name, name, sizeof(d->name));
    d->type = VFS_DIRECTORY;
    d->permissions = mode ? (mode & 0777) : 0755;
    d->inode = tmpfs_inode++;
    d->device = dd;
    d->finddir = tmpfs_dir_finddir;
    d->readdir = tmpfs_dir_readdir;
    d->create = tmpfs_create;
    d->mkdir = tmpfs_mkdir;
    d->mksock = tmpfs_mksock;
    d->unlink = tmpfs_unlink;
    d->rmdir = tmpfs_rmdir;
    tmpfs_dir_add(parent, d);
    return 0;
}

static int tmpfs_mksock(struct vfs_node *parent, const char *name, uint16_t mode) {
    if (!parent || !name || parent->type != VFS_DIRECTORY) return -22;
    if (tmpfs_dir_finddir(parent, name)) return -17; /* -EEXIST */
    struct vfs_node *s = (struct vfs_node *)kzalloc(sizeof(struct vfs_node));
    if (!s) return -12;
    kstrncpy(s->name, name, sizeof(s->name));
    s->type = VFS_SOCKET;
    s->permissions = mode ? (mode & 0777) : 0755;
    s->inode = tmpfs_inode++;
    s->device = NULL;
    tmpfs_dir_add(parent, s);
    return 0;
}

static int tmpfs_unlink(struct vfs_node *parent, const char *name) {
    if (!parent || !name || parent->type != VFS_DIRECTORY) return -22;
    struct tmpfs_dir_data *dir = (struct tmpfs_dir_data *)parent->device;
    if (!dir) return -22;
    for (uint32_t i = 0; i < dir->count; i++) {
        if (kstrcmp(dir->children[i]->name, name) == 0) {
            struct vfs_node *victim = dir->children[i];
            if (victim->type == VFS_DIRECTORY) return -21; /* -EISDIR: cannot unlink directory */
            struct tmpfs_file *vf = (struct tmpfs_file *)victim->device;
            if (vf) {
                if (vf->data) kfree(vf->data);
                kfree(vf);
            }
            for (uint32_t k = i + 1; k < dir->count; k++) {
                dir->children[k - 1] = dir->children[k];
            }
            dir->count--;
            kfree(victim);
            return 0;
        }
    }
    return -2; /* -ENOENT */
}

static int tmpfs_rmdir(struct vfs_node *parent, const char *name) {
    if (!parent || !name || parent->type != VFS_DIRECTORY) return -22;
    struct tmpfs_dir_data *dir = (struct tmpfs_dir_data *)parent->device;
    if (!dir) return -22;
    for (uint32_t i = 0; i < dir->count; i++) {
        if (kstrcmp(dir->children[i]->name, name) == 0) {
            struct vfs_node *victim = dir->children[i];
            if (victim->type != VFS_DIRECTORY) return -20; /* -ENOTDIR: cannot rmdir non-directory */
            struct tmpfs_dir_data *vd = (struct tmpfs_dir_data *)victim->device;
            if (vd && vd->count > 0) return -39; /* -ENOTEMPTY */
            if (vd) {
                if (vd->children) kfree(vd->children);
                kfree(vd);
            }
            for (uint32_t k = i + 1; k < dir->count; k++) {
                dir->children[k - 1] = dir->children[k];
            }
            dir->count--;
            kfree(victim);
            return 0;
        }
    }
    return -2; /* -ENOENT */
}

static struct vfs_node tmpfs_root_node;

struct vfs_node *tmpfs_new_root(const char *name) {
    struct vfs_node *root = (struct vfs_node *)kzalloc(sizeof(struct vfs_node));
    if (!root) return NULL;
    struct tmpfs_dir_data *dd = (struct tmpfs_dir_data *)kzalloc(sizeof(struct tmpfs_dir_data));
    if (!dd) {
        kfree(root);
        return NULL;
    }
    kstrncpy(root->name, name ? name : "tmpfs", sizeof(root->name));
    root->type = VFS_DIRECTORY;
    root->permissions = 0777;
    root->inode = tmpfs_inode++;
    root->device = dd;
    root->finddir = tmpfs_dir_finddir;
    root->readdir = tmpfs_dir_readdir;
    root->create = tmpfs_create;
    root->mkdir = tmpfs_mkdir;
    root->mksock = tmpfs_mksock;
    root->unlink = tmpfs_unlink;
    root->rmdir = tmpfs_rmdir;
    return root;
}

struct vfs_node *tmpfs_root(void) {
    static int done = 0;
    if (done) return &tmpfs_root_node;
    done = 1;

    struct tmpfs_dir_data *dd = (struct tmpfs_dir_data *)kzalloc(sizeof(struct tmpfs_dir_data));
    kstrncpy(tmpfs_root_node.name, "tmp", sizeof(tmpfs_root_node.name));
    tmpfs_root_node.type = VFS_DIRECTORY;
    tmpfs_root_node.permissions = 0777;
    tmpfs_root_node.inode = tmpfs_inode++;
    tmpfs_root_node.device = dd;
    tmpfs_root_node.finddir = tmpfs_dir_finddir;
    tmpfs_root_node.readdir = tmpfs_dir_readdir;
    tmpfs_root_node.create = tmpfs_create;
    tmpfs_root_node.mkdir = tmpfs_mkdir;
    tmpfs_root_node.mksock = tmpfs_mksock;
    tmpfs_root_node.unlink = tmpfs_unlink;
    tmpfs_root_node.rmdir = tmpfs_rmdir;

    serial_puts("[TMPFS] Ready (mounted at /tmp)\n");
    return &tmpfs_root_node;
}

/* Resize a regular file, zero-filling any growth. */
static int do_ftruncate(struct vfs_node *node, int64_t length) {
    if (!node || node->type == VFS_DIRECTORY) return -22;
    if (node->type != VFS_FILE) return -22;
    if (length < 0) return -22;
    if (!node->write) return -30; /* -EROFS */
    if ((uint64_t)length > TMPFS_MAX_FILE) return -27; /* -EFBIG */

    if (node->write == tmpfs_file_write) {
        /* tmpfs fast path: grow or shrink to arbitrary length. */
        struct tmpfs_file *f = (struct tmpfs_file *)node->device;
        if (!f) return -22;
        if ((uint64_t)length > f->cap) {
            size_t ncap = f->cap ? f->cap : 4096;
            while (ncap < (uint64_t)length) ncap *= 2;
            if (ncap > TMPFS_MAX_FILE) ncap = TMPFS_MAX_FILE;
            uint8_t *ndata = (uint8_t *)kmalloc(ncap);
            if (!ndata) return -12;
            for (size_t i = 0; i < node->size; i++) ndata[i] = f->data[i];
            if (f->data) kfree(f->data);
            f->data = ndata;
            f->cap  = ncap;
        }
        if (!f->data && length > 0) return -12;
        for (uint64_t i = node->size; i < (uint64_t)length; i++) f->data[i] = 0;
        node->size = (uint64_t)length;
        return 0;
    }

    /* Generic path: delegate to fs-specific truncate (tarfs COW, etc). */
    if (node->truncate) return node->truncate(node, length);
    return -22;
}

int vfs_ftruncate(int fd, int64_t length) {
    struct file_descriptor *fd_table = cur_fds();
    if (fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used) return -9; /* -EBADF */
    return do_ftruncate(fd_table[fd].node, length);
}

int vfs_truncate(const char *path, int64_t length) {
    if (!path) return -14; /* -EFAULT */
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -2; /* -ENOENT */
    return do_ftruncate(node, length);
}

int vfs_unlink(const char *path) {
    if (!path) return -14;
    char abs[512];
    vfs_abspath(path, abs, sizeof(abs));
    if (abs[0] == '\0') return -36;
    path = abs;

    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -2; /* -ENOENT */
    if (node->type == VFS_DIRECTORY) return -21; /* -EISDIR */

    char dir[256];
    const char *leaf = path;
    for (const char *s = path; *s; s++) {
        if (*s == '/' && *(s + 1) != '\0') leaf = s + 1;
    }
    size_t dirlen = (size_t)(leaf - path);
    if (dirlen <= 1) {
        dir[0] = '/';
        dir[1] = '\0';
    } else {
        if (dirlen > sizeof(dir)) return -36;
        for (size_t i = 0; i < dirlen - 1; i++) dir[i] = path[i];
        dir[dirlen - 1] = '\0';
    }
    struct vfs_node *parent = vfs_lookup(dir);
    if (!parent || parent->type != VFS_DIRECTORY) return -2;
    if (!parent->unlink) return -30; /* -EROFS */
    return parent->unlink(parent, leaf);
}

int vfs_rmdir(const char *path) {
    if (!path) return -14;
    char abs[512];
    vfs_abspath(path, abs, sizeof(abs));
    if (abs[0] == '\0') return -36;
    path = abs;

    if (kstrcmp(path, "/") == 0) return -16; /* -EBUSY */

    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -2; /* -ENOENT */
    if (node->type != VFS_DIRECTORY) return -20; /* -ENOTDIR */

    char dir[256];
    const char *leaf = path;
    for (const char *s = path; *s; s++) {
        if (*s == '/' && *(s + 1) != '\0') leaf = s + 1;
    }
    size_t dirlen = (size_t)(leaf - path);
    if (dirlen <= 1) {
        dir[0] = '/';
        dir[1] = '\0';
    } else {
        if (dirlen > sizeof(dir)) return -36;
        for (size_t i = 0; i < dirlen - 1; i++) dir[i] = path[i];
        dir[dirlen - 1] = '\0';
    }
    struct vfs_node *parent = vfs_lookup(dir);
    if (!parent || parent->type != VFS_DIRECTORY) return -2;
    if (!parent->rmdir) return -30; /* -EROFS */
    return parent->rmdir(parent, leaf);
}

int vfs_mkdir(const char *path, int mode) {
    if (!path) return -14;
    char abs[512];
    vfs_abspath(path, abs, sizeof(abs));
    if (abs[0] == '\0') return -36;
    path = abs;
    char dir[256];
    const char *leaf = path;
    for (const char *s = path; *s; s++) {
        if (*s == '/' && *(s + 1) != '\0') leaf = s + 1;
    }
    size_t dirlen = (size_t)(leaf - path);
    if (dirlen <= 1) {
        dir[0] = '/';
        dir[1] = '\0';
    } else {
        if (dirlen > sizeof(dir)) return -36;
        for (size_t i = 0; i < dirlen - 1; i++) dir[i] = path[i];
        dir[dirlen - 1] = '\0';
    }
    char base[128];
    size_t blen = 0;
    while (leaf[blen] && blen < sizeof(base) - 1) {
        base[blen] = leaf[blen];
        blen++;
    }
    base[blen] = '\0';
    if (base[0] == '\0') return -2;
    struct vfs_node *parent = vfs_lookup(dir);
    if (!parent || parent->type != VFS_DIRECTORY) return -2;
    if (!parent->mkdir) return -30; /* -EROFS */
    return parent->mkdir(parent, base, (uint16_t)mode);
}

int vfs_mksock(const char *path) {
    if (!path) return -14;
    char abs[512];
    vfs_abspath(path, abs, sizeof(abs));
    if (abs[0] == '\0') return -36;
    path = abs;
    char dir[256];
    const char *leaf = path;
    for (const char *s = path; *s; s++) {
        if (*s == '/' && *(s + 1) != '\0') leaf = s + 1;
    }
    size_t dirlen = (size_t)(leaf - path);
    if (dirlen <= 1) {
        dir[0] = '/';
        dir[1] = '\0';
    } else {
        if (dirlen > sizeof(dir)) return -36;
        for (size_t i = 0; i < dirlen - 1; i++) dir[i] = path[i];
        dir[dirlen - 1] = '\0';
    }
    char base[128];
    size_t blen = 0;
    while (leaf[blen] && blen < sizeof(base) - 1) {
        base[blen] = leaf[blen];
        blen++;
    }
    base[blen] = '\0';
    if (base[0] == '\0') return -2;
    struct vfs_node *parent = vfs_lookup(dir);
    if (!parent || parent->type != VFS_DIRECTORY) return -2;
    if (!parent->mksock) return -30; /* -EROFS */
    return parent->mksock(parent, base, 0755);
}

int vfs_chmod(const char *path, int mode) {
    if (!path) return -14;
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -2;
    node->permissions = (uint32_t)(mode & 07777);
    return 0;
}

int vfs_fchmod(int fd, int mode) {
    struct file_descriptor *fd_table = cur_fds();
    if (fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used) return -9; /* -EBADF */
    if (!fd_table[fd].node) return -2;
    fd_table[fd].node->permissions = (uint32_t)(mode & 07777);
    return 0;
}

/* ── Minimal procfs (for `top`, `tty`, `ps`) — mounted at /proc ────────── */

static int64_t proc_version_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    (void)offset;
    const char *s = "Linux version 0.1 Eigen Experimental (x86_64) #1 SMP PREEMPT Mon Sep 14 2026\n";
    size_t len = 0;
    while (s[len]) len++;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - (size_t)offset;
    const uint8_t *src = (const uint8_t *)s + offset;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < size; i++) dst[i] = src[i];
    return (int64_t)size;
}

static int64_t proc_uptime_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    uint64_t ticks = timer_get_ticks();
    uint64_t sec = ticks / 100;
    uint64_t cs = (ticks % 100) * 1;
    char tmp[64];
    int pos = 0;
    /* Format: "<sec>.<cs> <idle>\n" */
    char secbuf[32];
    int seclen = 0;
    uint64_t t = sec;
    if (t == 0) secbuf[seclen++] = '0';
    else {
        char rev[32];
        int r = 0;
        while (t > 0 && r < 30) { rev[r++] = '0' + (t % 10); t /= 10; }
        while (r > 0) secbuf[seclen++] = rev[--r];
    }
    for (int i = 0; i < seclen && pos < 60; i++) tmp[pos++] = secbuf[i];
    tmp[pos++] = '.';
    tmp[pos++] = '0' + (char)(cs % 10);
    tmp[pos++] = ' ';
    tmp[pos++] = '0';
    tmp[pos++] = '.';
    tmp[pos++] = '0';
    tmp[pos++] = '0';
    tmp[pos++] = '\n';
    size_t len = (size_t)pos;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - (size_t)offset;
    for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = (uint8_t)tmp[offset + i];
    return (int64_t)size;
}

static int64_t proc_meminfo_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    uint64_t total = pmm_get_total_memory() / 1024;
    uint64_t free = pmm_get_free_memory() / 1024;
    char tmp[256];
    int pos = 0;
    #define APPEND(s) do { const char *_s = (s); while (*_s && pos < 240) tmp[pos++] = *_s++; } while (0)
    #define APPEND_UL(v) do { char _b[24]; int _l = 0; uint64_t _v = (v); if (_v==0) _b[_l++]='0'; else { char _r[24]; int _ri=0; while(_v>0&&_ri<20){_r[_ri++]='0'+(_v%10);_v/=10;} while(_ri>0) _b[_l++]=_r[--_ri]; } for(int _i=0;_i<_l&&pos<240;_i++) tmp[pos++]=_b[_i]; } while(0)
    APPEND("MemTotal:       "); APPEND_UL(total); APPEND(" kB\n");
    APPEND("MemFree:        "); APPEND_UL(free); APPEND(" kB\n");
    APPEND("MemAvailable:   "); APPEND_UL(free); APPEND(" kB\n");
    APPEND("Buffers:              0 kB\n");
    APPEND("Cached:               0 kB\n");
    APPEND("SwapTotal:            0 kB\n");
    APPEND("SwapFree:             0 kB\n");
    #undef APPEND
    #undef APPEND_UL
    size_t len = (size_t)pos;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - (size_t)offset;
    for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = (uint8_t)tmp[offset + i];
    return (int64_t)size;
}

static int64_t proc_stat_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    const char *s = "cpu  0 0 0 0 0 0 0 0 0 0\n"
                    "cpu0 0 0 0 0 0 0 0 0 0 0\n"
                    "intr 0\n"
                    "ctxt 0\n"
                    "btime 0\n"
                    "processes 1\n"
                    "procs_running 1\n"
                    "procs_blocked 0\n";
    size_t len = 0;
    while (s[len]) len++;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - (size_t)offset;
    for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = (uint8_t)s[offset + i];
    return (int64_t)size;
}

static int64_t proc_interrupts_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    char tmp[512];
    int pos = 0;
    const char *chip = apic_available() ? "IO-APIC" : "XT-PIC";
    #define IPUT(s) do { const char *_s = (s); while (*_s && pos < 500) tmp[pos++] = *_s++; } while (0)
    #define IPUTU(v) do { char _r[12]; int _n = 0; uint64_t _v = (v); if (!_v) tmp[pos++] = '0'; else { while (_v && _n < 10) { _r[_n++] = '0' + (_v % 10); _v /= 10; } while (_n && pos < 500) tmp[pos++] = _r[--_n]; } } while (0)
    IPUT("           CPU0\n");
    for (uint8_t irq = 0; irq < 16 && pos < 460; irq++) {
        IPUT("  ");
        IPUTU(irq);
        IPUT(": ");
        IPUTU(idt_get_irq_count(irq));
        IPUT(" ");
        IPUT(chip);
        IPUT(irq == 0 ? "  timer\n" : irq == 1 ? "  keyboard\n" : irq == 2 ? "  cascade\n" : "\n");
    }
    #undef IPUT
    #undef IPUTU
    size_t len = (size_t)pos;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - (size_t)offset;
    for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = (uint8_t)tmp[offset + i];
    return (int64_t)size;
}

static void cpuid_leaf(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf));
}

static struct vfs_node proc_acpi_event_node;
static struct vfs_node proc_acpi_tab_nodes[4];

static int64_t proc_cpuinfo_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    char tmp[512];
    int pos = 0;
    uint32_t a, b, c, d;
    char vendor[13];
    cpuid_leaf(0, &a, &b, &c, &d);
    vendor[0] = (char)b; vendor[1] = (char)(b >> 8);
    vendor[2] = (char)(b >> 16); vendor[3] = (char)(b >> 24);
    vendor[4] = (char)d; vendor[5] = (char)(d >> 8);
    vendor[6] = (char)(d >> 16); vendor[7] = (char)(d >> 24);
    vendor[8] = (char)c; vendor[9] = (char)(c >> 8);
    vendor[10] = (char)(c >> 16); vendor[11] = (char)(c >> 24);
    vendor[12] = '\0';
    cpuid_leaf(1, &a, &b, &c, &d);
    uint32_t fam = ((a >> 8) & 0xF) + ((a >> 20) & 0xFF);
    uint32_t mod = ((a >> 4) & 0xF) | ((a >> 12) & 0xF0);
    uint32_t step = a & 0xF;
    uint32_t ncpu = acpi_cpu_count();
    if (!ncpu) ncpu = 1;
    if (ncpu > 8) ncpu = 8;
    for (uint32_t i = 0; i < ncpu && pos < 480; i++) {
        #define PUT(s) do { const char *_s = (s); while (*_s && pos < 500) tmp[pos++] = *_s++; } while (0)
        #define PUTU(v) do { char _r[12]; int _n = 0; uint32_t _v = (v); if (!_v) _r[_n++] = '0'; else { char _t[12]; int _m = 0; while (_v && _m < 10) { _t[_m++] = '0' + (_v % 10); _v /= 10; } while (_m) _r[_n++] = _t[--_m]; } for (int _k = 0; _k < _n && pos < 500; _k++) tmp[pos++] = _r[_k]; } while (0)
        PUT("processor\t: "); PUTU(i); PUT("\n");
        PUT("vendor_id\t: "); PUT(vendor); PUT("\n");
        PUT("cpu family\t: "); PUTU(fam); PUT("\n");
        PUT("model\t\t: "); PUTU(mod); PUT("\n");
        PUT("stepping\t: "); PUTU(step); PUT("\n");
        PUT("apicid\t\t: "); PUTU(acpi_cpu_apic_id(i)); PUT("\n\n");
        #undef PUT
        #undef PUTU
    }
    size_t len = (size_t)pos;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - (size_t)offset;
    for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = (uint8_t)tmp[offset + i];
    return (int64_t)size;
}

static int64_t proc_acpi_event_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node; (void)offset; (void)size; (void)buf;
    return 0;
}

static int64_t proc_acpi_table_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    uint32_t len = 0;
    const uint8_t *t = (const uint8_t *)acpi_get_table(node->name, &len);
    if (!t) return 0;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - (size_t)offset;
    for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = t[offset + i];
    return (int64_t)size;
}

/* ── Recovery console sources: kmsg, panic regs, exceptions, mounts ── */

static void kfmt_h(char *d, uint64_t v, int digits) {
    static const char hx[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) { d[i] = hx[v & 0xF]; v >>= 4; }
}

static int kfmt_u64(char *d, uint64_t v) {
    char rev[24];
    int r = 0;
    if (v == 0) rev[r++] = '0';
    else while (v > 0 && r < 20) { rev[r++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < r; i++) d[i] = rev[r - 1 - i];
    return r;
}

static int kfmt_str(char *d, int pos, int cap, const char *s) {
    while (*s && pos < cap) d[pos++] = *s++;
    return pos;
}

static int64_t proc_kmsg_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    return klog_read(offset, size, buf);
}

static void kfmt_flags(char *d, int *pos, int cap, uint64_t rflags) {
    static const struct { const char *n; int b; } fl[] = {
        { "CF", 0 }, { "PF", 2 }, { "ZF", 6 }, { "SF", 7 },
        { "IF", 9 }, { "DF", 10 }, { "OF", 11 },
    };
    *pos = kfmt_str(d, *pos, cap, "RFLAGS [");
    for (unsigned i = 0; i < sizeof(fl) / sizeof(fl[0]); i++) {
        *pos = kfmt_str(d, *pos, cap, fl[i].n);
        *pos = kfmt_str(d, *pos, cap, ":");
        if (*pos < cap) d[(*pos)++] = (rflags & (1ULL << fl[i].b)) ? '1' : '0';
        *pos = kfmt_str(d, *pos, cap, "]");
    }
    *pos = kfmt_str(d, *pos, cap, "\n");
}

static int kfmt_regs(char *d, int cap, const struct exc_record *r) {
    int pos = 0;
    pos = kfmt_str(d, pos, cap, "RIP "); kfmt_h(d + pos, r->rip, 16); pos += 16;
    pos = kfmt_str(d, pos, cap, "  RSP "); kfmt_h(d + pos, r->rsp, 16); pos += 16;
    pos = kfmt_str(d, pos, cap, "  ERR "); kfmt_h(d + pos, r->err_code, 16); pos += 16;
    pos = kfmt_str(d, pos, cap, "\n");
    pos = kfmt_str(d, pos, cap, "RAX "); kfmt_h(d + pos, r->rax, 16); pos += 16;
    pos = kfmt_str(d, pos, cap, "  RBX "); kfmt_h(d + pos, r->rbx, 16); pos += 16;
    pos = kfmt_str(d, pos, cap, "\n");
    pos = kfmt_str(d, pos, cap, "RCX "); kfmt_h(d + pos, r->rcx, 16); pos += 16;
    pos = kfmt_str(d, pos, cap, "  RDX "); kfmt_h(d + pos, r->rdx, 16); pos += 16;
    pos = kfmt_str(d, pos, cap, "\n");
    pos = kfmt_str(d, pos, cap, "RSI "); kfmt_h(d + pos, r->rsi, 16); pos += 16;
    pos = kfmt_str(d, pos, cap, "  RDI "); kfmt_h(d + pos, r->rdi, 16); pos += 16;
    pos = kfmt_str(d, pos, cap, "\n");
    pos = kfmt_str(d, pos, cap, "RBP "); kfmt_h(d + pos, r->rbp, 16); pos += 16;
    pos = kfmt_str(d, pos, cap, "  CR2 "); kfmt_h(d + pos, r->cr2, 16); pos += 16;
    pos = kfmt_str(d, pos, cap, "  CR3 "); kfmt_h(d + pos, r->cr3, 16); pos += 16;
    pos = kfmt_str(d, pos, cap, "\n");
    kfmt_flags(d, &pos, cap, r->rflags);
    return pos;
}

static int64_t proc_panic_regs_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    uint64_t n = exc_count();
    if (n == 0) {
        static const char none[] = "no exception recorded yet\n";
        size_t len = sizeof(none) - 1;
        if (offset >= len) return 0;
        if (offset + size > len) size = len - (size_t)offset;
        for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = (uint8_t)none[offset + i];
        return (int64_t)size;
    }
    struct exc_record r;
    if (exc_get(n - 1, &r) != 0) return 0;
    char tmp[768];
    int pos = 0;
    pos = kfmt_str(tmp, pos, (int)sizeof(tmp), "vector ");
    pos += kfmt_u64(tmp + pos, r.vector);
    pos = kfmt_str(tmp, pos, (int)sizeof(tmp), " (");
    pos = kfmt_str(tmp, pos, (int)sizeof(tmp), exc_mnemonic(r.vector));
    pos = kfmt_str(tmp, pos, (int)sizeof(tmp), ") pid ");
    pos += kfmt_u64(tmp + pos, r.pid);
    pos = kfmt_str(tmp, pos, (int)sizeof(tmp), " ticks ");
    pos += kfmt_u64(tmp + pos, r.ticks);
    pos = kfmt_str(tmp, pos, (int)sizeof(tmp), "\n");
    {
        int rp = kfmt_regs(tmp + pos, (int)sizeof(tmp) - pos, &r);
        pos += rp;
    }
    size_t len = (size_t)pos;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - (size_t)offset;
    for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = (uint8_t)tmp[offset + i];
    return (int64_t)size;
}

/* Full exception history, newest first, blank-line separated blocks. */
static char exc_text[4096];

static int64_t proc_exceptions_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    int pos = 0;
    const int cap = (int)sizeof(exc_text);
    uint64_t n = exc_count();
    uint32_t freq = timer_get_freq();
    if (freq == 0) freq = 100;
    for (uint64_t k = 0; k < n; k++) {
        struct exc_record r;
        if (exc_get(n - 1 - k, &r) != 0) break;
        uint64_t idx = n - 1 - k;
        pos = kfmt_str(exc_text, pos, cap, "ENTRY ");
        pos += kfmt_u64(exc_text + pos, idx);
        pos = kfmt_str(exc_text, pos, cap, " ");
        pos = kfmt_str(exc_text, pos, cap, exc_mnemonic(r.vector));
        pos = kfmt_str(exc_text, pos, cap, " @");
        kfmt_h(exc_text + pos, r.rip, 16); pos += 16;
        pos = kfmt_str(exc_text, pos, cap, " t+");
        uint64_t sec = r.ticks / freq, us = (r.ticks % freq) * 1000000ULL / freq;
        pos += kfmt_u64(exc_text + pos, sec);
        pos = kfmt_str(exc_text, pos, cap, ".");
        {
            char usbuf[8];
            for (int i = 5; i >= 0; i--) { usbuf[i] = (char)('0' + us % 10); us /= 10; }
            for (int i = 0; i < 6 && pos < cap; i++) exc_text[pos++] = usbuf[i];
        }
        pos = kfmt_str(exc_text, pos, cap, " pid=");
        pos += kfmt_u64(exc_text + pos, r.pid);
        pos = kfmt_str(exc_text, pos, cap, "\n");
        {
            int rp = kfmt_regs(exc_text + pos, cap - pos, &r);
            pos += rp;
        }
        pos = kfmt_str(exc_text, pos, cap, "\n");
        if (pos >= cap - 512) break; /* keep room; oldest entries dropped */
    }
    if (n == 0) pos = kfmt_str(exc_text, pos, cap, "no exceptions recorded\n");
    size_t len = (size_t)pos;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - (size_t)offset;
    for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = (uint8_t)exc_text[offset + i];
    return (int64_t)size;
}

static int64_t proc_mounts_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    char tmp[512];
    int pos = 0;
    const int cap = (int)sizeof(tmp);
    for (int i = 0; i < nmounts && pos < cap - 128; i++) {
        pos = kfmt_str(tmp, pos, cap, mount_tab[i].src);
        pos = kfmt_str(tmp, pos, cap, " ");
        pos = kfmt_str(tmp, pos, cap, mount_tab[i].mp);
        pos = kfmt_str(tmp, pos, cap, " ");
        pos = kfmt_str(tmp, pos, cap, mount_tab[i].fstype);
        pos = kfmt_str(tmp, pos, cap, " rw 0 0\n");
    }
    size_t len = (size_t)pos;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - (size_t)offset;
    for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = (uint8_t)tmp[offset + i];
    return (int64_t)size;
}

static int64_t proc_net_dev_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    char tmp[512];
    int pos = 0;
    const int cap = (int)sizeof(tmp);
    pos = kfmt_str(tmp, pos, cap, "Inter-|   Receive                                                |  Transmit\n");
    pos = kfmt_str(tmp, pos, cap, " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
    pos = kfmt_str(tmp, pos, cap, "eth0: ");
    pos += kfmt_u64(tmp + pos, net_rx_bytes);
    pos = kfmt_str(tmp, pos, cap, " ");
    pos += kfmt_u64(tmp + pos, net_rx_packets);
    pos = kfmt_str(tmp, pos, cap, " 0 0 0 0 0 0 ");
    pos += kfmt_u64(tmp + pos, net_tx_bytes);
    pos = kfmt_str(tmp, pos, cap, " ");
    pos += kfmt_u64(tmp + pos, net_tx_packets);
    pos = kfmt_str(tmp, pos, cap, " 0 0 0 0 0 0\n");
    pos = kfmt_str(tmp, pos, cap, "lo: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    {
        size_t len = (size_t)pos;
        if (offset >= len) return 0;
        if (offset + size > len) size = len - (size_t)offset;
        for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = (uint8_t)tmp[offset + i];
        return (int64_t)size;
    }
}

static int64_t proc_net_route_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    char tmp[512];
    int pos = 0;
    const int cap = (int)sizeof(tmp);
    pos = kfmt_str(tmp, pos, cap, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
    if (g_netif.gateway != 0) {
        /* default route via the gateway */
        pos = kfmt_str(tmp, pos, cap, "eth0\t00000000\t");
        kfmt_h(tmp + pos, g_netif.gateway, 8); pos += 8;
        pos = kfmt_str(tmp, pos, cap, "\t0003\t0\t0\t0\t00000000\t0\t0\t0\n");
    }
    if (g_netif.ip != 0) {
        /* directly connected subnet */
        uint32_t net = g_netif.ip & g_netif.netmask;
        pos = kfmt_str(tmp, pos, cap, "eth0\t");
        kfmt_h(tmp + pos, net, 8); pos += 8;
        pos = kfmt_str(tmp, pos, cap, "\t00000000\t0001\t0\t0\t0\t");
        kfmt_h(tmp + pos, g_netif.netmask, 8); pos += 8;
        pos = kfmt_str(tmp, pos, cap, "\t0\t0\t0\n");
    }
    {
        size_t len = (size_t)pos;
        if (offset >= len) return 0;
        if (offset + size > len) size = len - (size_t)offset;
        for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = (uint8_t)tmp[offset + i];
        return (int64_t)size;
    }
}

static int64_t proc_partitions_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {    (void)node;
    char tmp[512];
    int pos = 0;
    const int cap = (int)sizeof(tmp);
    pos = kfmt_str(tmp, pos, cap, "# no block layer yet; mounted filesystems:\n");
    pos = kfmt_str(tmp, pos, cap, "# <mountpoint> <fstype>\n");
    for (int i = 0; i < nmounts && pos < cap - 96; i++) {
        pos = kfmt_str(tmp, pos, cap, mount_tab[i].mp);
        pos = kfmt_str(tmp, pos, cap, " ");
        pos = kfmt_str(tmp, pos, cap, mount_tab[i].fstype);
        pos = kfmt_str(tmp, pos, cap, "\n");
    }
    size_t len = (size_t)pos;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - (size_t)offset;
    for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = (uint8_t)tmp[offset + i];
    return (int64_t)size;
}

static int64_t proc_pci_devices_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node;
    static char tmp[2048];
    int pos = 0;
    const int cap = (int)sizeof(tmp);
    uint32_t count = pci_get_device_count();
    for (uint32_t i = 0; i < count && pos < cap - 160; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) break;
        kfmt_h(tmp + pos, d->bus, 2); pos += 2;
        pos = kfmt_str(tmp, pos, cap, ":");
        kfmt_h(tmp + pos, d->slot, 2); pos += 2;
        pos = kfmt_str(tmp, pos, cap, ".");
        if (pos < cap) tmp[pos++] = (char)('0' + (d->func & 7));
        pos = kfmt_str(tmp, pos, cap, " ");
        kfmt_h(tmp + pos, d->vendor_id, 4); pos += 4;
        pos = kfmt_str(tmp, pos, cap, ":");
        kfmt_h(tmp + pos, d->device_id, 4); pos += 4;
        pos = kfmt_str(tmp, pos, cap, " class=");
        kfmt_h(tmp + pos, d->class_id, 2); pos += 2;
        kfmt_h(tmp + pos, d->subclass_id, 2); pos += 2;
        pos = kfmt_str(tmp, pos, cap, " \"");
        pos = kfmt_str(tmp, pos, cap, pci_class_to_string(d->class_id, d->subclass_id));
        pos = kfmt_str(tmp, pos, cap, "\" \"");
        pos = kfmt_str(tmp, pos, cap, pci_device_to_string(d->vendor_id, d->device_id));
        pos = kfmt_str(tmp, pos, cap, "\"\n");
    }
    size_t len = (size_t)pos;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - (size_t)offset;
    for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = (uint8_t)tmp[offset + i];
    return (int64_t)size;
}

static struct vfs_node *proc_acpi_finddir(struct vfs_node *node, const char *name);
static struct vfs_dirent *proc_acpi_readdir(struct vfs_node *node, uint32_t index);

static struct vfs_node *proc_acpi_finddir(struct vfs_node *node, const char *name) {
    (void)node;
    if (kstrcmp(name, "event") == 0) return &proc_acpi_event_node;
    static const char *tabs[] = { "FACP", "APIC", "MCFG", "HPET" };
    for (int i = 0; i < 4; i++) {
        if (kstrcmp(name, tabs[i]) == 0) {
            uint32_t len = 0;
            if (!acpi_get_table(tabs[i], &len)) return NULL;
            return &proc_acpi_tab_nodes[i];
        }
    }
    return NULL;
}

static struct vfs_dirent *proc_acpi_readdir(struct vfs_node *node, uint32_t index) {
    static struct vfs_dirent dent;
    const char *tabs[] = { "FACP", "APIC", "MCFG", "HPET" };
    (void)node;
    if (index == 0) {
        kstrncpy(dent.name, "event", sizeof(dent.name));
        dent.ino = 5000;
        dent.type = VFS_FILE;
        dent.size = 0;
        return &dent;
    }
    uint32_t cur = 1;
    for (int i = 0; i < 4; i++) {
        uint32_t len = 0;
        if (!acpi_get_table(tabs[i], &len)) continue;
        if (cur == index) {
            kstrncpy(dent.name, tabs[i], sizeof(dent.name));
            dent.ino = 5001 + (uint64_t)i;
            dent.type = VFS_FILE;
            dent.size = len;
            return &dent;
        }
        cur++;
    }
    return NULL;
}

static struct vfs_node proc_root_node;
static struct vfs_node proc_version_node;
static struct vfs_node proc_uptime_node;
static struct vfs_node proc_meminfo_node;
static struct vfs_node proc_stat_node;
static struct vfs_node proc_cpuinfo_node;
static struct vfs_node proc_interrupts_node;
static struct vfs_node proc_acpi_node;
static struct vfs_node proc_acpi_event_node;
static struct vfs_node proc_kmsg_node;
static struct vfs_node proc_panic_regs_node;
static struct vfs_node proc_exceptions_node;
static struct vfs_node proc_mounts_node;
static struct vfs_node proc_partitions_node;
static struct vfs_node proc_bus_node;
static struct vfs_node proc_bus_pci_node;
static struct vfs_node proc_pci_devices_node;
static struct vfs_node proc_net_node;
static struct vfs_node proc_net_dev_node;
static struct vfs_node proc_net_route_node;
static struct vfs_node proc_acpi_tab_nodes[4];
static struct vfs_node proc_self_node;
static struct vfs_node proc_pid_nodes[16];
static struct vfs_node proc_pid_fd_nodes[16];
static struct vfs_node proc_pid_fd_link_nodes[16][16];

static struct vfs_node *proc_bus_pci_finddir(struct vfs_node *node, const char *name);
static struct vfs_dirent *proc_bus_pci_readdir(struct vfs_node *node, uint32_t index);
static struct vfs_node *proc_bus_finddir(struct vfs_node *node, const char *name);
static struct vfs_dirent *proc_bus_readdir(struct vfs_node *node, uint32_t index);
static struct vfs_node *proc_net_finddir(struct vfs_node *node, const char *name);
static struct vfs_dirent *proc_net_readdir(struct vfs_node *node, uint32_t index);

static struct vfs_node *proc_bus_finddir(struct vfs_node *node, const char *name) {
    (void)node;
    if (kstrcmp(name, "pci") == 0) return &proc_bus_pci_node;
    return NULL;
}

static struct vfs_dirent *proc_bus_readdir(struct vfs_node *node, uint32_t index) {
    static struct vfs_dirent dent;
    (void)node;
    if (index != 0) return NULL;
    kstrncpy(dent.name, "pci", sizeof(dent.name));
    dent.ino = 17;
    dent.type = VFS_DIRECTORY;
    dent.size = 0;
    return &dent;
}

static struct vfs_node *proc_bus_pci_finddir(struct vfs_node *node, const char *name) {
    (void)node;
    if (kstrcmp(name, "devices") == 0) return &proc_pci_devices_node;
    return NULL;
}

static struct vfs_dirent *proc_bus_pci_readdir(struct vfs_node *node, uint32_t index) {
    static struct vfs_dirent dent;
    (void)node;
    if (index != 0) return NULL;
    kstrncpy(dent.name, "devices", sizeof(dent.name));
    dent.ino = 18;
    dent.type = VFS_FILE;
    dent.size = 0;
    return &dent;
}

static struct vfs_node *proc_net_finddir(struct vfs_node *node, const char *name) {
    (void)node;
    if (kstrcmp(name, "dev") == 0) return &proc_net_dev_node;
    if (kstrcmp(name, "route") == 0) return &proc_net_route_node;
    return NULL;
}

static struct vfs_dirent *proc_net_readdir(struct vfs_node *node, uint32_t index) {
    static struct vfs_dirent dent;
    (void)node;
    if (index == 0) {
        kstrncpy(dent.name, "dev", sizeof(dent.name));
        dent.ino = 20;
        dent.type = VFS_FILE;
        dent.size = 0;
        return &dent;
    }
    if (index == 1) {
        kstrncpy(dent.name, "route", sizeof(dent.name));
        dent.ino = 21;
        dent.type = VFS_FILE;
        dent.size = 0;
        return &dent;
    }
    return NULL;
}

static struct vfs_node *proc_pid_finddir(struct vfs_node *node, const char *name);
static struct vfs_dirent *proc_pid_readdir(struct vfs_node *node, uint32_t index);
static struct vfs_node *proc_fd_finddir(struct vfs_node *node, const char *name);
static struct vfs_dirent *proc_fd_readdir(struct vfs_node *node, uint32_t index);

static int64_t proc_pid_cmdline_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    uint32_t pid = (uint32_t)(uintptr_t)node->device;
    struct process *p = process_find(pid);
    if (!p) return 0;
    const char *s = p->name;
    size_t len = 0;
    while (s[len]) len++;
    if (len == 0) return 0;
    /* cmdline is NUL-separated, we return with trailing NUL */
    if (offset >= len + 1) return 0;
    size_t avail = len + 1 - (size_t)offset;
    if (size > avail) size = avail;
    for (size_t i = 0; i < size; i++) {
        char c = (offset + i < len) ? s[offset + i] : '\0';
        ((uint8_t *)buf)[i] = (uint8_t)c;
    }
    return (int64_t)size;
}

static int64_t proc_pid_status_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    uint32_t pid = (uint32_t)(uintptr_t)node->device;
    struct process *p = process_find(pid);
    if (!p) return 0;
    char tmp[256];
    int pos = 0;
    #define PAPPEND(s) do { const char *_s=(s); while(*_s && pos<240) tmp[pos++]=*_s++; } while(0)
    #define PAPPEND_UL(v) do { char _b[24]; int _l=0; uint64_t _v=(v); if(_v==0) _b[_l++]='0'; else {char _r[24];int _ri=0;while(_v>0&&_ri<20){_r[_ri++]='0'+(_v%10);_v/=10;}while(_ri>0)_b[_l++]=_r[--_ri];} for(int _i=0;_i<_l&&pos<240;_i++) tmp[pos++]=_b[_i]; } while(0)
    PAPPEND("Name:\t"); PAPPEND(p->name); PAPPEND("\n");
    PAPPEND("State:\t"); PAPPEND(p->stopped ? "T (stopped)\n" : "R (running)\n");
    PAPPEND("Pid:\t"); PAPPEND_UL(p->pid); PAPPEND("\n");
    PAPPEND("PPid:\t"); PAPPEND_UL(p->ppid); PAPPEND("\n");
    PAPPEND("VmSize:\t0 kB\n");
    #undef PAPPEND
    #undef PAPPEND_UL
    size_t len = (size_t)pos;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - (size_t)offset;
    for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = (uint8_t)tmp[offset + i];
    return (int64_t)size;
}

static int64_t proc_pid_stat_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    uint32_t pid = (uint32_t)(uintptr_t)node->device;
    struct process *p = process_find(pid);
    if (!p) return 0;
    char tmp[256];
    int pos = 0;
    #define SAPPEND(s) do { const char *_s=(s); while(*_s&&pos<240) tmp[pos++]=*_s++; } while(0)
    #define SAPPEND_UL(v) do { char _b[24]; int _l=0; uint32_t _v=(v); if(_v==0)_b[_l++]='0'; else {char _r[12];int _ri=0;while(_v>0&&_ri<10){_r[_ri++]='0'+(_v%10);_v/=10;}while(_ri>0)_b[_l++]=_r[--_ri];} for(int _i=0;_i<_l&&pos<240;_i++) tmp[pos++]=_b[_i]; } while(0)
    SAPPEND_UL(p->pid); SAPPEND(" ("); SAPPEND(p->name); SAPPEND(") R 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    #undef SAPPEND
    #undef SAPPEND_UL
    size_t len = 0;
    while (tmp[len]) len++;
    if (offset >= len) return 0;
    if (offset + size > len) size = len - (size_t)offset;
    for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = (uint8_t)tmp[offset + i];
    return (int64_t)size;
}

static struct vfs_node proc_pid_cmdline_nodes[16];
static struct vfs_node proc_pid_status_nodes[16];
static struct vfs_node proc_pid_stat_nodes[16];

static int64_t proc_readlink(struct vfs_node *node, char *buf, size_t size);

static struct vfs_node *proc_pid_finddir(struct vfs_node *node, const char *name) {
    if (kstrcmp(name, "cmdline") == 0) {
        uint32_t pid = (uint32_t)(uintptr_t)node->device;
        for (int i = 0; i < 16; i++) {
            if (proc_pid_cmdline_nodes[i].device == (void*)(uintptr_t)pid) {
                return &proc_pid_cmdline_nodes[i];
            }
        }
        for (int i = 0; i < 16; i++) {
            if (proc_pid_cmdline_nodes[i].name[0] == '\0') {
                kstrncpy(proc_pid_cmdline_nodes[i].name, "cmdline", sizeof(proc_pid_cmdline_nodes[i].name));
                proc_pid_cmdline_nodes[i].type = VFS_FILE;
                proc_pid_cmdline_nodes[i].permissions = 0444;
                proc_pid_cmdline_nodes[i].inode = 3000 + pid * 10 + 1;
                proc_pid_cmdline_nodes[i].device = (void*)(uintptr_t)pid;
                proc_pid_cmdline_nodes[i].read = proc_pid_cmdline_read;
                return &proc_pid_cmdline_nodes[i];
            }
        }
        return NULL;
    }
    if (kstrcmp(name, "status") == 0) {
        uint32_t pid = (uint32_t)(uintptr_t)node->device;
        for (int i = 0; i < 16; i++) {
            if (proc_pid_status_nodes[i].device == (void*)(uintptr_t)pid) return &proc_pid_status_nodes[i];
        }
        for (int i = 0; i < 16; i++) {
            if (proc_pid_status_nodes[i].name[0] == '\0') {
                kstrncpy(proc_pid_status_nodes[i].name, "status", sizeof(proc_pid_status_nodes[i].name));
                proc_pid_status_nodes[i].type = VFS_FILE;
                proc_pid_status_nodes[i].permissions = 0444;
                proc_pid_status_nodes[i].inode = 3000 + pid * 10 + 2;
                proc_pid_status_nodes[i].device = (void*)(uintptr_t)pid;
                proc_pid_status_nodes[i].read = proc_pid_status_read;
                return &proc_pid_status_nodes[i];
            }
        }
        return NULL;
    }
    if (kstrcmp(name, "stat") == 0) {
        uint32_t pid = (uint32_t)(uintptr_t)node->device;
        for (int i = 0; i < 16; i++) {
            if (proc_pid_stat_nodes[i].device == (void*)(uintptr_t)pid) return &proc_pid_stat_nodes[i];
        }
        for (int i = 0; i < 16; i++) {
            if (proc_pid_stat_nodes[i].name[0] == '\0') {
                kstrncpy(proc_pid_stat_nodes[i].name, "stat", sizeof(proc_pid_stat_nodes[i].name));
                proc_pid_stat_nodes[i].type = VFS_FILE;
                proc_pid_stat_nodes[i].permissions = 0444;
                proc_pid_stat_nodes[i].inode = 3000 + pid * 10 + 3;
                proc_pid_stat_nodes[i].device = (void*)(uintptr_t)pid;
                proc_pid_stat_nodes[i].read = proc_pid_stat_read;
                return &proc_pid_stat_nodes[i];
            }
        }
        return NULL;
    }
    if (kstrcmp(name, "fd") == 0) {
        uint32_t pid = (uint32_t)(uintptr_t)node->device;
        for (int i = 0; i < 16; i++) {
            if (proc_pid_fd_nodes[i].device == (void*)(uintptr_t)pid) return &proc_pid_fd_nodes[i];
        }
        for (int i = 0; i < 16; i++) {
            if (proc_pid_fd_nodes[i].name[0] == '\0') {
                kstrncpy(proc_pid_fd_nodes[i].name, "fd", sizeof(proc_pid_fd_nodes[i].name));
                proc_pid_fd_nodes[i].type = VFS_DIRECTORY;
                proc_pid_fd_nodes[i].permissions = 0555;
                proc_pid_fd_nodes[i].inode = 3000 + pid * 10 + 4;
                proc_pid_fd_nodes[i].device = (void*)(uintptr_t)pid;
                proc_pid_fd_nodes[i].finddir = proc_fd_finddir;
                proc_pid_fd_nodes[i].readdir = proc_fd_readdir;
                return &proc_pid_fd_nodes[i];
            }
        }
        return NULL;
    }
    return NULL;
}

static struct vfs_dirent *proc_pid_readdir(struct vfs_node *node, uint32_t index) {
    static struct vfs_dirent dent;
    const char *names[] = {"cmdline", "status", "stat", "fd"};
    if (index >= 4) return NULL;
    (void)node;
    kstrncpy(dent.name, names[index], sizeof(dent.name));
    dent.ino = 3000 + index;
    dent.type = (index == 3) ? VFS_DIRECTORY : VFS_FILE;
    dent.size = 0;
    return &dent;
}

static struct vfs_node *proc_fd_finddir(struct vfs_node *node, const char *name) {
    uint32_t pid = (uint32_t)(uintptr_t)node->device;
    /* name is fd number like "0" */
    int fd = 0;
    for (int i = 0; name[i]; i++) {
        if (name[i] < '0' || name[i] > '9') return NULL;
        fd = fd * 10 + (name[i] - '0');
    }
    if (fd < 0 || fd >= MAX_FD_COUNT) return NULL;
    struct process *p = process_find(pid);
    if (!p) {
        if (current_process && current_process->pid == pid) p = current_process;
        else return NULL;
    }
    if (fd < 0 || fd >= MAX_FD_COUNT || !p->fds[fd].is_used) return NULL;
    /* Check if fd exists for that process */
    struct file_descriptor *fds = p->fds;
    /* For kernel's ash (pid 1) fds are per-process; check */
    if (fd >= MAX_FD_COUNT || !fds[fd].is_used) {
        /* Also check global kfds for pid 1 fallback */
        return NULL;
    }
    /* Return a symlink node pointing to the fd's target */
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            if (proc_pid_fd_link_nodes[i][j].name[0] != '\0' &&
                proc_pid_fd_link_nodes[i][j].device == (void*)(uintptr_t)((pid<<16)|fd)) {
                return &proc_pid_fd_link_nodes[i][j];
            }
        }
    }
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            if (proc_pid_fd_link_nodes[i][j].name[0] == '\0') {
                char fdname[8];
                int len = 0;
                int t = fd;
                char rev[8];
                int r = 0;
                if (t == 0) rev[r++] = '0';
                else while (t > 0 && r < 7) { rev[r++] = '0' + (t%10); t/=10; }
                while (r > 0) fdname[len++] = rev[--r];
                fdname[len] = '\0';
                kstrncpy(proc_pid_fd_link_nodes[i][j].name, fdname, sizeof(proc_pid_fd_link_nodes[i][j].name));
                proc_pid_fd_link_nodes[i][j].type = VFS_SYMLINK;
                proc_pid_fd_link_nodes[i][j].permissions = 0777;
                proc_pid_fd_link_nodes[i][j].inode = 4000 + pid * 16 + fd;
                proc_pid_fd_link_nodes[i][j].device = (void*)(uintptr_t)((pid<<16)|fd);
                /* Determine link target: tty for char devices, else file */
                if (!fds[fd].node && fds[fd].sock_alias >= 0) {
                    char sockname[24];
                    int pos = 0;
                    const char *pre = "socket:[";
                    while (*pre && pos < 20) sockname[pos++] = *pre++;
                    int t2 = fds[fd].sock_alias;
                    char rev2[8];
                    int r2 = 0;
                    if (t2 == 0) rev2[r2++] = '0';
                    else while (t2 > 0 && r2 < 7) { rev2[r2++] = '0' + (t2%10); t2/=10; }
                    while (r2 > 0 && pos < 20) sockname[pos++] = rev2[--r2];
                    sockname[pos++] = ']';
                    sockname[pos] = '\0';
                    kstrncpy(proc_pid_fd_link_nodes[i][j].linktarget, sockname, sizeof(proc_pid_fd_link_nodes[i][j].linktarget));
                } else if (fds[fd].node && fds[fd].node->type == VFS_CHARDEVICE) {
                    kstrncpy(proc_pid_fd_link_nodes[i][j].linktarget, "/dev/tty", sizeof(proc_pid_fd_link_nodes[i][j].linktarget));
                } else if (fds[fd].node) {
                    /* Use node's name if available */
                    kstrncpy(proc_pid_fd_link_nodes[i][j].linktarget, fds[fd].node->name, sizeof(proc_pid_fd_link_nodes[i][j].linktarget));
                    if (proc_pid_fd_link_nodes[i][j].linktarget[0] == '\0') {
                        kstrncpy(proc_pid_fd_link_nodes[i][j].linktarget, "/dev/null", sizeof(proc_pid_fd_link_nodes[i][j].linktarget));
                    }
                } else {
                    kstrncpy(proc_pid_fd_link_nodes[i][j].linktarget, "/dev/null", sizeof(proc_pid_fd_link_nodes[i][j].linktarget));
                }
                return &proc_pid_fd_link_nodes[i][j];
            }
        }
    }
    return NULL;
}

static struct vfs_dirent *proc_fd_readdir(struct vfs_node *node, uint32_t index) {
    static struct vfs_dirent dent;
    uint32_t pid = (uint32_t)(uintptr_t)node->device;
    struct process *p = process_find(pid);
    if (!p) {
        if (current_process && current_process->pid == pid) p = current_process;
        else return NULL;
    }
    /* Iterate over fds to find index-th used fd */
    uint32_t seen = 0;
    for (int fd = 0; fd < MAX_FD_COUNT; fd++) {
        if (!p->fds[fd].is_used) continue;
        if (seen == index) {
            char fdname[8];
            int len = 0;
            int t = fd;
            char rev[8];
            int r = 0;
            if (t == 0) rev[r++] = '0';
            else while (t > 0 && r < 7) { rev[r++] = '0' + (t%10); t/=10; }
            while (r > 0) fdname[len++] = rev[--r];
            fdname[len] = '\0';
            kstrncpy(dent.name, fdname, sizeof(dent.name));
            dent.ino = 4000 + pid * 16 + fd;
            dent.type = VFS_SYMLINK;
            dent.size = 0;
            return &dent;
        }
        seen++;
    }
    return NULL;
}

static int64_t proc_readlink(struct vfs_node *node, char *buf, size_t size) {
    if (node->type != VFS_SYMLINK) return -1;
    size_t len = kstrlen(node->linktarget);
    if (len >= size) len = size - 1;
    kmemcpy(buf, node->linktarget, len);
    buf[len] = '\0';
    return (int64_t)len;
}

static struct vfs_node *proc_finddir(struct vfs_node *node, const char *name) {
    (void)node;
    if (kstrcmp(name, "self") == 0) {
        uint32_t pid = current_process ? current_process->pid : 1;
        char target[32];
        int pos = 0;
        const char *prefix = "/proc/";
        for (int i = 0; prefix[i] && pos < 30; i++) target[pos++] = prefix[i];
        /* Append pid decimal */
        char rev[12];
        int r = 0;
        uint32_t t = pid;
        if (t == 0) rev[r++] = '0';
        else while (t > 0 && r < 10) { rev[r++] = '0' + (t%10); t/=10; }
        while (r > 0 && pos < 30) target[pos++] = rev[--r];
        target[pos] = '\0';
        kstrncpy(proc_self_node.linktarget, target, sizeof(proc_self_node.linktarget));
        return &proc_self_node;
    }
    if (kstrcmp(name, "version") == 0) return &proc_version_node;
    if (kstrcmp(name, "uptime") == 0) return &proc_uptime_node;
    if (kstrcmp(name, "meminfo") == 0) return &proc_meminfo_node;
    if (kstrcmp(name, "stat") == 0) return &proc_stat_node;
    if (kstrcmp(name, "cpuinfo") == 0) return &proc_cpuinfo_node;
    if (kstrcmp(name, "interrupts") == 0) return &proc_interrupts_node;
    if (kstrcmp(name, "acpi") == 0) return &proc_acpi_node;
    if (kstrcmp(name, "kmsg") == 0) return &proc_kmsg_node;
    if (kstrcmp(name, "panic_regs") == 0) return &proc_panic_regs_node;
    if (kstrcmp(name, "exceptions") == 0) return &proc_exceptions_node;
    if (kstrcmp(name, "mounts") == 0) return &proc_mounts_node;
    if (kstrcmp(name, "partitions") == 0) return &proc_partitions_node;
    if (kstrcmp(name, "bus") == 0) return &proc_bus_node;
    if (kstrcmp(name, "net") == 0) return &proc_net_node;
    /* Numeric pid */
    int isnum = 1;
    for (int i = 0; name[i]; i++) {
        if (name[i] < '0' || name[i] > '9') { isnum = 0; break; }
    }
    if (isnum) {
        uint32_t pid = 0;
        for (int i = 0; name[i]; i++) pid = pid * 10 + (name[i] - '0');
        struct process *p = process_find(pid);
        if (!p) {
            if (current_process && current_process->pid == pid) p = current_process;
            else return NULL;
        }
        for (int i = 0; i < 16; i++) {
            if (proc_pid_nodes[i].device == (void*)(uintptr_t)pid) return &proc_pid_nodes[i];
        }
        for (int i = 0; i < 16; i++) {
            if (proc_pid_nodes[i].name[0] == '\0') {
                kstrncpy(proc_pid_nodes[i].name, name, sizeof(proc_pid_nodes[i].name));
                proc_pid_nodes[i].type = VFS_DIRECTORY;
                proc_pid_nodes[i].permissions = 0555;
                proc_pid_nodes[i].inode = 2000 + pid;
                proc_pid_nodes[i].device = (void*)(uintptr_t)pid;
                proc_pid_nodes[i].finddir = proc_pid_finddir;
                proc_pid_nodes[i].readdir = proc_pid_readdir;
                return &proc_pid_nodes[i];
            }
        }
        return NULL;
    }
    return NULL;
}

static struct vfs_dirent *proc_readdir(struct vfs_node *node, uint32_t index) {
    static struct vfs_dirent dent;
    (void)node;
    if (index == 0) {
        kstrncpy(dent.name, "self", sizeof(dent.name));
        dent.ino = 1;
        dent.type = VFS_SYMLINK;
        dent.size = 0;
        return &dent;
    }
    if (index == 1) {
        kstrncpy(dent.name, "version", sizeof(dent.name));
        dent.ino = 2;
        dent.type = VFS_FILE;
        dent.size = 0;
        return &dent;
    }
    if (index == 2) {
        kstrncpy(dent.name, "uptime", sizeof(dent.name));
        dent.ino = 3;
        dent.type = VFS_FILE;
        dent.size = 0;
        return &dent;
    }
    if (index == 3) {
        kstrncpy(dent.name, "meminfo", sizeof(dent.name));
        dent.ino = 4;
        dent.type = VFS_FILE;
        dent.size = 0;
        return &dent;
    }
    if (index == 4) {
        kstrncpy(dent.name, "stat", sizeof(dent.name));
        dent.ino = 5;
        dent.type = VFS_FILE;
        dent.size = 0;
        return &dent;
    }
    if (index == 5) {
        kstrncpy(dent.name, "cpuinfo", sizeof(dent.name));
        dent.ino = 7;
        dent.type = VFS_FILE;
        dent.size = 0;
        return &dent;
    }
    if (index == 6) {
        kstrncpy(dent.name, "interrupts", sizeof(dent.name));
        dent.ino = 9;
        dent.type = VFS_FILE;
        dent.size = 0;
        return &dent;
    }
    if (index == 7) {
        kstrncpy(dent.name, "acpi", sizeof(dent.name));
        dent.ino = 8;
        dent.type = VFS_DIRECTORY;
        dent.size = 0;
        return &dent;
    }
    if (index == 8) {
        kstrncpy(dent.name, "kmsg", sizeof(dent.name));
        dent.ino = 10;
        dent.type = VFS_FILE;
        dent.size = 0;
        return &dent;
    }
    if (index == 9) {
        kstrncpy(dent.name, "panic_regs", sizeof(dent.name));
        dent.ino = 11;
        dent.type = VFS_FILE;
        dent.size = 0;
        return &dent;
    }
    if (index == 10) {
        kstrncpy(dent.name, "exceptions", sizeof(dent.name));
        dent.ino = 12;
        dent.type = VFS_FILE;
        dent.size = 0;
        return &dent;
    }
    if (index == 11) {
        kstrncpy(dent.name, "mounts", sizeof(dent.name));
        dent.ino = 13;
        dent.type = VFS_FILE;
        dent.size = 0;
        return &dent;
    }
    if (index == 12) {
        kstrncpy(dent.name, "partitions", sizeof(dent.name));
        dent.ino = 14;
        dent.type = VFS_FILE;
        dent.size = 0;
        return &dent;
    }
    if (index == 13) {
        kstrncpy(dent.name, "bus", sizeof(dent.name));
        dent.ino = 16;
        dent.type = VFS_DIRECTORY;
        dent.size = 0;
        return &dent;
    }
    if (index == 14) {
        kstrncpy(dent.name, "net", sizeof(dent.name));
        dent.ino = 19;
        dent.type = VFS_DIRECTORY;
        dent.size = 0;
        return &dent;
    }
    /* Pids */
    uint32_t pid_index = index - 15;
    uint32_t seen = 0;
    for (struct process *p = NULL; (p = process_iter(p)) != NULL; ) {
        if (seen == pid_index) {
            char pidstr[12];
            int pos = 0;
            uint32_t t = p->pid;
            char rev[12];
            int r = 0;
            if (t == 0) rev[r++] = '0';
            else while (t > 0 && r < 10) { rev[r++] = '0' + (t%10); t/=10; }
            while (r > 0) pidstr[pos++] = rev[--r];
            pidstr[pos] = '\0';
            kstrncpy(dent.name, pidstr, sizeof(dent.name));
            dent.ino = 2000 + p->pid;
            dent.type = VFS_DIRECTORY;
            dent.size = 0;
            return &dent;
        }
        seen++;
    }
    if (current_process) {
        /* Also include current if not in list (pid 1 may already be) */
        int found = 0;
        for (struct process *p = NULL; (p = process_iter(p)) != NULL; ) {
            if (p->pid == current_process->pid) { found = 1; break; }
        }
        if (!found && pid_index == seen) {
            char pidstr[12];
            int pos = 0;
            uint32_t t = current_process->pid;
            char rev[12];
            int r = 0;
            if (t == 0) rev[r++] = '0';
            else while (t > 0 && r < 10) { rev[r++] = '0' + (t%10); t/=10; }
            while (r > 0) pidstr[pos++] = rev[--r];
            pidstr[pos] = '\0';
            kstrncpy(dent.name, pidstr, sizeof(dent.name));
            dent.ino = 2000 + current_process->pid;
            dent.type = VFS_DIRECTORY;
            dent.size = 0;
            return &dent;
        }
    }
    return NULL;
}

struct vfs_node *procfs_root(void) {
    static int done = 0;
    if (done) return &proc_root_node;
    done = 1;
    kstrncpy(proc_root_node.name, "proc", sizeof(proc_root_node.name));
    proc_root_node.type = VFS_DIRECTORY;
    proc_root_node.permissions = 0555;
    proc_root_node.inode = 1;
    proc_root_node.finddir = proc_finddir;
    proc_root_node.readdir = proc_readdir;

    kstrncpy(proc_version_node.name, "version", sizeof(proc_version_node.name));
    proc_version_node.type = VFS_FILE;
    proc_version_node.permissions = 0444;
    proc_version_node.inode = 2;
    proc_version_node.read = proc_version_read;

    kstrncpy(proc_uptime_node.name, "uptime", sizeof(proc_uptime_node.name));
    proc_uptime_node.type = VFS_FILE;
    proc_uptime_node.permissions = 0444;
    proc_uptime_node.inode = 3;
    proc_uptime_node.read = proc_uptime_read;

    kstrncpy(proc_meminfo_node.name, "meminfo", sizeof(proc_meminfo_node.name));
    proc_meminfo_node.type = VFS_FILE;
    proc_meminfo_node.permissions = 0444;
    proc_meminfo_node.inode = 4;
    proc_meminfo_node.read = proc_meminfo_read;

    kstrncpy(proc_stat_node.name, "stat", sizeof(proc_stat_node.name));
    proc_stat_node.type = VFS_FILE;
    proc_stat_node.permissions = 0444;
    proc_stat_node.inode = 5;
    proc_stat_node.read = proc_stat_read;

    kstrncpy(proc_cpuinfo_node.name, "cpuinfo", sizeof(proc_cpuinfo_node.name));
    proc_cpuinfo_node.type = VFS_FILE;
    proc_cpuinfo_node.permissions = 0444;
    proc_cpuinfo_node.inode = 7;
    proc_cpuinfo_node.read = proc_cpuinfo_read;

    kstrncpy(proc_interrupts_node.name, "interrupts", sizeof(proc_interrupts_node.name));
    proc_interrupts_node.type = VFS_FILE;
    proc_interrupts_node.permissions = 0444;
    proc_interrupts_node.inode = 9;
    proc_interrupts_node.read = proc_interrupts_read;

    kstrncpy(proc_acpi_node.name, "acpi", sizeof(proc_acpi_node.name));
    proc_acpi_node.type = VFS_DIRECTORY;
    proc_acpi_node.permissions = 0555;
    proc_acpi_node.inode = 8;
    proc_acpi_node.finddir = proc_acpi_finddir;
    proc_acpi_node.readdir = proc_acpi_readdir;

    kstrncpy(proc_acpi_event_node.name, "event", sizeof(proc_acpi_event_node.name));
    proc_acpi_event_node.type = VFS_FILE;
    proc_acpi_event_node.permissions = 0444;
    proc_acpi_event_node.inode = 5000;
    proc_acpi_event_node.read = proc_acpi_event_read;

    kstrncpy(proc_kmsg_node.name, "kmsg", sizeof(proc_kmsg_node.name));
    proc_kmsg_node.type = VFS_FILE;
    proc_kmsg_node.permissions = 0400;
    proc_kmsg_node.inode = 10;
    proc_kmsg_node.read = proc_kmsg_read;

    kstrncpy(proc_panic_regs_node.name, "panic_regs", sizeof(proc_panic_regs_node.name));
    proc_panic_regs_node.type = VFS_FILE;
    proc_panic_regs_node.permissions = 0400;
    proc_panic_regs_node.inode = 11;
    proc_panic_regs_node.read = proc_panic_regs_read;

    kstrncpy(proc_exceptions_node.name, "exceptions", sizeof(proc_exceptions_node.name));
    proc_exceptions_node.type = VFS_FILE;
    proc_exceptions_node.permissions = 0400;
    proc_exceptions_node.inode = 12;
    proc_exceptions_node.read = proc_exceptions_read;

    kstrncpy(proc_mounts_node.name, "mounts", sizeof(proc_mounts_node.name));
    proc_mounts_node.type = VFS_FILE;
    proc_mounts_node.permissions = 0444;
    proc_mounts_node.inode = 13;
    proc_mounts_node.read = proc_mounts_read;

    kstrncpy(proc_partitions_node.name, "partitions", sizeof(proc_partitions_node.name));
    proc_partitions_node.type = VFS_FILE;
    proc_partitions_node.permissions = 0444;
    proc_partitions_node.inode = 14;
    proc_partitions_node.read = proc_partitions_read;

    kstrncpy(proc_bus_node.name, "bus", sizeof(proc_bus_node.name));
    proc_bus_node.type = VFS_DIRECTORY;
    proc_bus_node.permissions = 0555;
    proc_bus_node.inode = 16;
    proc_bus_node.finddir = proc_bus_finddir;
    proc_bus_node.readdir = proc_bus_readdir;

    kstrncpy(proc_bus_pci_node.name, "pci", sizeof(proc_bus_pci_node.name));
    proc_bus_pci_node.type = VFS_DIRECTORY;
    proc_bus_pci_node.permissions = 0555;
    proc_bus_pci_node.inode = 17;
    proc_bus_pci_node.finddir = proc_bus_pci_finddir;
    proc_bus_pci_node.readdir = proc_bus_pci_readdir;

    kstrncpy(proc_pci_devices_node.name, "devices", sizeof(proc_pci_devices_node.name));
    proc_pci_devices_node.type = VFS_FILE;
    proc_pci_devices_node.permissions = 0444;
    proc_pci_devices_node.inode = 18;
    proc_pci_devices_node.read = proc_pci_devices_read;

    kstrncpy(proc_net_node.name, "net", sizeof(proc_net_node.name));
    proc_net_node.type = VFS_DIRECTORY;
    proc_net_node.permissions = 0555;
    proc_net_node.inode = 19;
    proc_net_node.finddir = proc_net_finddir;
    proc_net_node.readdir = proc_net_readdir;

    kstrncpy(proc_net_dev_node.name, "dev", sizeof(proc_net_dev_node.name));
    proc_net_dev_node.type = VFS_FILE;
    proc_net_dev_node.permissions = 0444;
    proc_net_dev_node.inode = 20;
    proc_net_dev_node.read = proc_net_dev_read;

    kstrncpy(proc_net_route_node.name, "route", sizeof(proc_net_route_node.name));
    proc_net_route_node.type = VFS_FILE;
    proc_net_route_node.permissions = 0444;
    proc_net_route_node.inode = 21;
    proc_net_route_node.read = proc_net_route_read;

    {
        const char *tabs[] = { "FACP", "APIC", "MCFG", "HPET" };
        for (int i = 0; i < 4; i++) {
            kstrncpy(proc_acpi_tab_nodes[i].name, tabs[i], sizeof(proc_acpi_tab_nodes[i].name));
            proc_acpi_tab_nodes[i].type = VFS_FILE;
            proc_acpi_tab_nodes[i].permissions = 0444;
            proc_acpi_tab_nodes[i].inode = 5001 + (uint64_t)i;
            proc_acpi_tab_nodes[i].read = proc_acpi_table_read;
        }
    }

    kstrncpy(proc_self_node.name, "self", sizeof(proc_self_node.name));
    proc_self_node.type = VFS_SYMLINK;
    proc_self_node.permissions = 0777;
    proc_self_node.inode = 6;
    proc_self_node.readlink = proc_readlink;
    kstrncpy(proc_self_node.linktarget, "/proc/1", sizeof(proc_self_node.linktarget));

    serial_puts("[PROCFS] Ready (mounted at /proc)\n");
    return &proc_root_node;
}
