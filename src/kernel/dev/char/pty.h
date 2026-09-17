/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * pty.h - PTY (pseudo-terminal) subsystem for EigenOS.
 */

#ifndef KERNEL_PTY_H
#define KERNEL_PTY_H

#include <stdint.h>
#include <stddef.h>
#include "fs/vfs/vfs.h"

#define PTY_BUF_SIZE    (64 * 1024)  /* ring buffer size, same as pipe */
#define PTY_MAX_CC      20           /* NCCS */
#define PTY_MAX_PAIRS   64           /* max concurrent PTY pairs */
#define PTY_MAX_NUM     PTY_MAX_PAIRS

/* Termios c_iflag bits */
#define PTY_IGNBRK   0x0001
#define PTY_BRKINT   0x0002
#define PTY_PARMRK   0x0008
#define PTY_INPCK    0x0010
#define PTY_ISTRIP   0x0020
#define PTY_INLCR    0x0040
#define PTY_IGNCR    0x0080
#define PTY_ICRNL    0x0100
#define PTY_IXON     0x0400
#define PTY_IXOFF    0x1000

/* Termios c_oflag bits */
#define PTY_OPOST    0x0001
#define PTY_ONLCR    0x0004

/* Termios c_cflag bits */
#define PTY_CS8      0x0030
#define PTY_CREAD    0x0080
#define PTY_HUPCL    0x0400

/* Termios c_lflag bits (extend TTY_LFLAG_*) */
#define PTY_ICANON   0x0002
#define PTY_ISIG     0x0001
#define PTY_ECHO     0x0008
#define PTY_ECHOE    0x0010

struct pty_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[PTY_MAX_CC];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

struct pty_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

struct pty_pair {
    int      used;              /* 1 if this slot is allocated */
    uint32_t num;               /* PTY number N in /dev/pts/N */

    /* Ring buffer: master writes here, slave reads from here */
    uint8_t  *m2s_buf;          /* master-to-slave ring buffer */
    uint32_t  m2s_head;
    uint32_t  m2s_tail;
    uint32_t  m2s_count;
    int       m2s_readers;      /* usually 1 (slave) */
    int       m2s_writers;      /* usually 1 (master) */

    /* Ring buffer: slave writes here, master reads from here */
    uint8_t  *s2m_buf;          /* slave-to-master ring buffer */
    uint32_t  s2m_head;
    uint32_t  s2m_tail;
    uint32_t  s2m_count;
    int       s2m_readers;      /* usually 1 (master) */
    int       s2m_writers;      /* usually 1 (slave) */

    /* Slave VFS node */
    struct vfs_node slave_node;

    /* Per-PTY state */
    struct pty_termios termios;
    struct pty_winsize winsize;

    /* Process group that controls this PTY (for hangup signals) */
    uint32_t pgid;
    uint32_t session;

    /* Lock state (grantpt/unlockpt) */
    int locked;

    /* Slave ownership (grantpt sets these) */
    uint32_t uid;
    uint32_t gid;
};

/*
 * Initialize the PTY subsystem. Called during kernel boot.
 */
void pty_init(void);

/*
 * Allocate a new PTY pair. Returns the PTY number (N for /dev/pts/N),
 * or -1 on failure. The caller must install the master as an anonymous
 * fd in the current process's fd table.
 */
int pty_alloc(void);

/*
 * Release a PTY pair. Called when both master and slave fds are closed.
 */
void pty_release(int num);

/*
 * Get the PTY pair by number. Returns NULL if invalid.
 */
struct pty_pair *pty_get(int num);

/*
 * Called when the master fd is closed. Sends SIGHUP to the PTY's
 * controlling process group if it has one.
 */
void pty_master_close(int num);

/*
 * Get the slave VFS node for /dev/pts/N. Returns NULL if N is invalid.
 */
struct vfs_node *pty_slave_node(int num);

/*
 * Get the termios for a PTY pair.
 */
struct pty_termios *pty_get_termios(int num);

/*
 * Set the termios for a PTY pair.
 */
int pty_set_termios(int num, const struct pty_termios *t);

/*
 * Get/Set the window size for a PTY pair.
 */
struct pty_winsize *pty_get_winsize(int num);
int pty_set_winsize(int num, const struct pty_winsize *w);

/*
 * grantpt: mark the slave as owned by the calling process's UID.
 */
int pty_grantpt(int num);

/*
 * unlockpt: clear the slave's lock bit.
 */
int pty_unlockpt(int num);

/*
 * ptsname: write "/dev/pts/N" into buf. Returns 0 on success.
 */
int pty_ptsname(int num, char *buf, size_t bufsize);

/*
 * PTY master operations (called from VFS when /dev/ptmx is opened).
 */
int pty_master_open(int *out_num);
int64_t pty_master_read(int num, uint64_t offset, size_t size, void *buf);
int64_t pty_master_write(int num, uint64_t offset, size_t size, const void *buf);

/*
 * PTY slave operations (called from VFS when /dev/pts/N is opened).
 */
int pty_slave_open(struct vfs_node *node, int flags);
int pty_slave_close(struct vfs_node *node);
int64_t pty_slave_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf);
int64_t pty_slave_write(struct vfs_node *node, uint64_t offset, size_t size, const void *buf);

/* Dup a master fd (fork/dup path bumps counts so close is safe). */
int pty_master_dup(int num);

/* PTY ioctl dispatch (called from the syscall layer after fd routing).
 * Handles TIOCGPTN/TIOCSPTLCK plus per-pair termios and winsize.
 * Returns 1 when cmd is not a PTY ioctl (caller runs normal dispatch). */
int64_t pty_ioctl(struct pty_pair *pp, int is_master, unsigned long cmd,
                  uint64_t arg);

#endif /* KERNEL_PTY_H */
