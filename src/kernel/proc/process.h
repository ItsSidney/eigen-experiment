/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * process.h - Process Control Block and Ring-3 execution management.
 *
 * PATCH NOTES (2026-09):
 *   - process_kill(pid, sig), process_find(), process_iter(),
 *     process_raise(), deliver_pending_signals() added.
 *   - process struct extended with signal state and fd table.
 *   - Kernel build uses -ffreestanding -fno-stack-protector; no libc needed.
 */

#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include "mm/vmm.h"
#include "fs/vfs/vfs.h"

#define USER_STACK_TOP  0x00007FFFFFFFE000ULL
#define USER_STACK_SIZE (64 * 1024)

/*
 * User address space layout (canonical lower half, 47-bit):
 *
 *   0x0000000000400000  ELF PT_LOAD segments (text, data, bss)
 *   [brk_base .. brk]   heap (grows up via sys_brk)
 *   0x0000700000000000  mmap region base (grows down)
 *   0x00007FFFFFFFE000  user stack (64 KB)
 */
#define USER_MMAP_BASE  0x0000700000000000ULL   /* mmap hint base   */
#ifndef USER_MMAP_END
#define USER_MMAP_END   0x00007F0000000000ULL   /* mmap upper limit */
#endif

struct process {
    uint32_t  pid;
    uint32_t  ppid;
    uint32_t  tid;
    uint32_t  tgid;
    int       is_thread;
    uint64_t  clear_child_tid;
    uint64_t  set_tid_addr;
    pml4_t   *pml4;
    uint64_t  entry_point;
    uint64_t  user_stack_top;
    char      name[64];

    uint64_t  brk_base;
    uint64_t  brk;
    uint64_t  mmap_next;

    char      cwd[256];
    uint32_t  umask;

    /* Credentials (minimal identity; everything boots as root).
     * No saved-IDs yet — su/login only need uid/euid transitions. */
    uint32_t  uid;
    uint32_t  euid;
    uint32_t  gid;
    uint32_t  egid;

    uint32_t  sid;
    uint32_t  pgid;      /* process group (job control) */
    int       has_ctty;

    /* Signal state (caught/blocked/pending per signo, 1..64) */
    uint64_t  sig_pending;
    uint64_t  sig_blocked;
    /* ITIMER_REAL state in PIT ticks: 0 expiry = disarmed. Interval 0 =
     * one-shot. Not inherited across fork (child starts disarmed). */
    uint64_t  itimer_expire;
    uint64_t  itimer_interval;
    uint64_t  sig_handlers[65];
    uint64_t  sig_flags[65];
    uint64_t  sig_restorer[65];
    int       in_sighandler;
    uint64_t  sig_saved_rip;
    uint64_t  sig_saved_rsp;
    uint64_t  sig_saved_rflags;
    uint64_t  sig_saved_rax;
    uint64_t  sig_saved_regs[12];
    /* Full 15-quadword syscall frame stashed when a handler is entered, plus
     * the syscall result that was about to be returned. Copied back by
     * rt_sigreturn(). Index 15 holds the interrupted return value. */
    uint64_t  sig_saved_frame[16];
    uint64_t  sig_saved_blocked;
    /* User RSP where the restorer return-address was planted during signal
     * delivery. Cleared by rt_sigreturn so a later `ret` at that stack
     * depth can never re-enter the restorer trampoline. */
    uint64_t  sig_frame_nrsp;

    int       stopped;
    int       stop_sig;
    int       stop_reported;
    void     *stop_frame;

    struct file_descriptor fds[MAX_FD_COUNT];

    int             exited;
    int             exit_status;
    int             started;        /* forked but not yet scheduled (deferred run) */
    int             nested;         /* runs nested inside parent's wait4 */
    uint64_t        fork_frame[15]; /* saved syscall frame for deferred child start */
    uint64_t        saved_kernel_ctx[8];
    struct process *parent;
    struct process *next;

    uint64_t        fs_base;
    uint64_t        gs_base;
    uint32_t        last_child_pid;
    void           *frame;
    uint8_t         kstack[16384] __attribute__((aligned(16)));
    uint64_t        kstack_top;
};

/* Global current process and kernel stack */
extern struct process *current_process;
extern uint64_t current_kstack_top;
extern struct process *process_list;

void            process_init(void);
struct process *process_create_from_elf(const char *path, const char *name, int argc, const char **argv);
int             process_run(struct process *proc);
void            process_exit(int status);

/* fork/exec/wait support (run-to-completion deferred model) */
struct process *process_fork(struct process *parent, const uint64_t *syscall_frame);
int             process_run_deferred(struct process *child);
int             process_execve(struct process *p, const char *path,
                               int argc, const char **argv, const char **envp);

int64_t         sys_fork(void);
int64_t         sys_clone(uint64_t flags, uint64_t stack, int *ptid, int *ctid, uint64_t tls);
int64_t         sys_clone3(void *uargs, size_t size);
int64_t         sys_wait4(int64_t pid, int *status, int options, void *rusage);
int             process_kill(uint32_t pid, int sig);
void            process_reap(struct process *p);
struct process *process_find(uint32_t pid);
struct process *process_iter(struct process *prev);
void            process_raise(int sig);
void            sig_check_current(void);
/* PIT-tick hook: fire expired ITIMER_REAL timers (SIGALRM). Runs in IRQ
 * context; only sets pending bits, delivery happens at syscall return. */
void            proc_timer_tick(void);
void            deliver_pending_signals(int64_t ret);
int64_t         sig_restore_frame(void);

#endif /* KERNEL_PROCESS_H */