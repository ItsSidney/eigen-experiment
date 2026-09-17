/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * pty.c - PTY (pseudo-terminal) subsystem for EigenOS.
 */

#include <stdint.h>
#include <stddef.h>
#include "dev/char/pty.h"
#include "mm/kheap.h"
#include "fs/vfs/vfs.h"
#include "proc/process.h"
#include "include/serial.h"

struct pty_pair pty_pairs[PTY_MAX_PAIRS];
static uint32_t pty_next_num = 0;

void pty_init(void) {
    for (int i = 0; i < PTY_MAX_PAIRS; i++) {
        pty_pairs[i].used = 0;
        pty_pairs[i].m2s_buf = NULL;
        pty_pairs[i].s2m_buf = NULL;
    }
    serial_puts("[PTY] Subsystem initialized (max ");
    serial_puthex(PTY_MAX_PAIRS);
    serial_puts(" pairs).\n");
}

static void kstrncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
}

int pty_alloc(void) {
    for (int i = 0; i < PTY_MAX_PAIRS; i++) {
        if (!pty_pairs[i].used) {
            struct pty_pair *p = &pty_pairs[i];
            p->used = 1;
            p->num = pty_next_num++;

            p->m2s_buf = (uint8_t *)kmalloc(PTY_BUF_SIZE);
            p->s2m_buf = (uint8_t *)kmalloc(PTY_BUF_SIZE);
            if (!p->m2s_buf || !p->s2m_buf) {
                if (p->m2s_buf) kfree(p->m2s_buf);
                if (p->s2m_buf) kfree(p->s2m_buf);
                p->used = 0;
                return -1;
            }

            p->m2s_head = 0; p->m2s_tail = 0; p->m2s_count = 0;
            p->m2s_readers = 0; p->m2s_writers = 1;
            p->s2m_head = 0; p->s2m_tail = 0; p->s2m_count = 0;
            p->s2m_readers = 1; p->s2m_writers = 0;

            p->termios.c_iflag = PTY_ICRNL | PTY_IXON;
            p->termios.c_oflag = PTY_OPOST | PTY_ONLCR;
            p->termios.c_cflag = PTY_CS8 | PTY_CREAD | PTY_HUPCL;
            p->termios.c_lflag = PTY_ICANON | PTY_ISIG | PTY_ECHO | PTY_ECHOE;
            p->termios.c_cc[0] = 3;
            p->termios.c_cc[1] = 28;
            p->termios.c_cc[2] = 127;
            p->termios.c_cc[3] = 21;
            p->termios.c_cc[4] = 4;
            p->termios.c_cc[5] = 0;
            p->termios.c_cc[6] = 0;
            p->termios.c_cc[7] = 26;
            p->termios.c_cc[8] = 17;
            p->termios.c_cc[9] = 19;
            p->termios.c_ispeed = 38400;
            p->termios.c_ospeed = 38400;

            p->winsize.ws_row = 24; p->winsize.ws_col = 80;
            p->winsize.ws_xpixel = 0; p->winsize.ws_ypixel = 0;

            p->pgid = 0; p->session = 0;
            p->locked = 1; p->uid = 0; p->gid = 0;

            char name[16];
            name[0] = 'p'; name[1] = 't'; name[2] = 's'; name[3] = '/';
            uint32_t n = p->num;
            int pos = 4;
            if (n == 0) {
                name[pos++] = '0';
            } else {
                char digits[10];
                int nd = 0;
                while (n > 0) { digits[nd++] = '0' + (n % 10); n /= 10; }
                for (int d = nd - 1; d >= 0; d--) name[pos++] = digits[d];
            }
            name[pos] = '\0';

            kstrncpy(p->slave_node.name, name, sizeof(p->slave_node.name));
            p->slave_node.type = VFS_CHARDEVICE;
            p->slave_node.permissions = 0666;
            p->slave_node.inode = 100 + p->num;
            p->slave_node.device = p;
            p->slave_node.read = pty_slave_read;
            p->slave_node.write = pty_slave_write;
            p->slave_node.open = pty_slave_open;
            p->slave_node.close = pty_slave_close;

            return (int)p->num;
        }
    }
    return -1;
}

void pty_release(int num) {
    struct pty_pair *p = pty_get(num);
    if (!p) return;
    serial_puts("[PTY] Releasing pair "); serial_puthex((uint64_t)num); serial_puts("\n");
    if (p->m2s_buf) kfree(p->m2s_buf);
    if (p->s2m_buf) kfree(p->s2m_buf);
    p->m2s_buf = NULL;
    p->s2m_buf = NULL;
    p->used = 0;
}

static struct pty_pair *pty_find(uint32_t num) {
    for (int i = 0; i < PTY_MAX_PAIRS; i++) {
        if (pty_pairs[i].used && pty_pairs[i].num == num)
            return &pty_pairs[i];
    }
    return NULL;
}

struct pty_pair *pty_get(int num) {
    if (num < 0) return NULL;
    return pty_find((uint32_t)num);
}

static void pty_try_release(struct pty_pair *p) {
    if (!p || !p->used) return;
    if (p->m2s_readers <= 0 && p->m2s_writers <= 0 &&
        p->s2m_readers <= 0 && p->s2m_writers <= 0)
        pty_release((int)p->num);
}

int pty_master_dup(int num) {
    struct pty_pair *p = pty_get(num);
    if (!p) return -9;
    p->m2s_writers++;
    p->s2m_readers++;
    return 0;
}

/* Linux ioctl numbers this dispatch serves. */
#define PTY_TCGETS    0x5401
#define PTY_TCSETS    0x5402
#define PTY_TCSETSW   0x5403
#define PTY_TCSETSF   0x5404
#define PTY_TIOCGWINSZ 0x5413
#define PTY_TIOCSWINSZ 0x5414
#define PTY_TIOCGPTN  0x80045430
#define PTY_TIOCSPTLCK 0x40045431

int64_t pty_ioctl(struct pty_pair *pp, int is_master, unsigned long cmd,
                  uint64_t arg) {
    struct ktermios {
        uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
        uint8_t  c_line;
        uint8_t  c_cc[32];
        uint32_t c_ispeed, c_ospeed;
    };
    if (!pp || !pp->used) return 1;
    switch (cmd) {
        case PTY_TIOCGPTN: { /* pty number for grantpt/ptsname */
            int *np = (int *)arg;
            if (!is_master) return -25; /* -ENOTTY */
            if (!np) return -14;        /* -EFAULT */
            *np = (int)pp->num;
            return 0;
        }
        case PTY_TIOCSPTLCK: { /* 0 = unlock, nonzero = lock */
            int *lp = (int *)arg;
            if (!is_master) return -25;
            if (!lp) return -14;
            pp->locked = (*lp) ? 1 : 0;
            return 0;
        }
        case PTY_TCGETS: {
            struct ktermios *t = (struct ktermios *)arg;
            int i;
            if (!t) return -14;
            t->c_iflag = pp->termios.c_iflag;
            t->c_oflag = pp->termios.c_oflag;
            t->c_cflag = pp->termios.c_cflag;
            t->c_lflag = pp->termios.c_lflag;
            t->c_line  = 0;
            for (i = 0; i < 32; i++) t->c_cc[i] = 0;
            for (i = 0; i < PTY_MAX_CC && i < 32; i++)
                t->c_cc[i] = pp->termios.c_cc[i];
            t->c_ispeed = 0x00001002; /* B38400 */
            t->c_ospeed = 0x00001002;
            return 0;
        }
        case PTY_TCSETS:
        case PTY_TCSETSW:
        case PTY_TCSETSF: {
            const struct ktermios *t = (const struct ktermios *)arg;
            int i;
            if (!t) return -14;
            pp->termios.c_iflag = t->c_iflag;
            pp->termios.c_oflag = t->c_oflag;
            pp->termios.c_cflag = t->c_cflag;
            pp->termios.c_lflag = t->c_lflag;
            for (i = 0; i < PTY_MAX_CC && i < 32; i++)
                pp->termios.c_cc[i] = t->c_cc[i];
            return 0;
        }
        case PTY_TIOCGWINSZ: {
            uint16_t *ws = (uint16_t *)arg;
            if (!ws) return -14;
            ws[0] = pp->winsize.ws_row;
            ws[1] = pp->winsize.ws_col;
            ws[2] = pp->winsize.ws_xpixel;
            ws[3] = pp->winsize.ws_ypixel;
            return 0;
        }
        case PTY_TIOCSWINSZ: {
            const uint16_t *ws = (const uint16_t *)arg;
            if (!ws) return -14;
            pp->winsize.ws_row = ws[0];
            pp->winsize.ws_col = ws[1];
            pp->winsize.ws_xpixel = ws[2];
            pp->winsize.ws_ypixel = ws[3];
            return 0;
        }
        default:
            return 1;
    }
}

int pty_slave_open(struct vfs_node *node, int flags) {
    (void)flags;
    if (!node) return -9;
    struct pty_pair *p = (struct pty_pair *)node->device;
    if (!p || !p->used) return -9;
    if (p->locked) return -11;
    p->m2s_readers++;
    p->s2m_writers++;
    return 0;
}

int pty_slave_close(struct vfs_node *node) {
    if (!node) return 0;
    struct pty_pair *p = (struct pty_pair *)node->device;
    if (!p || !p->used) return 0;
    p->m2s_readers--;
    p->s2m_writers--;
    pty_try_release(p);
    return 0;
}

void pty_master_close(int num) {
    struct pty_pair *p = pty_get(num);
    if (!p) return;

    /* SIGHUP to the PTY's controlling process group if it has one */
    if (p->pgid != 0) {
        struct process *tp = process_list;
        while (tp) {
            if (tp->tgid == p->pgid || tp->pid == p->pgid) {
                tp->sig_pending |= (1ULL << (1 - 1)); /* SIGHUP = 1 */
            }
            tp = tp->next;
        }
    }

    p->m2s_writers--;
    p->s2m_readers--;
    pty_try_release(p);
}

struct vfs_node *pty_slave_node(int num) {
    struct pty_pair *p = pty_get(num);
    return p ? &p->slave_node : NULL;
}

struct pty_termios *pty_get_termios(int num) {
    struct pty_pair *p = pty_get(num);
    return p ? &p->termios : NULL;
}

int pty_set_termios(int num, const struct pty_termios *t) {
    struct pty_pair *p = pty_get(num);
    if (!p || !t) return -1;
    p->termios = *t;
    return 0;
}

struct pty_winsize *pty_get_winsize(int num) {
    struct pty_pair *p = pty_get(num);
    return p ? &p->winsize : NULL;
}

int pty_set_winsize(int num, const struct pty_winsize *w) {
    struct pty_pair *p = pty_get(num);
    if (!p || !w) return -1;
    p->winsize = *w;
    return 0;
}

int pty_grantpt(int num) {
    struct pty_pair *p = pty_get(num);
    if (!p) return -1;
    p->slave_node.permissions = 0620;
    return 0;
}

int pty_unlockpt(int num) {
    struct pty_pair *p = pty_get(num);
    if (!p) return -1;
    p->locked = 0;
    return 0;
}

int pty_ptsname(int num, char *buf, size_t bufsize) {
    struct pty_pair *p = pty_get(num);
    if (!p || !buf || bufsize < 8) return -1;
    buf[0]='/'; buf[1]='d'; buf[2]='e'; buf[3]='v';
    buf[4]='/'; buf[5]='p'; buf[6]='t'; buf[7]='s'; buf[8]='/';
    uint32_t n = p->num;
    if (n == 0) { buf[9]='0'; buf[10]='\0'; return 0; }
    char digits[10]; int nd = 0;
    while (n > 0) { digits[nd++] = '0' + (n % 10); n /= 10; }
    int pos = 9;
    for (int d = nd - 1; d >= 0; d--) buf[pos++] = digits[d];
    buf[pos] = '\0';
    return 0;
}

int pty_master_open(int *out_num) {
    int num = pty_alloc();
    if (num < 0) return -1;
    *out_num = num;
    return 0;
}

int64_t pty_master_read(int num, uint64_t offset, size_t size, void *buf) {
    struct pty_pair *p = pty_get(num);
    if (!p || !buf) return -14;
    (void)offset;
    if (size == 0) return 0;

    while (p->s2m_count == 0) {
        if (p->s2m_writers <= 0) return 0;
        if (current_process &&
            (current_process->sig_pending & ~current_process->sig_blocked))
            return -4;
        __asm__ volatile("pause");
    }

    uint8_t *dst = (uint8_t *)buf;
    size_t to_read = size < p->s2m_count ? size : p->s2m_count;
    for (size_t i = 0; i < to_read; i++) {
        dst[i] = p->s2m_buf[p->s2m_tail];
        p->s2m_tail = (p->s2m_tail + 1) % PTY_BUF_SIZE;
    }
    p->s2m_count -= (uint32_t)to_read;
    return (int64_t)to_read;
}

int64_t pty_master_write(int num, uint64_t offset, size_t size, const void *buf) {
    struct pty_pair *p = pty_get(num);
    if (!p || !buf) return -14;
    (void)offset;

    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;
    while (done < size) {
        if (p->m2s_readers <= 0) return done ? (int64_t)done : -32;
        while (p->m2s_count >= PTY_BUF_SIZE) {
            if (p->m2s_readers <= 0) return done ? (int64_t)done : -32;
            if (current_process &&
                (current_process->sig_pending & ~current_process->sig_blocked))
                return done ? (int64_t)done : -4;
            __asm__ volatile("pause");
        }
        size_t space = PTY_BUF_SIZE - p->m2s_count;
        size_t chunk = (size - done) < space ? (size - done) : space;
        for (size_t i = 0; i < chunk; i++) {
            p->m2s_buf[p->m2s_head] = src[done + i];
            p->m2s_head = (p->m2s_head + 1) % PTY_BUF_SIZE;
        }
        p->m2s_count += (uint32_t)chunk;
        done += chunk;
    }
    return (int64_t)done;
}

int64_t pty_slave_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    struct pty_pair *p = (struct pty_pair *)node->device;
    if (!p || !buf) return -14;
    (void)offset;
    if (size == 0) return 0;

    while (p->m2s_count == 0) {
        if (p->m2s_writers <= 0) return 0;
        if (current_process &&
            (current_process->sig_pending & ~current_process->sig_blocked))
            return -4;
        __asm__ volatile("pause");
    }

    uint8_t *dst = (uint8_t *)buf;
    size_t to_read = size < p->m2s_count ? size : p->m2s_count;
    for (size_t i = 0; i < to_read; i++) {
        dst[i] = p->m2s_buf[p->m2s_tail];
        p->m2s_tail = (p->m2s_tail + 1) % PTY_BUF_SIZE;
    }
    p->m2s_count -= (uint32_t)to_read;
    return (int64_t)to_read;
}

int64_t pty_slave_write(struct vfs_node *node, uint64_t offset, size_t size, const void *buf) {
    struct pty_pair *p = (struct pty_pair *)node->device;
    if (!p || !buf) return -14;
    (void)offset;

    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;
    while (done < size) {
        if (p->s2m_readers <= 0) return done ? (int64_t)done : -32;
        while (p->s2m_count >= PTY_BUF_SIZE) {
            if (p->s2m_readers <= 0) return done ? (int64_t)done : -32;
            if (current_process &&
                (current_process->sig_pending & ~current_process->sig_blocked))
                return done ? (int64_t)done : -4;
            __asm__ volatile("pause");
        }
        size_t space = PTY_BUF_SIZE - p->s2m_count;
        size_t chunk = (size - done) < space ? (size - done) : space;
        for (size_t i = 0; i < chunk; i++) {
            p->s2m_buf[p->s2m_head] = src[done + i];
            p->s2m_head = (p->s2m_head + 1) % PTY_BUF_SIZE;
        }
        p->s2m_count += (uint32_t)chunk;
        done += chunk;
    }
    return (int64_t)done;
}
