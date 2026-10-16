/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * vfs.h - Virtual File System definitions and POSIX abstractions.
 */

#ifndef KERNEL_VFS_H
#define KERNEL_VFS_H

#include <stdint.h>
#include <stddef.h>

/* Node Types */
#define VFS_FILE        0x01
#define VFS_DIRECTORY   0x02
#define VFS_CHARDEVICE  0x03
#define VFS_BLOCKDEVICE 0x04
#define VFS_PIPE        0x05
#define VFS_SYMLINK     0x06
#define VFS_SOCKET      0x07
#define VFS_MOUNTPOINT  0x08

/* POSIX Open Flags */
#define O_RDONLY    00000000
#define O_WRONLY    00000001
#define O_RDWR      00000002
#define O_CREAT     00000100
#define O_EXCL      00000200
#define O_TRUNC     00001000
#define O_APPEND    00002000

/* Lseek Whence */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* Poll event flags (Linux ABI) */
#define POLLIN      0x001
#define POLLPRI     0x002
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010
#define POLLNVAL    0x020

/* Tty line-discipline flags (termios c_lflag subset) */
#define TTY_LFLAG_ISIG    0x0001
#define TTY_LFLAG_ICANON  0x0002
#define TTY_LFLAG_ECHO    0x0008

#define MAX_PATH_LEN    256
#define MAX_FD_COUNT    256

struct vfs_dirent {
    char name[128];
    uint64_t ino;
    uint32_t type;
    uint64_t size;
};

struct vfs_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime_sec;
    int64_t  st_atime_nsec;
    int64_t  st_mtime_sec;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime_sec;
    int64_t  st_ctime_nsec;
    int64_t  __unused[3];
};

struct vfs_node {
    char name[128];
    uint32_t type;
    uint32_t flags;
    uint32_t permissions;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t inode;

    void *device; /* Filesystem-specific private data */

    /* Node operations */
    int64_t (*read)(struct vfs_node *node, uint64_t offset, size_t size, void *buf);
    int64_t (*write)(struct vfs_node *node, uint64_t offset, size_t size, const void *buf);
    int (*open)(struct vfs_node *node, int flags);
    int (*close)(struct vfs_node *node);
    struct vfs_dirent *(*readdir)(struct vfs_node *node, uint32_t index);
    struct vfs_node *(*finddir)(struct vfs_node *node, const char *name);
    int64_t (*readlink)(struct vfs_node *node, char *buf, size_t size);
    int (*create)(struct vfs_node *parent, const char *name, uint16_t mode);
    int (*mkdir)(struct vfs_node *parent, const char *name, uint16_t mode);
    int (*mksock)(struct vfs_node *parent, const char *name, uint16_t mode);
    int (*unlink)(struct vfs_node *parent, const char *name);
    int (*rmdir)(struct vfs_node *parent, const char *name);
    int (*truncate)(struct vfs_node *node, int64_t length);
    int (*ioctl)(struct vfs_node *node, unsigned long cmd, void *arg);
    int (*poll)(struct vfs_node *node, int events);

    /* Optional mmap hook for device nodes (fb0, mem). NULL means "not
     * mappable": the VFS routes that fd to the anonymous path. */
    int64_t (*mmap)(struct vfs_node *node, uint64_t vaddr, uint64_t size,
                    uint64_t offset, uint64_t pte_flags);

    /* Mount point redirection */
    struct vfs_node *mount_target;

    /* Symlink target path (valid when type == VFS_SYMLINK) */
    char linktarget[128];
};

struct file_descriptor {
    struct vfs_node *node;
    uint64_t offset;
    int flags;
    int is_used;
    int sock_alias;      /* Socket alias: -1 when backed by node */
    int pty_num;         /* PTY number: -1 when not a PTY master fd */
};

/* Core VFS Functions */
void vfs_init(void);
void vfs_mount_root(struct vfs_node *root);
int vfs_mount(const char *mountpoint, struct vfs_node *fs_root);
/* Mount with a recorded fstype/src label for /proc/mounts. */
int vfs_mount_named(const char *mountpoint, struct vfs_node *fs_root,
                    const char *fstype, const char *src);
struct vfs_node *vfs_lookup(const char *path);
struct vfs_node *devfs_root(void);
/* Allocate a user fd naming kernel socket `sock_idx`. This is what makes
 * sockets ordinary file descriptors: dup2 can move them to fd 0 (busybox
 * ping does exactly that), close frees the alias, and read/write/poll
 * resolve through the fd table like every other descriptor. */
int vfs_install_socket(int sock_idx);

/* Install an already-built node as a new fd. Virtual filesystems that create
 * inodes without a path (memfd/shmem, epoll instances) need this: the node
 * exists, it just has no directory entry. */
int vfs_install_node(struct vfs_node *node);

/* Status flags of the descriptor currently inside a node read op; 0 when no
 * read is in flight. Device read ops that must honour O_NONBLOCK consult
 * this, since the op itself only receives (node, offset, size, buf). */
int vfs_read_flags(void);
int vfs_write_flags(void);
struct vfs_node *vfs_get_node(int fd);

/* Publish an externally-owned device node into /dev. The table entry is the
 * live node userspace reaches through path lookups, so drivers keep the
 * implementation and devfs keeps the directory. */
int devfs_attach(struct vfs_node *node);
struct vfs_node *tmpfs_root(void);
struct vfs_node *tmpfs_new_root(const char *name);
struct vfs_node *procfs_root(void);
void vfs_normalize(const char *path, char *out, size_t out_size);

/* Map a file-backed range: device hook first, anonymous read-through after.
 * Returns 0 on success, negative errno on failure. */
int vfs_mmap_fd(int fd, uint64_t vaddr, uint64_t size, uint64_t offset,
                uint64_t pte_flags);

/* High-level File Operations */
int vfs_open(const char *path, int flags, int mode);
int64_t vfs_read(int fd, void *buf, size_t count);
int64_t vfs_write(int fd, const void *buf, size_t count);
int vfs_close(int fd);
int64_t vfs_lseek(int fd, int64_t offset, int whence);
int vfs_stat(const char *path, struct vfs_stat *st);
int vfs_lstat(const char *path, struct vfs_stat *st);
int64_t vfs_readlink(const char *path, char *buf, size_t size);
int vfs_fstat(int fd, struct vfs_stat *st);
struct vfs_statfs {
    int64_t f_type;
    int64_t f_bsize;
    int64_t f_blocks;
    int64_t f_bfree;
    int64_t f_bavail;
    int64_t f_files;
    int64_t f_ffree;
    int32_t f_fsid[2];
    int64_t f_namelen;
    int64_t f_frsize;
    int64_t f_flags;
    int64_t f_spare[4];
};
int vfs_statfs(const char *path, struct vfs_statfs *st);
int vfs_fstatfs(int fd, struct vfs_statfs *st);
int64_t vfs_getdents64(int fd, void *dirp, size_t count);
int vfs_dup(int oldfd);
int vfs_dup2(int oldfd, int newfd);
int vfs_fcntl_dupfd(int oldfd, int min_fd);
int vfs_fcntl_getfl(int fd);
int vfs_fcntl_setfl(int fd, int flags);
int vfs_pipe(int pipefd[2]);
int vfs_poll(int fd, int events);
int vfs_is_seekable(int fd);
int vfs_is_tty(int fd);
int vfs_pty_master_num(int fd);
int vfs_pty_slave_num(int fd);
/* Resolve fd to a numeric kernel socket fd (direct or alias), or -1. */
int vfs_sock_fd(int fd);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);
int vfs_mkdir(const char *path, int mode);
int vfs_mksock(const char *path);
int vfs_chmod(const char *path, int mode);
int vfs_fchmod(int fd, int mode);
int vfs_truncate(const char *path, int64_t length);
int vfs_ftruncate(int fd, int64_t length);

/* Tty line-discipline state (shared console) */
uint32_t tty_get_lflag(void);
uint8_t tty_get_vmin(void);
uint8_t tty_get_vtime(void);
void tty_set_termios(uint32_t lflag, uint8_t vmin, uint8_t vtime);
void tty_handle_signal(char c);
int has_unblocked_pending(void);
/* libvterm keyboard path: queue one decoded byte for stdin_read. */
void vfs_tty_qput(char c);
int vfs_tty_poll(int events);

/* Kernel RNG (RDRAND with xorshift fallback) */
void rng_fill(void *buf, size_t len);

/* Per-process descriptor table management */
void vfs_install_stdio(struct file_descriptor *table);
void vfs_clone_table(struct file_descriptor *dst, struct file_descriptor *src);
void vfs_close_all(struct file_descriptor *table);

#endif /* KERNEL_VFS_H */
