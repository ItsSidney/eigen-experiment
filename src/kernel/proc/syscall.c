/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * syscall.c - System Call dispatcher implementing Linux x86_64 ABI.
 */

#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "process.h"
#include "mm/pmm.h"
#include "mm/shmem.h"
#include "fs/epoll/epoll.h"
#include "dev/chan/chanfd.h"
#include "mm/vmm.h"
#include "fs/vfs/vfs.h"
#include "include/serial.h"
#include "drivers/timer/pit.h"
#include "drivers/char/console.h"
#include "dev/fb/fb.h"
#include "dev/char/pty.h"
#include "net/net.h"
#include "net/socket.h"

extern int is_socket_fd(int fd);

/* Sleep until the next hardware interrupt (IF is set on syscall entry, so
 * timer and keyboard IRQs wake us). Used by indefinite readiness waits so
 * they coast instead of pause-spinning; the waiter re-checks vfs_poll on
 * wake and also notices pending signals. */
static inline void hlt_until_irq_or_read(void) {
    __asm__ volatile("hlt" ::: "memory");
}

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

#define MSR_FS_BASE 0xC0000100
#define MSR_GS_BASE 0xC0000101

extern void syscall_enable_msrs(void);

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void syscall_init(void) {
    syscall_enable_msrs();
    serial_puts("[SYSCALL] Fast syscall interface initialized (EFER.SCE, STAR, LSTAR, SFMASK set).\n");
}

/* ── Memory Management Syscalls ─────────────────────────────────────────── */

static uint64_t sys_brk(uint64_t new_brk) {
    if (!current_process) return 0;
    if (new_brk == 0 || new_brk < current_process->brk_base) {
        return current_process->brk;
    }

    uint64_t cur_aligned = (current_process->brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t new_aligned = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (new_aligned > cur_aligned) {
        for (uint64_t v = cur_aligned; v < new_aligned; v += PAGE_SIZE) {
            void *p = pmm_alloc_page();
            if (!p) {
                return current_process->brk;
            }
            uint8_t *dest = (uint8_t *)(0xFFFF800000000000ULL + (uint64_t)p);
            for (int i = 0; i < 4096; i++) dest[i] = 0;

            vmm_map_page(current_process->pml4, v, (uint64_t)p, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
        }
    } else if (new_aligned < cur_aligned) {
        for (uint64_t v = new_aligned; v < cur_aligned; v += PAGE_SIZE) {
            uint64_t phys = vmm_virt_to_phys(current_process->pml4, v);
            if (phys) {
                pmm_free_page((void *)(phys & PAGE_ADDR_MASK));
                vmm_unmap_page(current_process->pml4, v);
            }
        }
    }

    current_process->brk = new_brk;
    return current_process->brk;
}

static int64_t sys_mmap(uint64_t addr, uint64_t length, int prot, int flags, int fd, int64_t offset) {
    if (!current_process || length == 0) return -22; /* -EINVAL */

    uint64_t size = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t vaddr = addr;

    /* MAP_FIXED is 0x10 */
    if (!(flags & 0x10) || vaddr == 0) {
        vaddr = current_process->mmap_next;
        current_process->mmap_next += size;
        if (current_process->mmap_next >= USER_MMAP_END) {
            return -12; /* -ENOMEM */
        }
    } else {
        vaddr &= ~(PAGE_SIZE - 1);
    }

    uint64_t pte_flags = PAGE_PRESENT | PAGE_USER;
    if (prot & 0x02) { /* PROT_WRITE */
        pte_flags |= PAGE_WRITABLE;
    }

    /* File-backed mapping. A device node may back the range itself (fb0 maps
     * the scanout pages); otherwise the content is preloaded through the fd,
     * which needs the pages writable while the kernel fills them. */
    int preload = 0;
    if (!(flags & 0x20) && fd >= 0) { /* Not MAP_ANONYMOUS */
        if (vfs_mmap_fd(fd, vaddr, size, (uint64_t)offset, pte_flags) == 0)
            return (int64_t)vaddr;
        preload = 1;
    }

    /* Filling a PROT_READ mapping (musl maps /etc/localtime and other zone
     * files that way) must not write through a read-only PTE: map writable,
     * copy, then restore the caller's protection. */
    uint64_t fill_flags = pte_flags | (preload ? PAGE_WRITABLE : 0);
    for (uint64_t v = vaddr; v < vaddr + size; v += PAGE_SIZE) {
        void *p = pmm_alloc_page();
        if (!p) return -12; /* -ENOMEM */
        uint8_t *dest = (uint8_t *)(0xFFFF800000000000ULL + (uint64_t)p);
        for (int i = 0; i < 4096; i++) dest[i] = 0;

        vmm_map_page(current_process->pml4, v, (uint64_t)p, fill_flags);
    }

    if (preload) {
        vfs_lseek(fd, offset, SEEK_SET);
        vfs_read(fd, (void *)vaddr, length);
        /* Restore the caller's protection: the temporary write access was
         * only for filling the range. */
        if (!(pte_flags & PAGE_WRITABLE)) {
            for (uint64_t v = vaddr; v < vaddr + size; v += PAGE_SIZE) {
                uint64_t phys = vmm_virt_to_phys(current_process->pml4, v);
                if (phys)
                    vmm_map_page(current_process->pml4, v,
                                 phys & PAGE_ADDR_MASK, pte_flags);
            }
        }
    }

    return (int64_t)vaddr;
}

static int64_t sys_munmap(uint64_t addr, uint64_t length) {
    if (!current_process || (addr & (PAGE_SIZE - 1)) != 0 || length == 0) {
        return -22; /* -EINVAL */
    }

    uint64_t size = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t v = addr; v < addr + size; v += PAGE_SIZE) {
        uint64_t phys = vmm_virt_to_phys(current_process->pml4, v);
        if (phys) {
            pmm_free_page((void *)(phys & PAGE_ADDR_MASK));
            vmm_unmap_page(current_process->pml4, v);
        }
    }
    return 0;
}

static int64_t sys_mprotect(uint64_t addr, uint64_t length, int prot) {
    if (!current_process || (addr & (PAGE_SIZE - 1)) != 0) {
        return -22; /* -EINVAL */
    }

    uint64_t size = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t pte_flags = PAGE_PRESENT | PAGE_USER;
    if (prot & 0x02) {
        pte_flags |= PAGE_WRITABLE;
    }

    for (uint64_t v = addr; v < addr + size; v += PAGE_SIZE) {
        uint64_t phys = vmm_virt_to_phys(current_process->pml4, v);
        if (phys) {
            vmm_map_page(current_process->pml4, v, phys & PAGE_ADDR_MASK, pte_flags);
        }
    }
    return 0;
}

/* ── futex ───────────────────────────────────────────────────────────────
 * A futex here is a single atomic 32-bit word with a waiter count, exactly
 * the shape libwayland's and musl's mutexes need on a single-core,
 * non-preemptive kernel: lock acquisition can only ever race with a signal
 * handler, so spinning on the word with a pause is correct. What the table
 * cannot do is block, which FUTEX_WAIT would require; it returns EAGAIN
 * instead (spurious wakeup), which is the documented "retry" contract, and
 * WAIT_BITSET gets EINVAL because the kernel has no bitset waiters. FUTEX_WAKE
 * wakes by count: everything here wakes immediately, so it returns the number
 * of nominal waiters (0) unless the value mismatches.
 *
 * Two operations, both single-instruction on x86_64:
 *   FUTEX_WAIT (0):    if *uaddr == val, return -EAGAIN (wake spuriously);
 *                      if it differs, -EAGAIN immediately (Linux value).
 *   FUTEX_WAKE (1):    return 0 (no one to wake - everything spins).
 *   FUTEX_FD/REQUEUE/CMP_REQUEUE, PRIVATE_FLAG variants: mapped onto the
 *   non-private forms, since address space identity is trivially decidable
 *   here (per-process address spaces, kernel shares none). */

#define FUTEX_WAIT        0
#define FUTEX_WAKE        1
#define FUTEX_FD          2
#define FUTEX_REQUEUE     3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP     5
#define FUTEX_LOCK_PI     6
#define FUTEX_UNLOCK_PI   7
#define FUTEX_TRYLOCK_PI  8
#define FUTEX_WAIT_BITSET 9
#define FUTEX_PRIVATE_FLAG 128

static int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val,
                         const struct ktimespec *timeout,
                         uint32_t *uaddr2, uint32_t val3) {
    (void)timeout;
    (void)uaddr2;
    (void)val3;
    if (!uaddr) return -14; /* -EFAULT */

    int cmd = op & 0x7F;
    if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
        /* Report the classic spurious wakeup: value matches, but we have no
         * wait queue to park on. The caller retries the lock atomically. */
        uint32_t cur = *uaddr;
        if (cur != val) return -11; /* -EAGAIN: value changed underneath */
        return -11;                 /* -EAGAIN: spurious wakeup */
    }
    if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_OP) {
        /* Nothing blocks, so nothing wakes; the count argument is advisory. */
        (void)val;
        return 0;
    }
    return -38; /* -ENOSYS: PI ops, FD, requeue need a real wait queue */
}

/* ── file locks ──────────────────────────────────────────────────────────
 * A small cooperative lock table for flock(2). libwayland's socket lock is
 * the visible consumer: it opens /tmp/wayland-N.lock and takes LOCK_EX |
 * LOCK_NB; without a table every acquisition returned -ENOSYS, which the
 * caller reads as "another compositor is running" and walks through all 33
 * display names before giving up.
 *
 * Entries are identified by the inode number of the underlying node, so two
 * fds opened on the same path contend correctly, and by owner pid so a
 * process's locks are released when its last fd on the file is closed.
 * LOCK_SH shares; LOCK_EX is exclusive; LOCK_NB makes a collision return
 * -EWOULDBLOCK instead of parking (a blocking flock has no parking spot in
 * the deferred model); LOCK_UN drops it. */

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

#define FLOCK_TABLE_MAX 32

struct flock_entry {
    struct vfs_node *node;   /* NULL = free */
    int              fd;
    int              owner_pid;
    int              mode;   /* LOCK_SH or LOCK_EX */
};

static struct flock_entry g_flocks[FLOCK_TABLE_MAX];

static int vfs_flock(int fd, int op) {
    struct file_descriptor *fd_table = current_process ? current_process->fds
                                                       : NULL;
    if (!fd_table || fd < 0 || fd >= MAX_FD_COUNT || !fd_table[fd].is_used)
        return -9; /* -EBADF */
    struct vfs_node *node = fd_table[fd].node;
    if (!node) return -9;

    if (op & LOCK_UN) {
        for (int i = 0; i < FLOCK_TABLE_MAX; i++) {
            if (g_flocks[i].node == node && g_flocks[i].fd == fd) {
                g_flocks[i].node = NULL;
                return 0;
            }
        }
        return 0; /* POSIX: unlocking an unlocked file succeeds */
    }

    int want_ex = (op & LOCK_EX) != 0;

    /* Already held by this fd? Refresh the mode. */
    for (int i = 0; i < FLOCK_TABLE_MAX; i++) {
        if (g_flocks[i].node == node && g_flocks[i].fd == fd) {
            if ((g_flocks[i].mode == LOCK_EX) != want_ex) return -22;
            return 0;
        }
    }

    /* POSIX: existing shared locks block an exclusive request; existing
     * exclusive locks block everything. Check before claiming a slot. */
    for (int i = 0; i < FLOCK_TABLE_MAX; i++) {
        if (g_flocks[i].node != node) continue;
        int held_ex = (g_flocks[i].mode == LOCK_EX);
        if (held_ex || want_ex) {
            if (op & LOCK_NB) return -11; /* -EWOULDBLOCK */
            return -11; /* no parking in the deferred model */
        }
        /* SH over SH falls through and shares. */
    }

    for (int i = 0; i < FLOCK_TABLE_MAX; i++) {
        if (!g_flocks[i].node) {
            g_flocks[i].node      = node;
            g_flocks[i].fd        = fd;
            g_flocks[i].owner_pid = current_process ? (int)current_process->pid : 0;
            g_flocks[i].mode      = want_ex ? LOCK_EX : LOCK_SH;
            return 0;
        }
    }
    return -3; /* -ESRCH: table full; Linux would block here too */
}

/* ── Vector and File Syscalls ───────────────────────────────────────────── */

/* ── sendmsg/recvmsg ─────────────────────────────────────────────────────
 * musl's struct msghdr keeps msg_iovlen and msg_controllen as 32-bit fields
 * with explicit padding (glibc uses size_t), so the offsets are spelled out
 * rather than mirrored from a header: getting them wrong silently shifts
 * every field after msg_name.
 *
 * A datagram has to leave as exactly one packet, so a multi-iovec message is
 * gathered before it is handed to the socket layer. Streams could go out
 * piecewise instead, but one code path is worth the copy at these sizes. */
#define MSG_IOV_TOTAL_MAX 8192

struct kmsghdr {
    void    *msg_name;       /* +0  */
    uint32_t msg_namelen;    /* +8  */
    uint32_t __pad0;         /* +12 */
    void    *msg_iov;        /* +16 */
    uint32_t msg_iovlen;     /* +24 */
    uint32_t __pad1;         /* +28 */
    void    *msg_control;    /* +32 */
    uint32_t msg_controllen; /* +40 */
    uint32_t __pad2;         /* +44 */
    uint32_t msg_flags;      /* +48 */
    uint32_t __pad3;         /* +52 */
};

static int64_t sys_sendmsg(int fd, const struct kmsghdr *msg, int flags) {
    if (!msg) return -14; /* -EFAULT */
    const struct iovec *iov = (const struct iovec *)msg->msg_iov;
    uint32_t n = msg->msg_iovlen;
    if (!iov || n == 0) return -22; /* -EINVAL */

    size_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!iov[i].iov_base && iov[i].iov_len) return -14;
        total += iov[i].iov_len;
        if (total > MSG_IOV_TOTAL_MAX) return -90; /* -EMSGSIZE */
    }

    static uint8_t gather[MSG_IOV_TOTAL_MAX];
    size_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *s = (const uint8_t *)iov[i].iov_base;
        for (size_t k = 0; k < iov[i].iov_len; k++) gather[off + k] = s[k];
        off += iov[i].iov_len;
    }

    return sys_sock_sendto(fd, gather, total, flags,
                           (const struct sockaddr_in *)msg->msg_name,
                           msg->msg_namelen);
}

static int64_t sys_recvmsg(int fd, struct kmsghdr *msg, int flags) {
    if (!msg) return -14; /* -EFAULT */
    struct iovec *iov = (struct iovec *)msg->msg_iov;
    uint32_t n = msg->msg_iovlen;
    if (!iov || n == 0) return -22; /* -EINVAL */

    size_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!iov[i].iov_base && iov[i].iov_len) return -14;
        total += iov[i].iov_len;
    }
    if (total > MSG_IOV_TOTAL_MAX) total = MSG_IOV_TOTAL_MAX;

    static uint8_t scatterbuf[MSG_IOV_TOTAL_MAX];
    uint32_t namelen = msg->msg_namelen;
    int64_t got = sys_sock_recvfrom(fd, scatterbuf, total, flags,
                                    msg->msg_name
                                        ? (struct sockaddr_in *)msg->msg_name
                                        : NULL,
                                    msg->msg_name ? &namelen : NULL);
    if (got < 0) return got;

    if (msg->msg_name) msg->msg_namelen = namelen;
    msg->msg_flags      = 0;
    msg->msg_controllen = 0;

    /* Scatter what arrived across the caller's iovecs, in order. */
    size_t done = 0;
    for (uint32_t i = 0; i < n && done < (size_t)got; i++) {
        size_t take = (size_t)got - done;
        if (take > iov[i].iov_len) take = iov[i].iov_len;
        uint8_t *d = (uint8_t *)iov[i].iov_base;
        for (size_t k = 0; k < take; k++) d[k] = scatterbuf[done + k];
        done += take;
    }
    return got;
}

static int64_t sys_writev(int fd, const struct iovec *iov, int iovcnt) {
    if (iovcnt < 0 || !iov) return -22; /* -EINVAL */
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) continue;
        if (!iov[i].iov_base) return -14; /* -EFAULT */
        int64_t written = vfs_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (written < 0) return written;
        total += written;
    }
    return total;
}

static int64_t sys_uname(struct utsname *buf) {
    if (!buf) return -14; /* -EFAULT */
    static const char s_sysname[]  = "Eigen";
    static const char s_nodename[] = "eigen-experimental";
    static const char s_release[]  = "0.1-alpha";
    static const char s_version[]  = "Eigen Monolithic x86_64";
    static const char s_machine[]  = "x86_64";
    static const char s_domain[]   = "(none)";

    char *p;
    p = buf->sysname;    for (int i = 0; s_sysname[i]; i++)  *p++ = s_sysname[i];  *p = '\0';
    p = buf->nodename;   for (int i = 0; s_nodename[i]; i++) *p++ = s_nodename[i]; *p = '\0';
    p = buf->release;    for (int i = 0; s_release[i]; i++)  *p++ = s_release[i];  *p = '\0';
    p = buf->version;    for (int i = 0; s_version[i]; i++)  *p++ = s_version[i];  *p = '\0';
    p = buf->machine;    for (int i = 0; s_machine[i]; i++)  *p++ = s_machine[i];  *p = '\0';
    p = buf->domainname; for (int i = 0; s_domain[i]; i++)   *p++ = s_domain[i];   *p = '\0';
    return 0;
}

static int64_t sys_access(const char *pathname, int mode) {
    (void)mode;
    if (!pathname) return -14; /* -EFAULT */
    struct vfs_node *node = vfs_lookup(pathname);
    if (!node) return -2; /* -ENOENT */
    return 0;
}

static int64_t sys_getcwd(char *buf, size_t size) {
    if (!buf) return -14; /* -EFAULT */
    const char *cwd = "/";
    if (current_process && current_process->cwd[0] != '\0') {
        cwd = current_process->cwd;
    }
    size_t len = 0;
    while (cwd[len]) len++;
    if (size < len + 1) return -34; /* -ERANGE */
    for (size_t i = 0; i <= len; i++) buf[i] = cwd[i];
    return (int64_t)buf;
}

/* ── Process lifecycle (deferred run-to-completion model) ───────────────── */

extern uint64_t current_syscall_frame;

#define CLONE_VM      0x00000100ULL
#define CLONE_FS      0x00000200ULL
#define CLONE_FILES   0x00000400ULL
#define CLONE_SIGHAND 0x00000800ULL
#define CLONE_THREAD  0x00010000ULL

static int64_t sys_do_wait4(int64_t pid, int *status, int options, void *rusage) {
    if (!current_process) return -10; /* -ECHILD */

    /* Find a matching child; prefer one that already exited. */
    struct process *target = NULL;
    for (struct process *p = process_iter(NULL); p; p = process_iter(p)) {
        if (p->parent != current_process) continue;
        if (pid > 0 && (int64_t)p->pid != pid) continue;
        /* pid == 0: any child in our group (approximated as any child);
         * pid == -1: any child */
        target = p;
        if (p->exited || pid > 0) break;
    }
    /* WNOHANG callers poll for finished children; returning 0 (nothing
     * ready) instead of -ECHILD when the list is empty prevents shells
     * from crashing on their post-reap job-control sweep.
     * WUNTRACED/WCONTINUED are accepted and ignored: nothing in the kernel
     * sets p->stopped yet (SIGSTOP delivery lands in Step 5), so there are
     * no stop/continue events to report. */
    if (!target) {
        if (options & 1) return 0; /* WNOHANG: no children at all */
        return -10; /* -ECHILD */
    }

    if (!target->exited) {
        if (options & 1) return 0; /* WNOHANG: still pending */
        /* Deferred execution: run the child to completion, nested. */
        process_run_deferred(target);
    }

    /* Linux packs wait status in the low 16 bits: WIFEXITED → exit code in
     * bits 15:8, WIFSIGNALED → signal number in bits 6:0 + core bit 7.
     * (Our older code stored the full 32-bit exit code raw, which confuses
     * ash's job handling right after a non-zero exit.) */
    if (status) {
        if (target->exit_status >= 128) {
            int sig = (target->exit_status - 128) & 0x7F;
            *status = sig | (sig == 11 ? 0x80 : 0); /* WIFSIGNALED (+core) */
        } else {
            *status = (target->exit_status & 0xFF) << 8; /* WIFEXITED */
        }
    }
    if (rusage) {
        /* No preemption accounting yet: report zero CPU time (valid POSIX,
         * same layout as sys_getrusage). musl's wait4 writes through this. */
        uint64_t *u = (uint64_t *)rusage;
        for (int i = 0; i < 16; i++) u[i] = 0;
    }
    target->parent = NULL; /* reaped */
    process_reap(target);
    return (int64_t)target->pid;
}

/* Read the first bytes of a file node (kernel context, safe for scripts) */
static int64_t node_read_head(struct vfs_node *f, void *buf, size_t len) {
    if (!f || !f->read) return -1;
    return f->read(f, 0, (int64_t)len, buf);
}

/* If 'path' is a "#!" script, resolve interpreter + optional argument.
 * Returns 1 and fills interp/iarg on success, 0 if not a script. */
static int resolve_shebang(const char *path, char *interp, size_t interpsz,
                           char *iarg, size_t iargsz) {
    struct vfs_node *f = vfs_lookup(path);
    if (!f) return 0;

    char head[256];
    int64_t n = node_read_head(f, head, sizeof(head));
    if (n < 2 || head[0] != '#' || head[1] != '!') return 0;

    /* Parse the shebang line: "#!/interp[ arg]\n" */
    size_t len = (n < (int64_t)sizeof(head)) ? (size_t)n : sizeof(head) - 1;
    head[len] = '\0';
    char *line = head + 2;
    while (*line == ' ' || *line == '\t') line++;

    size_t ii = 0, ai = 0;
    int in_arg = 0;
    for (size_t k = 0; line[k] && line[k] != '\n' && line[k] != '\r'; k++) {
        if (!in_arg) {
            if (line[k] == ' ' || line[k] == '\t') {
                if (ii == 0) continue;      /* leading spaces before interp */
                in_arg = 1;
                continue;
            }
            if (ii + 1 < interpsz) interp[ii++] = line[k];
        } else {
            if (line[k] == ' ' || line[k] == '\t') break; /* arg is one token */
            if (ai + 1 < iargsz) iarg[ai++] = line[k];
        }
    }
    interp[ii] = '\0';
    iarg[ai] = '\0';
    return ii > 0 ? 1 : 0;
}

static int64_t sys_execve_user(const char *upath, const char **uargv, const char **uenvp) {
    if (!upath) return -14; /* -EFAULT */

    /* Copy everything out of the user address space BEFORE exec replaces it */
    static char path[256];
    size_t i = 0;
    while (i + 1 < sizeof(path) && upath[i]) { path[i] = upath[i]; i++; }
    path[i] = '\0';

    static char argbuf[64][128];
    static const char *argv[64];
    int argc = 0;
    if (uargv) {
        while (argc < 63 && uargv[argc]) {
            const char *s = uargv[argc];
            size_t j = 0;
            while (j + 1 < sizeof(argbuf[0]) && s[j]) { argbuf[argc][j] = s[j]; j++; }
            argbuf[argc][j] = '\0';
            argv[argc] = argbuf[argc];
            argc++;
        }
    }
    argv[argc] = NULL;

    static char envbuf[24][256];
    static const char *envp[24];
    int envc = 0;
    if (uenvp) {
        while (envc < 23 && uenvp[envc]) {
            const char *s = uenvp[envc];
            size_t j = 0;
            while (j + 1 < sizeof(envbuf[0]) && s[j]) { envbuf[envc][j] = s[j]; j++; }
            envbuf[envc][j] = '\0';
            envp[envc] = envbuf[envc];
            envc++;
        }
    }
    envp[envc] = NULL;

    /* POSIX "#!" support: if the target is a script, exec its interpreter
     * with argv = [interp, (iarg), script, original argv[1..]] */
    {
        static char interp[128];
        static char iarg[128];
        if (resolve_shebang(path, interp, sizeof(interp), iarg, sizeof(iarg))) {
            static const char *sargv[66];
            static char spath[256];
            size_t si = 0;
            while (si + 1 < sizeof(spath) && path[si]) { spath[si] = path[si]; si++; }
            spath[si] = '\0';

            int sc = 0;
            sargv[sc++] = interp;
            if (iarg[0]) sargv[sc++] = iarg;
            sargv[sc++] = spath;
            for (int k = 1; k < argc && sc < 64; k++) sargv[sc++] = argv[k];
            sargv[sc] = NULL;

            /* Never returns on success (enters ring 3 at the new entry point) */
            return process_execve(current_process, interp, sc, sargv, envp);
        }
    }

    /* Never returns on success (enters ring 3 at the new entry point) */
    return process_execve(current_process, path, argc, argv, envp);
}

static int64_t sys_chdir(const char *path) {
    if (!path) return -14; /* -EFAULT */
    if (!current_process) return -1;

    /* Resolve against cwd when relative (mirrors vfs_abspath logic) */
    char abs[512];
    if (path[0] == '/') {
        size_t i = 0;
        while (path[i] && i + 1 < sizeof(abs)) { abs[i] = path[i]; i++; }
        abs[i] = '\0';
    } else {
        const char *cwd = current_process->cwd[0] ? current_process->cwd : "/";
        size_t pos = 0;
        while (cwd[pos] && pos + 1 < sizeof(abs)) { abs[pos] = cwd[pos]; pos++; }
        if (pos == 0 || abs[pos - 1] != '/') {
            if (pos + 1 >= sizeof(abs)) return -36; /* -ENAMETOOLONG */
            abs[pos++] = '/';
        }
        for (size_t i = 0; path[i] && pos + 1 < sizeof(abs); i++) abs[pos++] = path[i];
        abs[pos] = '\0';
    }

    struct vfs_node *node = vfs_lookup(abs);
    if (!node) return -2; /* -ENOENT */
    if (node->type != VFS_DIRECTORY) return -20; /* -ENOTDIR */

    size_t len = 0;
    while (abs[len] && len + 1 < sizeof(current_process->cwd)) len++;
    for (size_t i = 0; i <= len; i++) current_process->cwd[i] = abs[i];
    return 0;
}

/* ── CMOS RTC (real-time clock) ──────────────────────────────────────────── */

static uint8_t cmos_read(uint8_t reg) {
    uint8_t v;
    __asm__ volatile("outb %0, $0x70" :: "a"(reg));
    __asm__ volatile("inb $0x71, %0" : "=a"(v));
    return v;
}

static int rtc_updating(void) {
    return cmos_read(0x0A) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t b) { return (uint8_t)((b & 0x0F) + (b >> 4) * 10); }

/* Seconds since Unix epoch, read stably from the CMOS clock. */
static uint64_t rtc_unix_time(void) {
    uint8_t sec, min, hour, day, mon;
    uint16_t year, century = 0;

    while (rtc_updating()) { __asm__ volatile("pause"); }
    sec  = cmos_read(0x00);
    min  = cmos_read(0x02);
    hour = cmos_read(0x04);
    day  = cmos_read(0x07);
    mon  = cmos_read(0x08);
    year = cmos_read(0x09);
    century = cmos_read(0x32); /* ACPI century register (may be 0) */

    /* Stability re-check (carry during read) */
    if (sec != cmos_read(0x00)) return rtc_unix_time();

    uint8_t regB = cmos_read(0x0B);
    if (!(regB & 0x04)) { /* BCD mode */
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin(hour & 0x7F);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        year = bcd_to_bin((uint8_t)year);
        century = bcd_to_bin((uint8_t)century);
    }
    if (!(regB & 0x02)) { /* 12-hour mode */
        uint8_t pm = hour & 0x80;
        hour &= 0x7F;
        if (pm && hour != 12) hour += 12;
        if (!pm && hour == 12) hour = 0;
    }

    year += century ? century * 100 : (year < 100 ? 2000 : 0);

    /* Days from 1970-01-01 to y/m/d (Howard Hinnant's civil algorithm) */
    int64_t y = year, m = mon, d = day;
    if (m <= 2) y -= 1;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + (int64_t)doe - 719468;

    return (uint64_t)(days * 86400 + hour * 3600 + min * 60 + sec);
}

/* ── Time, Vector-I/O, and System Info ───────────────────────────────────── */

#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1
#define CLOCK_BOOTTIME   7
#define CLOCK_REALTIME_COARSE  2
#define CLOCK_MONOTONIC_COARSE 6

static uint64_t sys_uptime_sec(void) {
    return timer_get_ticks() / 100; /* PIT runs at 100 Hz */
}

/* Wall-clock base. Captured from CMOS once and adjusted by clock_settime.
 * (The old model added uptime to a LIVE CMOS read, which double-counted every
 * second since boot - the clock ran fast by exactly the uptime. CMOS already
 * advances in real time; uptime only extends it if the base is frozen.) */
static int64_t time_base = -1;

static int64_t wall_clock_sec(void) {
    if (time_base < 0) time_base = (int64_t)rtc_unix_time();
    return time_base + (int64_t)sys_uptime_sec();
}

static int64_t sys_clock_gettime(int clk, struct ktimespec *ts) {
    if (!ts) return -14; /* -EFAULT */
    if (clk == CLOCK_MONOTONIC || clk == CLOCK_MONOTONIC_COARSE || clk == CLOCK_BOOTTIME) {
        uint64_t up = sys_uptime_sec();
        uint64_t rem_ms = (timer_get_ticks() % 100) * 10;
        ts->tv_sec = (int64_t)up;
        ts->tv_nsec = (int64_t)rem_ms * 1000000;
        return 0;
    }
    /* REALTIME (and anything unknown): frozen boot epoch + uptime */
    ts->tv_sec = wall_clock_sec();
    ts->tv_nsec = 0;
    return 0;
}

static int64_t sys_readv(int fd, const struct iovec *iov, int iovcnt) {
    if (iovcnt < 0 || !iov) return -22; /* -EINVAL */
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) continue;
        if (!iov[i].iov_base) return total > 0 ? total : -14;
        int64_t got = vfs_read(fd, iov[i].iov_base, iov[i].iov_len);
        if (got < 0) return total > 0 ? total : got;
        total += got;
        if ((size_t)got < iov[i].iov_len) break; /* short read ends the vector */
    }
    return total;
}

/* Linux struct sysinfo layout (musl-compatible) */
struct ksysinfo {
    int64_t  uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
};

static int64_t sys_sysinfo(struct ksysinfo *si) {
    if (!si) return -14; /* -EFAULT */
    uint64_t page = 4096;
    si->uptime    = (int64_t)sys_uptime_sec();
    si->loads[0]  = 0; si->loads[1] = 0; si->loads[2] = 0;
    si->totalram  = pmm_get_total_pages() * page;
    si->freeram   = pmm_get_free_pages() * page;
    si->sharedram = 0;
    si->bufferram = 0;
    si->totalswap = 0;
    si->freeswap  = 0;
    si->procs     = 0;
    for (struct process *p = process_list; p; p = p->next) {
        if (!p->exited) si->procs++;
    }
    si->totalhigh = 0;
    si->freehigh  = 0;
    si->mem_unit  = 1;
    return 0;
}

static int64_t sys_getrusage(int who, void *usage) {
    (void)who;
    if (!usage) return -14; /* -EFAULT */
    /* No preemption accounting yet: report zero CPU time (valid POSIX). */
    uint64_t *u = (uint64_t *)usage;
    for (int i = 0; i < 16; i++) u[i] = 0;
    return 0;
}

/* ── Main Dispatcher ─────────────────────────────────────────────────────── */

/* Inner dispatcher: all syscall handlers live here. */
static int64_t syscall_dispatch_impl(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    switch (num) {
        case SYS_READ:
            return vfs_read((int)a1, (void *)a2, (size_t)a3);

        case SYS_PREAD64: {
            /* pread64(fd, buf, count, offset): positional read, the file
             * offset is left untouched (save/seek/read/restore). */
            int fd = (int)a1;
            int64_t saved;
            int64_t n;
            if (!vfs_is_seekable(fd))
                return -29; /* -ESPIPE: unseekable */
            saved = vfs_lseek(fd, 0, SEEK_CUR);
            if (saved < 0)
                return -29;
            if (vfs_lseek(fd, (int64_t)a4, SEEK_SET) < 0)
                return -29;
            n = vfs_read(fd, (void *)a2, (size_t)a3);
            vfs_lseek(fd, saved, SEEK_SET);
            return n;
        }

        case SYS_PWRITE64: {
            /* pwrite64(fd, buf, count, offset): positional write. */
            int fd = (int)a1;
            int64_t saved;
            int64_t n;
            if (!vfs_is_seekable(fd))
                return -29; /* -ESPIPE: unseekable */
            saved = vfs_lseek(fd, 0, SEEK_CUR);
            if (saved < 0)
                return -29;
            if (vfs_lseek(fd, (int64_t)a4, SEEK_SET) < 0)
                return -29;
            n = vfs_write(fd, (const void *)a2, (size_t)a3);
            vfs_lseek(fd, saved, SEEK_SET);
            return n;
        }

        case SYS_WRITE:
            return vfs_write((int)a1, (const void *)a2, (size_t)a3);

        case SYS_OPEN:
            return (int64_t)vfs_open((const char *)a1, (int)a2, 0);

        case SYS_CLOSE:
            if (is_socket_fd((int)a1)) return (int64_t)sys_sock_close((int)a1);
            return (int64_t)vfs_close((int)a1);

        case SYS_STAT:
            return (int64_t)vfs_stat((const char *)a1, (struct vfs_stat *)a2);

        case SYS_FSTAT:
            return (int64_t)vfs_fstat((int)a1, (struct vfs_stat *)a2);

        case SYS_STATFS:
            return (int64_t)vfs_statfs((const char *)a1,
                                       (struct vfs_statfs *)a2);

        case SYS_FSTATFS:
            return (int64_t)vfs_fstatfs((int)a1,
                                        (struct vfs_statfs *)a2);

        case SYS_STATX: {
            /* struct statx (x86_64 Linux layout); mask bits per statx(2).
             * Supports AT_FDCWD + absolute paths (like openat) and
             * AT_EMPTY_PATH (fstat). dirfd-relative and BTIME unsupported. */
            struct kstatx_ts { int64_t tv_sec; uint32_t tv_nsec; int32_t __r; };
            struct kstatx {
                uint32_t stx_mask, stx_blksize;
                uint64_t stx_attributes;
                uint32_t stx_nlink, stx_uid, stx_gid;
                uint16_t stx_mode, __sp0[1];
                uint64_t stx_ino, stx_size, stx_blocks, stx_attributes_mask;
                struct kstatx_ts stx_atime, stx_btime, stx_ctime, stx_mtime;
                uint32_t stx_rdev_major, stx_rdev_minor;
                uint32_t stx_dev_major, stx_dev_minor;
                uint64_t stx_mnt_id, __sp2, __sp3[12];
            };
            int dfd = (int)a1;
            const char *path = (const char *)a2;
            int flags = (int)a3;
            /* unsigned mask = (unsigned)a4; (honored below via what we fill) */
            struct kstatx *sx = (struct kstatx *)a5;
            struct vfs_stat vs;
            int r;
            uint8_t *z;
            size_t zi;
            if (!sx) return -14; /* -EFAULT */
            /* SYNC-type and unknown flag bits: accepted, ignored. */
            if (flags & 0x1000) { /* AT_EMPTY_PATH: stat the fd itself */
                if (!path || path[0] != '\0') return -22;
                r = vfs_fstat(dfd, &vs);
            } else {
                int nofollow = (flags & 0x100) != 0; /* AT_SYMLINK_NOFOLLOW */
                if (!path) return -14;
                if (dfd != -100 && path[0] != '/') return -9; /* -EBADF */
                r = nofollow ? vfs_lstat(path, &vs) : vfs_stat(path, &vs);
            }
            if (r != 0) return (int64_t)r;
            z = (uint8_t *)sx;
            for (zi = 0; zi < sizeof(struct kstatx); zi++) z[zi] = 0;
            sx->stx_mask = 0x717; /* TYPE|MODE|NLINK|UID|GID|INO|SIZE|BLOCKS */
            sx->stx_blksize = (uint32_t)vs.st_blksize;
            sx->stx_nlink = (uint32_t)vs.st_nlink;
            sx->stx_uid = vs.st_uid;
            sx->stx_gid = vs.st_gid;
            sx->stx_mode = (uint16_t)vs.st_mode;
            sx->stx_ino = vs.st_ino;
            sx->stx_size = (uint64_t)vs.st_size;
            sx->stx_blocks = (uint64_t)vs.st_blocks;
            sx->stx_dev_minor = (uint32_t)vs.st_dev;
            return 0;
        }

        case SYS_LSEEK:
            return vfs_lseek((int)a1, (int64_t)a2, (int)a3);

        case SYS_MMAP:
            return sys_mmap(a1, a2, (int)a3, (int)a4, (int)a5, (int64_t)a6);

        case SYS_MPROTECT:
            return sys_mprotect(a1, a2, (int)a3);

        case SYS_MUNMAP:
            return sys_munmap(a1, a2);

        case SYS_BRK:
            return (int64_t)sys_brk(a1);

        case SYS_RT_SIGACTION: {
            /* x86_64 rt_sigaction(sig, const struct sigaction *act,
             *                     struct sigaction *old, size_t sigsetsize)
             * with the kernel layout: handler, flags, restorer, mask. */
            struct ksigaction {
                uint64_t handler;
                uint64_t flags;
                uint64_t restorer;
                uint64_t mask;
            };
            int sig = (int)a1;
            const struct ksigaction *act = (const struct ksigaction *)a2;
            struct ksigaction *old = (struct ksigaction *)a3;

            if (sig < 1 || sig > 64) return -22; /* -EINVAL */
            if (sig == 9 || sig == 19) return -22; /* SIGKILL/SIGSTOP immutable */
            if (!current_process) return -1;

            if (old) {
                old->handler = current_process->sig_handlers[sig];
                old->flags = current_process->sig_flags[sig];
                old->restorer = current_process->sig_restorer[sig];
                old->mask = 0;
            }
            if (act) {
                uint64_t h = act->handler;
                uint64_t bit = 1ULL << (sig - 1);

                if (h <= 1) { /* SIG_DFL or SIG_IGN: discard the disposition */
                    current_process->sig_handlers[sig] = h;
                    current_process->sig_flags[sig] = 0;
                    current_process->sig_restorer[sig] = 0;
                    /* POSIX: setting SIG_IGN drops pending instances. */
                    if (h == 1) current_process->sig_pending &= ~bit;
                } else {
                    current_process->sig_handlers[sig] = h;
                    current_process->sig_flags[sig] = act->flags;
                    current_process->sig_restorer[sig] = act->restorer;
                }
            }
            return 0;
        }

        case SYS_RT_SIGPROCMASK: {
            /* rt_sigprocmask(how, const sigset_t *set, sigset_t *old,
             *                size_t sigsetsize) */
            int how = (int)a1;
            const uint64_t *set = (const uint64_t *)a2;
            uint64_t *old = (uint64_t *)a3;

            if (!current_process) return -1;
            if (old) *old = current_process->sig_blocked;

            if (set) {
                /* The kernel-side mask is a single 64-bit word covering
                 * signals 1..64; libc hands us at least that much. */
                uint64_t m = *set;
                switch (how) {
                    case 0: /* SIG_BLOCK   */
                        current_process->sig_blocked |= m;
                        break;
                    case 1: /* SIG_UNBLOCK */
                        current_process->sig_blocked &= ~m;
                        break;
                    case 2: /* SIG_SETMASK */
                        current_process->sig_blocked = m;
                        break;
                    default:
                        return -22; /* -EINVAL */
                }
                /* SIGKILL/SIGSTOP can never be blocked. */
                current_process->sig_blocked &= ~((1ULL << 8) | (1ULL << 18));
            }
            return 0;
        }

        case SYS_IOCTL: {
            /* Userspace tty probes. musl's isatty() issues TCGETS; without a
             * success reply the shell runs non-interactive (silent, no prompt). */
            /* Truncate to 32 bits like Linux (unsigned int cmd): musl
             * passes req as signed int, so high-bit ioctls (TIOCGPTN)
             * arrive sign-extended without this. */
            unsigned long cmd = (uint32_t)a2;
            /* PTY pairs own their termios/winsize/lock ioctls: route master
             * and slave fds to the pair before the console defaults below. */
            {
                int mnum = vfs_pty_master_num((int)a1);
                int snum = (mnum < 0) ? vfs_pty_slave_num((int)a1) : -1;
                int pnum = (mnum >= 0) ? mnum : snum;
                if (pnum >= 0) {
                    struct pty_pair *pp = pty_get(pnum);
                    if (pp) {
                        int64_t pr = pty_ioctl(pp, mnum >= 0, cmd, a3);
                        if (pr != 1)
                            return pr;
                    }
                }
            }
            /* Check if the target node provides its own ioctl hook (e.g. sound dsp/mixer) */
            {
                struct vfs_node *vnode = vfs_get_node((int)a1);
                if (vnode && vnode->ioctl) {
                    int64_t ir = vnode->ioctl(vnode, cmd, (void *)a3);
                    if (ir != -25)
                        return ir;
                }
            }

            /* Framebuffer ioctls route to the fb0 driver regardless of the
             * fd used to issue them. Handy for the console, harmless for a
             * compositor. */
            switch (cmd) {
                case 0x4600: /* FBIOGET_VSCREENINFO */
                case 0x4601: /* FBIOPUT_VSCREENINFO */
                case 0x4602: /* FBIOGET_FSCREENINFO */
                case 0x4604: /* FBIOGETCMAP */
                case 0x4605: /* FBIOPUTCMAP */
                case 0x4611: /* FBIOBLANK */
                    return (int64_t)fb_ioctl(cmd, a3);
                case 0x5401: { /* TCGETS: report the tty's actual line state */
                    struct ktermios {
                        uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
                        uint8_t  c_line;
                        uint8_t  c_cc[32];
                        uint32_t c_ispeed, c_ospeed;
                    } *t = (struct ktermios *)a3;
                    if (!t) return -14; /* -EFAULT */
                    t->c_iflag = 0x0000A4BF; /* ICRNL|IXON|BRKINT defaults */
                    t->c_oflag = 0x00000005; /* OPOST|ONLCR */
                    t->c_cflag = 0x000000BF; /* CS8 + defaults */
                    t->c_lflag = tty_get_lflag(); /* live kernel state */
                    t->c_line  = 0;
                    for (int i = 0; i < 32; i++) t->c_cc[i] = 0;
                    t->c_cc[6] = tty_get_vmin();  /* VMIN */
                    t->c_cc[5] = tty_get_vtime(); /* VTIME */
                    t->c_ispeed = 0x00001002; /* B38400 */
                    t->c_ospeed = 0x00001002;
                    return 0;
                }
                case 0x5402: /* TCSETS: apply the caller's line discipline */
                case 0x5403: /* TCSETSW */
                case 0x5404: { /* TCSETSF */
                    const struct ktermios {
                        uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
                        uint8_t  c_line;
                        uint8_t  c_cc[32];
                        uint32_t c_ispeed, c_ospeed;
                    } *t = (const struct ktermios *)a3;
                    if (!t) return -14; /* -EFAULT */
                    tty_set_termios(t->c_lflag, t->c_cc[6] /*VMIN*/, t->c_cc[5] /*VTIME*/);
                    return 0;
                }
                case 0x5413: { /* TIOCGWINSZ: real console geometry */
                    uint16_t *ws = (uint16_t *)a3;
                    if (!ws) return -14; /* -EFAULT */
                    console_get_size(&ws[0], &ws[1]);
                    ws[2] = 0;  /* ws_xpixel */
                    ws[3] = 0;  /* ws_ypixel */
                    return 0;
                }
                case 0x540E: /* TIOCSCTTY: acquire the controlling terminal */
                    if (current_process) current_process->has_ctty = 1;
                    return 0;
                case 0x5422: /* TIOCNOTTY: give it up */
                    if (current_process) current_process->has_ctty = 0;
                    return 0;
                case 0x540F: /* TIOCGPGRP: foreground process group */
                    if (a3) *(uint32_t *)a3 = current_process ? current_process->pgid : 0;
                    return 0;
                case 0x5410: /* TIOCSPGRP: set the foreground group */
                    if (current_process && a3) current_process->pgid = *(uint32_t *)a3;
                    return 0;
                /* ── network interface ioctls (ifconfig/route/ip) ───────
                 * Linux x86_64 ABI: sizeof(struct ifreq) is 40 bytes:
                 *   ifr_name[16] @ 0..15
                 *   ifr_ifru (union sockaddr etc.) @ 16..39
                 * Only eth0 (the e1000/pcnet NIC) and lo exist. */
                case 0x8912: { /* SIOCGIFCONF: list interfaces */
                    int *lenp = (int *)a3;
                    char *buf;
                    int len;
                    int off = 0;
                    if (!lenp) return -14;
                    len = *lenp;
                    /* struct ifconf: int len @0, 4-byte padding, char *buf @8 */
                    buf = *(char **)((uint8_t *)a3 + 8);
                    if (!buf) {
                        *lenp = 80; /* 2 * 40 bytes */
                        return 0;
                    }
                    /* eth0 entry: 40 bytes (sizeof struct ifreq) */
                    if (len >= off + 40) {
                        int i;
                        for (i = 0; i < 40; i++) buf[off + i] = 0;
                        for (i = 0; i < 4; i++) buf[off + i] = "eth0"[i];
                        buf[off + 16] = 2; buf[off + 17] = 0; /* AF_INET */
                        {
                            uint8_t *ip = (uint8_t *)&g_netif.ip;
                            for (i = 0; i < 4; i++) buf[off + 20 + i] = ip[i];
                        }
                        off += 40;
                    }
                    /* lo entry: 40 bytes */
                    if (len >= off + 40) {
                        int i;
                        for (i = 0; i < 40; i++) buf[off + i] = 0;
                        for (i = 0; i < 2; i++) buf[off + i] = "lo"[i];
                        buf[off + 16] = 2; buf[off + 17] = 0; /* AF_INET */
                        buf[off + 20] = 127; buf[off + 21] = 0;
                        buf[off + 22] = 0; buf[off + 23] = 1;
                        off += 40;
                    }
                    *lenp = off;
                    return 0;
                }
                case 0x8913: /* SIOCGIFFLAGS */
                case 0x8914: /* SIOCSIFFLAGS */
                case 0x8915: /* SIOCGIFADDR */
                case 0x8919: /* SIOCGIFBRDADDR */
                case 0x891B: /* SIOCGIFNETMASK */
                case 0x8921: /* SIOCGIFMTU */
                case 0x8927: { /* SIOCGIFHWADDR */
                    char *ifr = (char *)a3;
                    char name[16];
                    int i;
                    int is_lo = 0;
                    int is_eth = 0;
                    if (!ifr) return -14;
                    for (i = 0; i < 16; i++) name[i] = ifr[i];
                    if (name[0] == 'e' && name[1] == 't' && name[2] == 'h' &&
                        name[3] == '0' && name[4] == '\0')
                        is_eth = 1;
                    else if (name[0] == 'l' && name[1] == 'o' &&
                             name[2] == '\0')
                        is_lo = 1;
                    if (!is_eth && !is_lo) return -19; /* -ENODEV */
                    if (cmd == 0x8914) { /* SIOCSIFFLAGS: acknowledge interface state changes */
                        return 0;
                    }
                    if (cmd == 0x8913) { /* flags */
                        int *f = (int *)(ifr + 16);
                        /* UP|BROADCAST|RUNNING|MULTICAST, lo: UP|LOOPBACK|RUNNING */
                        *f = is_lo ? 0x49 : 0x1043;
                        return 0;
                    }
                    if (cmd == 0x8921) { /* mtu */
                        int *m = (int *)(ifr + 16);
                        *m = is_lo ? 65536 : 1500;
                        return 0;
                    }
                    /* sockaddr-style outputs: family + bytes */
                    for (i = 16; i < 32; i++) ifr[i] = 0;
                    if (cmd == 0x8927) { /* hwaddr */
                        ifr[16] = 1; ifr[17] = 0; /* ARPHRD_ETHER */
                        if (is_eth) {
                            for (i = 0; i < 6; i++)
                                ifr[18 + i] = (char)g_netif.mac[i];
                        }
                        return 0;
                    }
                    /* ADDR / NETMASK / BRDADDR: AF_INET + 4 address bytes */
                    ifr[16] = 2; ifr[17] = 0;
                    {
                        uint32_t v = 0;
                        if (cmd == 0x8915)
                            v = is_lo ? 0x0100007FUL : g_netif.ip;
                        else if (cmd == 0x8919)
                            v = is_lo ? 0 : ((g_netif.ip & g_netif.netmask) | ~g_netif.netmask);
                        else
                            v = is_lo ? 0x000000FFUL : g_netif.netmask;
                        {
                            uint8_t *b = (uint8_t *)&v;
                            for (i = 0; i < 4; i++) ifr[20 + i] = (char)b[i];
                        }
                    }
                    return 0;
                }
                default:
                    return -25; /* -ENOTTY */
            }
        }

        case SYS_WRITEV:
            return sys_writev((int)a1, (const struct iovec *)a2, (int)a3);

        case SYS_SENDFILE: {
            /* sendfile(out, in, &off, count): the fd->fd fast path busybox
             * uses for cat/cp/tar. The fds may be files, pipes, sockets or a
             * tty, so a plain read/write loop is both correct and enough;
             * partial transfers and -EFAULT/-EBADF follow POSIX. A NULL
             * offset reads from the input's current position and advances it;
             * a non-NULL one repositions around the transfer.
             *
             * The bounce buffer is static because several call sites (cat of
             * multi-megabyte tarballs) transfer in many iterations and the
             * kernel here is single-threaded: nothing can interleave on this
             * stack while we hold it. */
            int out_fd = (int)a1, in_fd = (int)a2;
            int64_t *offp = (int64_t *)a3;
            uint64_t count = a4;
            if (count == 0) return 0;
            static char sbuf[4096];
            uint64_t done = 0;
            int64_t saved = -1;
            if (offp) {
                int fdinfo_ok = vfs_lseek(in_fd, 0, SEEK_CUR);
                if (fdinfo_ok < 0) return (int64_t)fdinfo_ok;
                saved = (int64_t)fdinfo_ok;
                if (vfs_lseek(in_fd, *offp, SEEK_SET) < 0) return -29; /* -ESPIPE */
            }
            while (done < count) {
                uint64_t want = count - done;
                if (want > sizeof(sbuf)) want = sizeof(sbuf);
                int64_t got = vfs_read(in_fd, sbuf, (size_t)want);
                if (got < 0) goto sfail;
                if (got == 0) break;
                int64_t w = vfs_write(out_fd, sbuf, (size_t)got);
                if (w < 0) goto sfail;
                done += (uint64_t)w;
                if (w < got) break;
            }
            if (offp) {
                *offp += (int64_t)done;
                vfs_lseek(in_fd, saved, SEEK_SET);
            }
            return (int64_t)done;
        sfail:;
            if (offp) vfs_lseek(in_fd, saved, SEEK_SET);
            return done ? (int64_t)done : -5; /* partial progress or -EIO */
        }

        case SYS_ACCESS:
            return sys_access((const char *)a1, (int)a2);

        case SYS_DUP:
            return (int64_t)vfs_dup((int)a1);

        case SYS_DUP2:
            return (int64_t)vfs_dup2((int)a1, (int)a2);

        case SYS_GETPID:
            return current_process ? (int64_t)current_process->pid : 1;

        case SYS_GETTID:
            return current_process ? (int64_t)current_process->pid : 1;

        case SYS_SET_TID_ADDRESS:
            return current_process ? (int64_t)current_process->pid : 1;

        case SYS_UNAME:
            return sys_uname((struct utsname *)a1);

        case SYS_FLOCK:
            return (int64_t)vfs_flock((int)a1, (int)a2);

        case SYS_FCNTL: {            /* fcntl: musl's ash relies on F_DUPFD returning a REAL new fd. */
            int fd = (int)a1;
            int cmd = (int)a2;
            int arg = (int)a3;
            switch (cmd) {
                case 0:      /* F_DUPFD */
                    return vfs_fcntl_dupfd(fd, arg);
                case 1030:   /* F_DUPFD_CLOEXEC */
                    return vfs_fcntl_dupfd(fd, arg);
                case 1:      /* F_GETFD */
                case 2:      /* F_SETFD */
                    return 0;
                case 3:      /* F_GETFL */
                    return vfs_fcntl_getfl(fd);
                case 4:      /* F_SETFL */
                    return vfs_fcntl_setfl(fd, arg);
                case 5:      /* F_GETLK */
                case 6:      /* F_SETLK */
                case 7:      /* F_SETLKW */
                    return 0; /* No file locking */
                case 13:     /* F_GETSIG */
                    return 0;
                default:
                    return -22; /* -EINVAL */
            }
        }

        case SYS_GETCWD:
            return sys_getcwd((char *)a1, (size_t)a2);

        case SYS_CHDIR:
            return sys_chdir((const char *)a1);

        /* ── Credentials: minimal identity (boot as root, su/login can
         * transition; unprivileged callers can only shuffle between
         * their own ruid/euid — standard POSIX rules, no saved-IDs) ── */
        case SYS_GETUID:
            return current_process ? (int64_t)current_process->uid : 0;
        case SYS_GETGID:
            return current_process ? (int64_t)current_process->gid : 0;
        case SYS_GETEUID:
            return current_process ? (int64_t)current_process->euid : 0;
        case SYS_GETEGID:
            return current_process ? (int64_t)current_process->egid : 0;

        case SYS_SETUID: {
            if (!current_process) return -1;
            uint32_t id = (uint32_t)a1;
            if (current_process->euid == 0) {
                current_process->uid = id;
                current_process->euid = id;
                return 0;
            }
            if (id == current_process->uid || id == current_process->euid) {
                current_process->euid = id;
                return 0;
            }
            return -1; /* -EPERM */
        }

        case SYS_SETGID: {
            if (!current_process) return -1;
            uint32_t id = (uint32_t)a1;
            if (current_process->euid == 0) {
                current_process->gid = id;
                current_process->egid = id;
                return 0;
            }
            if (id == current_process->gid || id == current_process->egid) {
                current_process->egid = id;
                return 0;
            }
            return -1; /* -EPERM */
        }

        case SYS_SETREUID: {
            if (!current_process) return -1;
            int64_t r = (int64_t)a1, e = (int64_t)a2; /* -1 = no change */
            if (r != -1) {
                if (current_process->euid != 0 &&
                    (uint32_t)r != current_process->uid &&
                    (uint32_t)r != current_process->euid) return -1;
                current_process->uid = (uint32_t)r;
            }
            if (e != -1) {
                if (current_process->euid != 0 &&
                    (uint32_t)e != current_process->uid &&
                    (uint32_t)e != current_process->euid) return -1;
                current_process->euid = (uint32_t)e;
            }
            return 0;
        }

        case SYS_SETREGID: {
            if (!current_process) return -1;
            int64_t r = (int64_t)a1, e = (int64_t)a2;
            if (r != -1) {
                if (current_process->euid != 0 &&
                    (uint32_t)r != current_process->gid &&
                    (uint32_t)r != current_process->egid) return -1;
                current_process->gid = (uint32_t)r;
            }
            if (e != -1) {
                if (current_process->euid != 0 &&
                    (uint32_t)e != current_process->gid &&
                    (uint32_t)e != current_process->egid) return -1;
                current_process->egid = (uint32_t)e;
            }
            return 0;
        }

        case SYS_SETRESUID: {
            if (!current_process) return -1;
            int64_t r = (int64_t)a1, e = (int64_t)a2;
            if (r != -1) {
                if (current_process->euid != 0 &&
                    (uint32_t)r != current_process->uid &&
                    (uint32_t)r != current_process->euid) return -1;
                current_process->uid = (uint32_t)r;
            }
            if (e != -1) {
                if (current_process->euid != 0 &&
                    (uint32_t)e != current_process->uid &&
                    (uint32_t)e != current_process->euid) return -1;
                current_process->euid = (uint32_t)e;
            }
            return 0; /* no saved-ID slot yet: s ignored */
        }

        case SYS_GETRESUID: {
            if (!current_process) return -1;
            if (a1) *(uint32_t *)a1 = current_process->uid;
            if (a2) *(uint32_t *)a2 = current_process->euid;
            if (a3) *(uint32_t *)a3 = current_process->euid; /* suid == euid */
            return 0;
        }

        case SYS_SETRESGID: {
            if (!current_process) return -1;
            int64_t r = (int64_t)a1, e = (int64_t)a2;
            if (r != -1) {
                if (current_process->euid != 0 &&
                    (uint32_t)r != current_process->gid &&
                    (uint32_t)r != current_process->egid) return -1;
                current_process->gid = (uint32_t)r;
            }
            if (e != -1) {
                if (current_process->euid != 0 &&
                    (uint32_t)e != current_process->gid &&
                    (uint32_t)e != current_process->egid) return -1;
                current_process->egid = (uint32_t)e;
            }
            return 0;
        }

        case SYS_GETRESGID: {
            if (!current_process) return -1;
            if (a1) *(uint32_t *)a1 = current_process->gid;
            if (a2) *(uint32_t *)a2 = current_process->egid;
            if (a3) *(uint32_t *)a3 = current_process->egid;
            return 0;
        }

        case SYS_GETPPID:
            return current_process ? (int64_t)current_process->ppid : 0;

        /* ── Process lifecycle (deferred run-to-completion) ──────────────── */
        case SYS_FORK:
        case SYS_VFORK: {
            if (!current_process) return -1;
            struct process *c = process_fork(current_process,
                                             (const uint64_t *)current_syscall_frame);
            return c ? (int64_t)c->pid : -12; /* -ENOMEM */
        }

        case SYS_CLONE: {
            if (!current_process) return -1;
            /* Only fork-like clone (no shared VM/FD/signal tables) supported */
            if (a1 & (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD)) {
                return -38; /* -ENOSYS */
            }
            struct process *c = process_fork(current_process,
                                             (const uint64_t *)current_syscall_frame);
            return c ? (int64_t)c->pid : -12;
        }

        case SYS_CLONE3:
            return -38; /* -ENOSYS */

        case SYS_EXECVE:
            return sys_execve_user((const char *)a1, (const char **)a2, (const char **)a3);

        case SYS_WAIT4:
            return sys_do_wait4((int64_t)a1, (int *)a2, (int)a3, (void *)a4);

        case SYS_PIPE:
        case SYS_PIPE2:
            return (int64_t)vfs_pipe((int *)a1);

        case SYS_UMASK:
            return current_process ? (int64_t)current_process->umask : 022;

        /* ── *at() variants: musl uses these for all path syscalls ───────── */
        case SYS_OPENAT: {
            const char *path = (const char *)a2;
            int dirfd = (int)a1;
            /* Absolute paths ignore dirfd; AT_FDCWD resolves against cwd. */
            if (!path || path[0] == '/' || dirfd == AT_FDCWD) {
                return (int64_t)vfs_open(path, (int)a3, (int)a4);
            }
            return -9; /* -EBADF: dirfd-relative lookups unsupported */
        }

        case SYS_NEWFSTATAT: {
            const char *path = (const char *)a2;
            int dirfd = (int)a1;
            if (!path || path[0] == '/' || dirfd == AT_FDCWD) {
                if (a4 & AT_SYMLINK_NOFOLLOW) {
                    return (int64_t)vfs_lstat(path, (struct vfs_stat *)a3);
                }
                return (int64_t)vfs_stat(path, (struct vfs_stat *)a3);
            }
            return -9; /* -EBADF */
        }

        case SYS_GETDENTS64:
            return vfs_getdents64((int)a1, (void *)a2, (size_t)a3);

        case SYS_LSTAT:
            return (int64_t)vfs_lstat((const char *)a1, (struct vfs_stat *)a2);

        case SYS_READLINK:
            return vfs_readlink((const char *)a1, (char *)a2, (size_t)a3);

        case SYS_MKDIR:
            return (int64_t)vfs_mkdir((const char *)a1, (int)a2);

        case SYS_RENAME: {
            /* rename(oldpath, newpath): copy content, then unlink the source.
             * The VFS has no atomic rename yet, so this is what POSIX
             * programs get; for regular files on tarfs/tmpfs the result is
             * indistinguishable. */
            const char *oldpath = (const char *)a1;
            const char *newpath = (const char *)a2;
            if (!oldpath || !newpath) return -14; /* -EFAULT */
            struct vfs_node *old_node = vfs_lookup(oldpath);
            if (!old_node) return -2; /* -ENOENT */
            if (old_node->type == 4 /* VFS_DIRECTORY */) return -22; /* -EINVAL: dirs unsupported */

            int rfd = vfs_open(oldpath, O_RDONLY, 0);
            if (rfd < 0) return rfd;
            int wfd = vfs_open(newpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (wfd < 0) { vfs_close(rfd); return wfd; }

            static char rbuf[4096];
            int64_t total = 0;
            for (;;) {
                int64_t got = vfs_read(rfd, rbuf, sizeof(rbuf));
                if (got <= 0) break;
                int64_t wrote = vfs_write(wfd, rbuf, (size_t)got);
                if (wrote < 0) { total = wrote; break; }
                total += wrote;
            }
            vfs_close(rfd);
            vfs_close(wfd);
            if (total < 0) { vfs_unlink(newpath); return total; }
            /* Content copied; drop the source so the rename is complete. */
            return (int64_t)vfs_unlink(oldpath);
        }

        case SYS_UNLINK:
            return (int64_t)vfs_unlink((const char *)a1);

        case SYS_TRUNCATE:
            return (int64_t)vfs_truncate((const char *)a1, (int64_t)a2);

        case SYS_FTRUNCATE:
            return (int64_t)vfs_ftruncate((int)a1, (int64_t)a2);

        case SYS_RMDIR:
            return (int64_t)vfs_rmdir((const char *)a1);

        case SYS_FSYNC:
        case SYS_FDATASYNC:
        case SYS_SYNC:
            return 0; /* No write-back caching: nothing to flush */

        case SYS_GETRANDOM:
            rng_fill((void *)a1, (size_t)a2);
            return (int64_t)a2;

        case SYS_RT_SIGSUSPEND: {
            /* rt_sigsuspend(mask, sigsetsize): swap in the mask, then wait
             * until a signal becomes deliverable and report EINTR, which is
             * exactly how Linux returns after running the handler. */
            if (!current_process) return -1;
            uint64_t saved = current_process->sig_blocked;
            if (a1) current_process->sig_blocked = *(const uint64_t *)a1;
            for (;;) {
                if (current_process->sig_pending & ~current_process->sig_blocked)
                    break;
                __asm__ volatile("pause");
            }
            current_process->sig_blocked = saved;
            return -4; /* -EINTR */
        }

        case SYS_RT_SIGPENDING: {
            if (!current_process) return -1;
            if (a1) *(uint64_t *)a1 =
                current_process->sig_pending & ~current_process->sig_blocked;
            return 0;
        }

        case SYS_RT_SIGRETURN:
            /* Handler finished (libc's restorer issued rt_sigreturn): put the
             * interrupted ring-3 frame back and deliver its result. */
            return sig_restore_frame();

        case SYS_NANOSLEEP: {
            struct ktimespec *req = (struct ktimespec *)a1;
            if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) return -22; /* -EINVAL */
            timer_sleep_ms((uint32_t)(req->tv_sec * 1000 + req->tv_nsec / 1000000));
            if (a2) { /* rem: slept the full interval, nothing remaining */
                struct ktimespec *rem = (struct ktimespec *)a2;
                rem->tv_sec = 0;
                rem->tv_nsec = 0;
            }
            return 0;
        }

        case SYS_CLOCK_NANOSLEEP: {
            struct ktimespec *req = (struct ktimespec *)a4;
            if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) return -22;
            timer_sleep_ms((uint32_t)(req->tv_sec * 1000 + req->tv_nsec / 1000000));
            if (a5) {
                struct ktimespec *rem = (struct ktimespec *)a5;
                rem->tv_sec = 0;
                rem->tv_nsec = 0;
            }
            return 0;
        }

        case SYS_SETITIMER:
        case SYS_GETITIMER: {
            /* struct itimerval { timeval interval, value; },
             * timeval { long sec, usec }. ITIMER_REAL (which 0) only:
             * VIRTUAL/PROF need CPU-time accounting we do not have. */
            struct ktimeval { long tv_sec; long tv_usec; };
            struct kitimerval { struct ktimeval interval, value; };
            int which = (int)a1;
            uint32_t freq = timer_get_freq();
            const struct kitimerval *n;
            struct kitimerval *o;
            if (which != 0) return -22; /* -EINVAL: only ITIMER_REAL */
            if (!current_process) return -1;
            if (!freq) freq = 100;
            if (num == (uint64_t)SYS_GETITIMER) {
                uint64_t now;
                uint64_t rem = 0;
                o = (struct kitimerval *)a2;
                if (!o) return -14;
                now = timer_get_ticks();
                if (current_process->itimer_expire > now)
                    rem = current_process->itimer_expire - now;
                o->interval.tv_sec = (long)(current_process->itimer_interval / freq);
                o->interval.tv_usec = (long)((current_process->itimer_interval % freq) * 1000000ULL / freq);
                o->value.tv_sec = (long)(rem / freq);
                o->value.tv_usec = (long)((rem % freq) * 1000000ULL / freq);
                return 0;
            }
            n = (const struct kitimerval *)a2;
            o = (struct kitimerval *)a3;
            if (o) {
                /* report the previous setting first */
                uint64_t now = timer_get_ticks();
                uint64_t rem = 0;
                if (current_process->itimer_expire > now)
                    rem = current_process->itimer_expire - now;
                o->interval.tv_sec = (long)(current_process->itimer_interval / freq);
                o->interval.tv_usec = (long)((current_process->itimer_interval % freq) * 1000000ULL / freq);
                o->value.tv_sec = (long)(rem / freq);
                o->value.tv_usec = (long)((rem % freq) * 1000000ULL / freq);
            }
            if (n) {
                uint64_t val;
                uint64_t iv;
                if (n->value.tv_sec < 0 || n->value.tv_usec < 0 ||
                    n->value.tv_usec >= 1000000 ||
                    n->interval.tv_sec < 0 || n->interval.tv_usec < 0 ||
                    n->interval.tv_usec >= 1000000)
                    return -22;
                val = (uint64_t)n->value.tv_sec * freq +
                      (uint64_t)n->value.tv_usec * freq / 1000000ULL;
                iv = (uint64_t)n->interval.tv_sec * freq +
                     (uint64_t)n->interval.tv_usec * freq / 1000000ULL;
                if (val == 0) {
                    /* zero value disarms (and stops the interval) */
                    current_process->itimer_expire = 0;
                    current_process->itimer_interval = 0;
                } else {
                    current_process->itimer_interval = iv;
                    current_process->itimer_expire = timer_get_ticks() + val;
                }
            }
            return 0;
        }

        case SYS_SCHED_YIELD:
            return 0; /* No preemption yet: nothing to yield to, success */

        /* sched_getaffinity(pid, len, mask): this kernel runs on one CPU,
         * so the honest answer is a 1-bit mask. musl's pthread_create and
         * some library inits call this unconditionally and abort if it
         * returns -ENOSYS; returning success with one bit lets them
         * proceed without pretending about hardware we do not have. */
        case SYS_SCHED_GETAFFINITY: {
            uint32_t *mask = (uint32_t *)a3;
            size_t len = (size_t)a2;
            if (!mask || len < sizeof(uint32_t)) return -22; /* -EINVAL */
            mask[0] = 1; /* CPU 0 */
            for (size_t i = 4; i < len && i < 128; i += 4)
                mask[i / 4] = 0;
            return sizeof(uint32_t);
        }

        case SYS_SCHED_SETAFFINITY:
            /* Accepting and discarding is what a 1-CPU kernel can do; a
             * caller that pins to CPU 1 would otherwise get -EINVAL and
             * abort for no functional reason. */
            return 0;

        case SYS_POLL:
        case SYS_PPOLL: {
            /* struct pollfd { int fd; short events; short revents; } = 8 bytes */
            struct pollfd_k { int fd; short events; short revents; };
            struct pollfd_k *fds = (struct pollfd_k *)a1;
            int nfds = (int)a2;
            if (nfds < 0) return -22;
            /* Timeout decoding (Linux x86_64 ABI):
             *   poll(fds, nfds, timeout_ms)   -> timeout in a3 (ms, -1 wait)
             *   ppoll(fds, nfds, ts*, mask*)  -> NULL ts in a3 waits forever */
            int64_t timeout_ms = -1;
            if (num == (uint64_t)SYS_PPOLL) {
                const struct ktimespec *ts = (const struct ktimespec *)a3;
                if (ts) {
                    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L)
                        return -22;
                    timeout_ms = ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
                }
            } else {
                timeout_ms = (int)a3;
            }
            if (timeout_ms < -1) timeout_ms = -1;
            int ready = 0;
            /* Wait loop: re-check until something is ready, the deadline
             * passes, or a signal needs delivering. Sleeps beat spinning. */
            uint64_t waited = 0;
            for (;;) {
                net_poll();
                ready = 0;
                for (int i = 0; i < nfds; i++) {
                    fds[i].revents = 0;
                    if (fds[i].fd >= 0) {
                        if (is_socket_fd(fds[i].fd)) {
                            short r = (short)sys_sock_poll(fds[i].fd, fds[i].events);
                            if (r) { fds[i].revents = r; ready++; }
                        } else {
                            /* Honest readiness from the fd layer: console
                             * stdin is readable only while a keystroke is
                             * already queued (safe_read_key/readit rely on
                             * this to either drain or block). */
                            short r = (short)vfs_poll(fds[i].fd, fds[i].events);
                            if (r) { fds[i].revents = r; ready++; }
                        }
                    } else {
                        fds[i].revents = 0x0008; /* POLLNVAL */
                    }
                }
                if (ready) break;
                if (current_process && has_unblocked_pending()) break;
                if (timeout_ms == 0) break;
                if (timeout_ms > 0) {
                    if ((int64_t)waited >= timeout_ms) break;
                    timer_sleep_ms(1);
                    waited++;
                } else {
                    hlt_until_irq_or_read();
                    if (current_process && has_unblocked_pending()) break;
                }
            }
            return ready;
        }

        case SYS_SELECT: {
            /* select(nfds, readfds, writefds, exceptfds, timeval):
             * musl: a1=nfds a2=r a3=w a4=e a5=timeout(timeval* or NULL) */
            typedef struct { uint64_t bits[16]; } kfd_set;
            kfd_set *rset = (kfd_set *)a2;
            kfd_set *wset = (kfd_set *)a3;
            kfd_set *eset = (kfd_set *)a4;
            const struct ktimeval { long tv_sec; long tv_usec; } *tv =
                (const struct ktimeval *)a5;
            if ((int64_t)a1 < 0) return -22;
            int nfds = (int)a1;
            if (nfds > 1024) nfds = 1024;
            int64_t timeout_ms = -1;
            if (tv) {
                if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000)
                    return -22;
                timeout_ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;
            }
            uint64_t waited = 0;
            for (;;) {
                net_poll();
                int count = 0;
                for (int i = 0; i < nfds && i < 1024; i++) {
                    int bit = (i >> 6) & 15, m = 1 << (i & 63);
                    int inr = rset && (rset->bits[bit] & (uint64_t)m);
                    int inw = wset && (wset->bits[bit] & (uint64_t)m);
                    int want = (inr ? 0x001 : 0) | (inw ? 0x004 : 0);
                    int r = 0;
                    if (is_socket_fd(i)) r = sys_sock_poll(i, want);
                    else r = want ? vfs_poll(i, want) : 0;
                    if (inr && !(r & 0x001)) rset->bits[bit] &= ~(uint64_t)m;
                    else if (inr) count++;
                    if (inw && !(r & 0x004)) wset->bits[bit] &= ~(uint64_t)m;
                    else if (inw) count++;
                    if (eset && eset->bits[bit] & (uint64_t)m)
                        eset->bits[bit] &= ~(uint64_t)m;
                }
                if (count) return count;
                if (current_process && has_unblocked_pending()) return -4;
                if (timeout_ms == 0) return 0;
                if (timeout_ms > 0) {
                    if (waited >= (uint64_t)timeout_ms) return 0;
                    timer_sleep_ms(1);
                    waited++;
                } else {
                    hlt_until_irq_or_read();
                    if (current_process && has_unblocked_pending()) return -4;
                }
            }
        }

        case SYS_PSELECT6: {
            typedef struct { uint64_t bits[16]; } kfd_set; /* 1024 fds */
            kfd_set *rset = (kfd_set *)a2;
            kfd_set *wset = (kfd_set *)a3;
            kfd_set *eset = (kfd_set *)a4;
            int nfds = (int)a1;
            if (nfds < 0) return -22; /* -EINVAL */
            if (nfds > 1024) nfds = 1024;
            for (;;) {
                net_poll();
                int count = 0;
                for (int i = 0; i < nfds && i < 1024; i++) {
                    int bit = (i >> 6) & 15, m = 1 << (i & 63);
                    int inr = rset && (rset->bits[bit] & (uint64_t)m);
                    int inw = wset && (wset->bits[bit] & (uint64_t)m);
                    int want = (inr ? 0x001 : 0) | (inw ? 0x004 : 0);
                    int r = 0;
                    if (is_socket_fd(i)) r = sys_sock_poll(i, want);
                    else r = want ? vfs_poll(i, want) : 0;
                    if (inr && !(r & 0x001)) rset->bits[bit] &= ~(uint64_t)m;
                    else if (inr) count++;
                    if (inw && !(r & 0x004)) wset->bits[bit] &= ~(uint64_t)m;
                    else if (inw) count++;
                    if (eset && eset->bits[bit] & (uint64_t)m)
                        eset->bits[bit] &= ~(uint64_t)m;
                }
                if (count) return count;
                if (current_process && has_unblocked_pending()) return -4;
                /* pselect with a NULL timeout blocks indefinitely */
                hlt_until_irq_or_read();
                if (current_process && has_unblocked_pending()) return -4;
            }
        }

        case SYS_ARCH_PRCTL: {
            int code = (int)a1;
            uint64_t addr = a2;
            if (code == ARCH_SET_FS) {
                wrmsr(MSR_FS_BASE, addr);
                /* Mirror into the PCB: fork() copies this to children and
                 * execve() clears it, so the TLS base is tracked correctly
                 * even though the MSR is what the CPU actually uses. */
                if (current_process) current_process->fs_base = addr;
                return 0;
            } else if (code == ARCH_GET_FS) {
                if (addr) *(uint64_t *)addr = rdmsr(MSR_FS_BASE);
                return 0;
            } else if (code == ARCH_SET_GS) {
                wrmsr(MSR_GS_BASE, addr);
                if (current_process) current_process->gs_base = addr;
                return 0;
            } else if (code == ARCH_GET_GS) {
                if (addr) *(uint64_t *)addr = rdmsr(MSR_GS_BASE);
                return 0;
            }
            return -22; /* -EINVAL */
        }

        case SYS_READV:
            return sys_readv((int)a1, (const struct iovec *)a2, (int)a3);

        case SYS_CLOCK_GETTIME:
            return sys_clock_gettime((int)a1, (struct ktimespec *)a2);

        case SYS_EPOLL_CREATE1:
            return sys_epoll_create1((int)a1);

        case SYS_EVENTFD:
        case SYS_EVENTFD2:
            /* eventfd(initval, flags): flags are CLOEXEC/NONBLOCK, which the
             * fd table does not model; the value is what matters. */
            return (int64_t)chan_eventfd((unsigned int)a1);

        case SYS_TIMERFD_CREATE:
            return (int64_t)chan_timerfd_create((int)a1);

        case SYS_TIMERFD_SETTIME: {
            /* timerfd_settime(fd, flags, new, old): the timespec pair is
             * sec+nsec for both the initial and interval timers. */
            struct chanfd_ts { int64_t sec; int64_t nsec; };
            const struct chanfd_ts *nt = (const struct chanfd_ts *)a3;
            if (!nt) return -14; /* -EFAULT */
            if (nt->sec < 0 || nt->nsec < 0 || nt->nsec >= 1000000000L)
                return -22;
            uint64_t isec = 0, insec = 0;
            if (a4) {
                const struct chanfd_ts *it = (const struct chanfd_ts *)a4;
                if (it->sec < 0 || it->nsec < 0 || it->nsec >= 1000000000L)
                    return -22;
                isec = (uint64_t)it->sec;
                insec = (uint64_t)it->nsec;
            }
            struct file_descriptor *ft = current_process ? current_process->fds
                                                         : NULL;
            if (!ft || (int)a1 < 0 || (int)a1 >= MAX_FD_COUNT ||
                !ft[(int)a1].is_used || !ft[(int)a1].node)
                return -9; /* -EBADF */
            return (int64_t)chan_timer_settime(ft[(int)a1].node, (int)a2,
                                               (uint64_t)nt->sec,
                                               (uint64_t)nt->nsec,
                                               isec, insec);
        }

        case SYS_SIGNALFD:
        case SYS_SIGNALFD4:
            /* signalfd(fd, mask, sizemask): mask is a 64-bit sigset; the low
             * 32 bits cover the signals this kernel can raise. */
            return (int64_t)chan_signalfd((int)(a2 ? *(const uint32_t *)a2 : 0));

        case SYS_ACCEPT4:
            /* accept4(sockfd, addr, addrlen, flags): the flags are
             * CLOEXEC/NONBLOCK, so plain accept semantics apply. */
            return sys_sock_accept((int)a1, (struct sockaddr_un *)a2,
                                   (uint32_t *)a3);

        case SYS_EPOLL_CTL:
            return sys_epoll_ctl((int)a1, (int)a2, (int)a3,
                                 (const struct epoll_event_k *)a4);

        case SYS_EPOLL_WAIT:
            return sys_epoll_wait((int)a1, (struct epoll_event_k *)a2,
                                  (int)a3, (int)a4);

        /* musl's epoll_wait goes through epoll_pwait on x86_64 and only falls
         * back to 232 on -ENOSYS. The signal mask and size args are accepted
         * and ignored: with no preemption there is no race for the mask to
         * close, and the size is always _NSIG/8. */
        case SYS_EPOLL_PWAIT:
            return sys_epoll_wait((int)a1, (struct epoll_event_k *)a2,
                                  (int)a3, (int)a4);

        case SYS_MEMFD_CREATE: {
            /* memfd_create(name, flags): an anonymous file that can be
             * mmap'd shared. Wayland's wl_shm buffer pool is exactly this -
             * the client and the compositor must land on the same frames,
             * which heap-backed tmpfs files cannot provide. */
            const char *name = (const char *)a1;
            (void)a2; /* MFD_CLOEXEC/MFD_ALLOW_SEALING: no exec-time fd table */
            return (int64_t)shmem_create(name, 0);
        }

        case SYS_FUTEX:
            return sys_futex((uint32_t *)a1, (int)a2, (uint32_t)a3,
                             (const struct ktimespec *)a4,
                             (uint32_t *)a5, (uint32_t)a6);

        case SYS_CLOCK_SETTIME: {
            /* clock_settime(clk, ts): only CLOCK_REALTIME is settable. The
             * adjustment rewrites the frozen boot base, so the running
             * session keeps the new time without touching CMOS. */
            int clk = (int)a1;
            const struct ktimespec *ts = (const struct ktimespec *)a2;
            if (!ts) return -14; /* -EFAULT */
            if (clk != CLOCK_REALTIME) return -22; /* -EINVAL */
            if (ts->tv_sec < 0 || ts->tv_nsec < 0 ||
                ts->tv_nsec >= 1000000000L)
                return -22;
            time_base = ts->tv_sec - (int64_t)sys_uptime_sec();
            return 0;
        }

        case SYS_TIME:
            return wall_clock_sec();

        case SYS_CLOCK_GETRES: {
            /* clock_getres(clk, ts): the PIT ticks at 100 Hz, so the finest
             * quantum any clock here can report is 10 ms. libwayland asks
             * once at startup to size its timer wheel; lying with a finer
             * number would only make it busy-wait. */
            int clk = (int)a1;
            struct ktimespec *ts = (struct ktimespec *)a2;
            if (clk < 0 || clk > 15) return -22; /* -EINVAL */
            if (ts) {
                ts->tv_sec = 0;
                ts->tv_nsec = 10000000L; /* 10 ms */
            }
            return 0;
        }

        /* prctl(2): implement the slice the userspace platform actually
         * uses, and say -EINVAL for the rest. Returning success for unknown
         * subcommands would make callers believe a policy is in force that
         * is not (dumpable, no-new-privs and friends are security promises);
         * -EINVAL tells them honestly that nothing happened. */
        case SYS_PRCTL: {
            int op = (int)a1;
            if (op == 15) { /* PR_SET_NAME: process label for logs/tools */
                const char *s = (const char *)a2;
                if (!s) return -14; /* -EFAULT */
                if (current_process) {
                    int i = 0;
                    for (; i < 15 && s[i]; i++)
                        current_process->name[i] = s[i];
                    current_process->name[i] = '\0';
                }
                return 0;
            }
            if (op == 16) { /* PR_GET_NAME: fill the caller's buffer */
                char *s = (char *)a2;
                if (!s) return -14; /* -EFAULT */
                const char *n = current_process ? current_process->name : "";
                int i = 0;
                for (; i < 15 && n[i]; i++) s[i] = n[i];
                s[i] = '\0';
                return 0;
            }
            return -22; /* -EINVAL */
        }

        case SYS_SYSINFO:
            return sys_sysinfo((struct ksysinfo *)a1);

        case SYS_GETRUSAGE:
            return sys_getrusage((int)a1, (void *)a2);

        case SYS_GETRLIMIT:
        case SYS_SETRLIMIT: {
            /* struct rlimit { rlim_cur, rlim_max } = 2 x uint64 */
            uint64_t *rl = (uint64_t *)a2;
            if (!rl) return -14; /* -EFAULT */
            rl[0] = 0xFFFFFFFFFFFFFFFFULL; /* RLIM_INFINITY */
            rl[1] = 0xFFFFFFFFFFFFFFFFULL;
            return 0;
        }

        case SYS_PRLIMIT64: {
            /* prlimit64(pid, resource, new, old) */
            uint64_t *old = (uint64_t *)a4;
            if (old) {
                old[0] = 0xFFFFFFFFFFFFFFFFULL;
                old[1] = 0xFFFFFFFFFFFFFFFFULL;
            }
            return 0;
        }

        case SYS_KILL:
            return process_kill((uint32_t)a1, (int)a2);
        case SYS_TKILL:
            /* tkill(tid, sig) */
            return process_kill((uint32_t)a1, (int)a2);

        case SYS_TGKILL:
            /* tgkill(tgid, tid, sig) - the tid, not the tgid, is signalled. */
            return process_kill((uint32_t)a2, (int)a3);

        case SYS_REBOOT: {
            /* reboot(magic1, magic2, cmd, arg): only privileged callers.
             * Mirrors the kernel shell's reboot/halt paths. */
            extern int acpi_poweroff(void);
            if (a1 != 0xFEE1DEADULL || a2 != 0x28121969ULL) return -22;
            if (!current_process || current_process->euid != 0) return -1;
            if (a3 == 0x01234567UL || a3 == 0x89ABCDEFUL) { /* RESTART / CAD */
                serial_puts("[SYS] reboot(RESTART)\n");
                /* Drain the i8042 input buffer first: a pulse sent while
                 * IBF is set is silently dropped (same sequence as the
                 * kernel shell's reboot command). */
                for (int i = 0; i < 100000; i++) {
                    uint8_t st;
                    __asm__ volatile("inb $0x64, %0" : "=a"(st));
                    if (!(st & 0x02)) break;
                }
                __asm__ volatile("outb %0, $0x64" :: "a"((uint8_t)0xFE));
                timer_sleep_ms(500);
                __asm__ volatile("outb %0, $0x64" :: "a"((uint8_t)0xFE));
                for (;;) __asm__ volatile("hlt");
            }
            if (a3 == 0xCDEF0123UL || a3 == 0x4321FEDCUL) { /* HALT / POWER_OFF */
                serial_puts("[SYS] reboot(POWER_OFF)\n");
                if (acpi_poweroff() != 0) __asm__ volatile("cli");
                for (;;) __asm__ volatile("hlt");
            }
            return -22; /* unknown cmd */
        }

        /* ── Process groups / sessions (job-control probes) ──────────────── */
        case SYS_SETSID:
            if (!current_process) return -1;
            current_process->sid = current_process->pid;
            current_process->pgid = current_process->pid;
            return (int64_t)current_process->pid;

        case SYS_SETPGID:
            if (!current_process) return -1;
            if (a2) {
                current_process->pgid = (uint32_t)a2;
            } else if (a1) {
                current_process->pgid = (uint32_t)a1;
            } else {
                current_process->pgid = current_process->pid;
            }
            return 0;

        case SYS_GETPGID:
        case SYS_GETPGRP: {
            if (a1 && a1 != 0xFFFFFFFFFFFFFFFF) {
                struct process *p = process_find((uint32_t)a1);
                return p ? (int64_t)p->pgid : -3; /* -ESRCH */
            }
            return current_process ? (int64_t)current_process->pgid : 0;
        }

        /* ── BSD socket layer ────────────────────────────────────────────── */
        case SYS_SOCKET:
            return sys_sock_socket((int)a1, (int)a2, (int)a3);

        case SYS_CONNECT:
            return sys_sock_connect((int)a1, (const struct sockaddr_in *)a2, (uint32_t)a3);

        case SYS_BIND:
            return sys_sock_bind((int)a1, (const struct sockaddr_un *)a2, (uint32_t)a3);

        case SYS_LISTEN:
            return sys_sock_listen((int)a1, (int)a2);

        case SYS_ACCEPT:
            return sys_sock_accept((int)a1, (struct sockaddr_un *)a2, (uint32_t *)a3);

        case SYS_SENDTO:
            return sys_sock_sendto((int)a1, (const void *)a2, (size_t)a3, (int)a4,
                                   (const struct sockaddr_in *)a5, (uint32_t)a6);

        case SYS_RECVFROM:
            return sys_sock_recvfrom((int)a1, (void *)a2, (size_t)a3, (int)a4,
                                     (struct sockaddr_in *)a5, (uint32_t *)a6);

        case SYS_SENDMSG:
            return sys_sendmsg((int)a1, (const struct kmsghdr *)a2, (int)a3);

        case SYS_RECVMSG:
            return sys_recvmsg((int)a1, (struct kmsghdr *)a2, (int)a3);

        case SYS_SHUTDOWN:
            return sys_sock_shutdown((int)a1, (int)a2);

        case SYS_GETSOCKNAME:
            return sys_sock_getsockname((int)a1, (struct sockaddr_un *)a2, (uint32_t *)a3);

        case SYS_GETPEERNAME:
            return sys_sock_getpeername((int)a1, (struct sockaddr_un *)a2, (uint32_t *)a3);

        case SYS_SOCKETPAIR:
            return sys_sock_socketpair((int)a1, (int)a2, (int)a3, (int *)a4);

        case SYS_SETSOCKOPT:
            return 0; /* Accept and ignore option requests */

        case SYS_GETSOCKOPT: {
            /* SO_TYPE and SO_ERROR are the two options a program cannot fake
             * (curl branches on SO_ERROR when a connect says it failed);
             * everything else is reported as zero. optlen is in/out. */
            int fd = (int)a1, opt = (int)a3;
            void *val = (void *)a4;
            uint32_t *lenp = (uint32_t *)a5;
            if (!lenp) return -14; /* -EFAULT */
            if (*lenp < sizeof(int)) return -22; /* -EINVAL */
            int out = 0;
            if (opt == 3) { /* SO_TYPE */
                out = sock_get_type(fd);
                if (out < 0) out = 0;
            } else if (opt == 4) { /* SO_ERROR */
                out = 0;
            }
            if (val) *(int *)val = out;
            *lenp = sizeof(int);
            return 0;
        }

        case SYS_EXIT:
        case SYS_EXIT_GROUP:
            serial_puts("[PROC] Process exited with status: ");
            serial_puthex(a1);
            serial_puts("\n");
            process_exit((int)a1);
            return 0;

        default:
            serial_puts("[SYSCALL] Warning: Unimplemented syscall #");
            serial_puthex(num);
            serial_puts("\n");
            return -38; /* -ENOSYS */
    }
}

/* Wrapper: logs syscall entry, key args, and return value for debugging.
 * Set to 1 to trace syscalls on serial (very noisy; use only when debugging). */
#define SYSCALL_TRACE 0
/* TEMP-DEBUG: trace input-related syscalls from whiptail only. */
#define WT_TRACE 0

int64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
#if WT_TRACE
    if (current_process && (num == 0 || num == 23)) {
        const char *nm = current_process->name;
        for (int i = 0; nm[i] && nm[i+1] && nm[i+2]; i++) {
            if (nm[i] == 'w' && nm[i+1] == 'h' && nm[i+2] == 'i') {
                serial_puts(num == 0 ? "[WT-ENTER read]\n" : "[WT-ENTER select]\n");
                break;
            }
        }
    }
#endif
    int64_t ret = syscall_dispatch_impl(num, a1, a2, a3, a4, a5, a6);
#if WT_TRACE
    if (current_process && (1)) {
        const char *nm = current_process->name;
        int is_wt = 0;
        for (int i = 0; nm[i] && nm[i+1] && nm[i+2]; i++) {
            if (nm[i] == 'w' && nm[i+1] == 'h' && nm[i+2] == 'i') { is_wt = 1; break; }
        }
        if (is_wt) {
        serial_puts("[WT sc=");
        serial_puthex(num);
        serial_puts(" a1=");
        serial_puthex(a1);
        serial_puts(" a2=");
        serial_puthex(a2);
        serial_puts(" a3=");
        serial_puthex(a3);
        serial_puts("] -> ");
        serial_puthex((uint64_t)ret);
        serial_puts("\n");
        }
    }
#endif
#if SYSCALL_TRACE
    serial_puts("[SC ");
    serial_puthex(num);
    serial_puts(" a1=");
    serial_puthex(a1);
    serial_puts(" a3=");
    serial_puthex(a3);
    serial_puts("] -> ");
    serial_puthex((uint64_t)ret);
    serial_puts("\n");
#endif
    /* Last thing before the frame is popped back to ring 3: act on pending
     * signals. Doing it here means a handler is entered with the interrupted
     * syscall frame intact (rt_sigreturn can resume it) and never from IRQ
     * context, where a stack switch would be fatal. */
    deliver_pending_signals(ret);
    return ret;
}
