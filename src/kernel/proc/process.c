/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * process.c - Process creation, user stack preparation, and Ring-3 execution.
 */

#include <stdint.h>
#include <stddef.h>
#include "process.h"
#include "elf.h"
#include "vdso.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"
#include "fs/vfs/vfs.h"
#include "include/serial.h"

extern void jump_to_ring3(uint64_t entry, uint64_t user_rsp);

struct process *current_process = NULL;
struct process *process_list = NULL;
static uint32_t next_pid = 1;

struct process *process_find(uint32_t pid) {
    struct process *p = process_list;
    while (p) {
        if (p->pid == pid || p->tgid == pid) return p;
        p = p->next;
    }
    return NULL;
}

struct process *process_iter(struct process *prev) {
    if (!prev) return process_list;
    return prev->next;
}

static void kstrncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
}

/* Live syscall frame captured by syscall_entry (see syscall_asm.asm). Written
 * by interrupt/kernel code and read here to rewrite the ring-3 return. */
extern uint64_t current_syscall_frame;

/* Signals whose default disposition is "terminate". Everything else (SIGCHLD,
 * SIGWINCH, SIGURG, the stop signals, ...) is ignored by default: without job
 * control there is nowhere to stop a process to, and killing on SIGTSTP would
 * take the shell down with the job. */
static const uint8_t sig_fatal_default[65] = {
    [1] = 1, [2] = 1, [3] = 1, [4] = 1, [5] = 1, [6] = 1, [7] = 1, [8] = 1,
    [11] = 1, [13] = 1, [15] = 1,
};

/* Raise: record only. Signals may be raised from interrupt context (IRQ1
 * turns Ctrl-C into SIGINT) where switching process or tearing down a ring-3
 * frame is unsafe, so raising never acts -- it only sets the pending bit.
 * Action happens in deliver_pending_signals() at the syscall boundary. */
void process_raise(int sig) {
    if (current_process && sig >= 1 && sig <= 64) {
        current_process->sig_pending |= 1ULL << (sig - 1);
    }
}

/* Called once per PIT tick from IRQ context. Fires expired ITIMER_REAL
 * timers by raising SIGALRM (signal 14); periodic timers reload from
 * the current tick so a late tick cannot double-fire. Only touches
 * pending bits; the syscall-return path performs delivery. */
extern uint64_t timer_get_ticks(void);
void proc_timer_tick(void) {
    uint64_t now = timer_get_ticks();
    for (struct process *p = process_list; p; p = p->next) {
        if (p->exited || p->itimer_expire == 0)
            continue;
        if (now < p->itimer_expire)
            continue;
        p->sig_pending |= 1ULL << (14 - 1); /* SIGALRM */
        if (p->itimer_interval)
            p->itimer_expire = now + p->itimer_interval;
        else
            p->itimer_expire = 0;
    }
}

/* Kept for callers that want to poke the signal machinery from process
 * context (blocking-read spin loops). Delivery is deferred to the syscall
 * return path, so there is nothing to do here. */
void sig_check_current(void) {
}

/*
 * deliver_pending_signals - act on the lowest pending, unblocked signal.
 *
 * Called from the syscall dispatcher immediately before the frame is popped
 * back into ring 3, which is the only point where a handler may be entered
 * safely: the live syscall frame is writable and the interrupted call can be
 * resumed later by rt_sigreturn().
 *
 * Dispositions:
 *   SIG_IGN   -> drop the signal
 *   SIG_DFL   -> fatal set terminates (128+n); everything else is ignored
 *                (we have no job control, so stop signals must not kill)
 *   handler   -> stash the frame, point RIP at the handler, RSP at the
 *                restorer, RDI/RSI/RDX at (signo, NULL, NULL)
 */
void deliver_pending_signals(int64_t ret) {
    struct process *p = current_process;
    if (!p || p->exited || !current_syscall_frame) return;

    /* SIGKILL/SIGSTOP cannot be blocked or ignored. */
    uint64_t unavoidable = 1ULL << 8 /*9*/ | 1ULL << 18 /*19*/;
    uint64_t pending = p->sig_pending & (~p->sig_blocked | unavoidable);
    if (!pending) return;

    int sig = 0;
    for (int i = 1; i <= 64; i++) {
        if (pending & (1ULL << (i - 1))) { sig = i; break; }
    }
    if (!sig) return;

    uint64_t bit = 1ULL << (sig - 1);
    p->sig_pending &= ~bit;

    uint64_t handler = p->sig_handlers[sig];

    if (handler == 1) return; /* SIG_IGN */

    if (handler == 0) { /* SIG_DFL */
        if (sig_fatal_default[sig] || sig == 9) {
            serial_puts("[PROC] Terminated by signal ");
            serial_puthex((uint64_t)sig);
            serial_puts("\n");
            process_exit(128 + sig);
        }
        return;
    }

    if (p->in_sighandler) return; /* no nesting: retry on the next return */

    uint64_t *frame = (uint64_t *)current_syscall_frame;

    for (int i = 0; i < 15; i++) p->sig_saved_frame[i] = frame[i];
    p->sig_saved_frame[15] = (uint64_t)ret;
    p->sig_saved_blocked = p->sig_blocked;
    p->in_sighandler = 1;

    /* The handler must find a return address on the stack. libc supplies one
     * (musl's __restore_rt, which issues rt_sigreturn); otherwise plant a
     * `mov eax,15; syscall` stub on the stack itself. */
    /* Place the handler's stack strictly BELOW the interrupted frame's RSP.
     * (((S-8) & ~0xF) + 8) looks like plain 16-byte ABI alignment, but when S
     * is already 8 (mod 16) - the normal case for a syscall inside a function
     * - it evaluates back to S and overwrites the interrupted function's own
     * return address. Subtracting a further 8 first guarantees the handler's
     * frame lands at least 8 bytes below S while still entering with the
     * ABI-expected RSP % 16 == 8. */
    uint64_t old_rsp = frame[14];
    uint64_t nrsp = ((old_rsp - 16) & ~0xFULL) + 8;
    uint64_t restorer = p->sig_restorer[sig];
    if (!restorer) {
        static const uint8_t tramp[8] = { 0xB8, 15, 0, 0, 0, 0x0F, 0x05, 0xC3 };
        nrsp -= sizeof(tramp);
        uint8_t *dst = (uint8_t *)nrsp;
        for (unsigned b = 0; b < sizeof(tramp); b++) dst[b] = tramp[b];
        restorer = nrsp;
    }
    *(uint64_t *)nrsp = restorer;

    frame[0] = (uint64_t)sig;  /* RDI: signo                 */
    frame[1] = 0;              /* RSI: siginfo_t * (unused)  */
    frame[2] = 0;              /* RDX: ucontext_t * (unused) */
    frame[12] = handler;       /* RIP: handler entry         */
    frame[14] = nrsp;          /* RSP: restorer return slot  */

    p->sig_blocked |= bit; /* handler runs with its own signal masked */

    /* Remember where we planted the restorer: rt_sigreturn clears it so a
     * later `ret` at this stack depth cannot re-enter the trampoline. */
    p->sig_frame_nrsp = nrsp;

    return;
}

int64_t sig_restore_frame(void) {
    struct process *p = current_process;
    if (!p || !p->in_sighandler || !current_syscall_frame) return 0;

    uint64_t *frame = (uint64_t *)current_syscall_frame;
    int64_t restored = (int64_t)p->sig_saved_frame[15];
    for (int i = 0; i < 15; i++) frame[i] = p->sig_saved_frame[i];

    p->in_sighandler = 0;
    p->sig_blocked = p->sig_saved_blocked;
    /* Poison the restorer slot: without this, the stale __restore_rt
     * address below the restored RSP gets popped by a later `ret` in the
     * interrupted call chain, re-entering rt_sigreturn outside any signal
     * context (in_sighandler == 0) and crashing the process. */
    if (p->sig_frame_nrsp)
        *(volatile uint64_t *)p->sig_frame_nrsp = 0;
    p->sig_frame_nrsp = 0;
    return restored;
}

void process_init(void) {
    serial_puts("[PROC] Process management subsystem initialized.\n");
}

/* Build the initial user stack image (auxv + argv/envp strings) and return
 * the entry RSP. Shared by process_create_from_elf and process_execve. */
static int build_user_stack(pml4_t *proc_pml4, int argc, const char **argv,
                            const char **envp, const struct elf_load_info *info,
                            uint32_t uid, uint32_t euid,
                            uint32_t gid, uint32_t egid,
                            uint64_t *user_rsp_out) {
    uint64_t top_page_phys = vmm_virt_to_phys(proc_pml4, USER_STACK_TOP - PAGE_SIZE);
    if (!top_page_phys) return -1;
    uint8_t *page_virt = (uint8_t *)(0xFFFF800000000000ULL + (top_page_phys & PAGE_ADDR_MASK));

    /* Reserve 16 random bytes at the very top of the stack for AT_RANDOM.
     * sp starts BELOW the reservation so later strings/auxv never clobber it. */
    uint8_t *stack_byte_sp = page_virt + PAGE_SIZE - 16;
    rng_fill(stack_byte_sp, 16);
    uint64_t rand_bytes = USER_STACK_TOP - 16;

    if (argc > 64) argc = 64;

    int envc = 0;
    if (envp) {
        while (envp[envc] && envc < 32) envc++;
    }

    uint64_t argv_addrs[64];
    for (int i = argc - 1; i >= 0; i--) {
        const char *s = argv[i] ? argv[i] : "";
        size_t len = 0;
        while (s[len]) len++;
        stack_byte_sp -= (len + 1);
        for (size_t j = 0; j <= len; j++) stack_byte_sp[j] = s[j];
        argv_addrs[i] = USER_STACK_TOP - PAGE_SIZE + (uint64_t)(stack_byte_sp - page_virt);
    }

    uint64_t envp_addrs[32];
    for (int i = envc - 1; i >= 0; i--) {
        const char *s = envp[i] ? envp[i] : "";
        size_t len = 0;
        while (s[len]) len++;
        stack_byte_sp -= (len + 1);
        for (size_t j = 0; j <= len; j++) stack_byte_sp[j] = s[j];
        envp_addrs[i] = USER_STACK_TOP - PAGE_SIZE + (uint64_t)(stack_byte_sp - page_virt);
    }

    stack_byte_sp = (uint8_t *)((uint64_t)stack_byte_sp & ~7ULL);
    uint64_t *sp = (uint64_t *)stack_byte_sp;

    /* Push auxv: [type, value] (value-first pairs) */
    *(--sp) = 0;                    *(--sp) = AT_NULL;
    *(--sp) = rand_bytes;           *(--sp) = AT_RANDOM;
    *(--sp) = 16;                   *(--sp) = AT_HWCAP;
    *(--sp) = 0;                    *(--sp) = AT_SECURE;
    *(--sp) = (uint64_t)egid;       *(--sp) = AT_EGID;
    *(--sp) = (uint64_t)gid;        *(--sp) = AT_GID;
    *(--sp) = (uint64_t)euid;       *(--sp) = AT_EUID;
    *(--sp) = (uint64_t)uid;        *(--sp) = AT_UID;
    *(--sp) = VDSO_BASE;            *(--sp) = AT_SYSINFO_EHDR;
    *(--sp) = info->entry_point;    *(--sp) = AT_ENTRY;
    *(--sp) = info->phnum;          *(--sp) = AT_PHNUM;
    *(--sp) = info->phent;          *(--sp) = AT_PHENT;
    *(--sp) = info->phdr_vaddr;     *(--sp) = AT_PHDR;
    *(--sp) = 4096;                 *(--sp) = AT_PAGESZ;

    /* Push envp array (null-terminated) */
    *(--sp) = 0;
    for (int i = envc - 1; i >= 0; i--) *(--sp) = envp_addrs[i];

    /* Push argv array (null-terminated) */
    *(--sp) = 0;
    for (int i = argc - 1; i >= 0; i--) *(--sp) = argv_addrs[i];

    /* Push argc */
    *(--sp) = (uint64_t)argc;

    uint64_t elements_pushed = (uint64_t)((uint64_t *)(page_virt + PAGE_SIZE) - sp);
    *user_rsp_out = USER_STACK_TOP - (elements_pushed * sizeof(uint64_t));
    return 0;
}

/* Default environment for processes spawned without an explicit envp
 * (TERM is required by slang/curses — the whiptail dependency — and
 * PATH/HOME/SHELL make simple shells usable out of the box).
 * /usr/basisd carries the default init (basis-core) and its tools, so
 * basisctl and basis-log resolve without touching every manifest. */
static const char *default_envp[] = {
    "TERM=linux",
    "TERMINFO=/etc/terminfo",
    "PATH=/bin:/sbin:/usr/bin:/usr/basisd/bin:/usr/basisd/sbin",
    "HOME=/root",
    "SHELL=/bin/sh",
    "HOSTNAME=eigen",
    NULL, /* replaced at runtime by the TZ entry (see default_envp_full) */
    NULL
};

/* The configured zone as a "TZ=<zone>" string, or NULL when /etc/timezone is
 * absent or unreadable. Re-read on every exec rather than cached, so
 * `basisctl tzset` affects the next command with no reboot.
 *
 * Lifetime: the returned pointer is a static buffer, overwritten by the next
 * call. Both consumers (the default-env slot below, and the execve filter)
 * consume it before any other exec can happen, so this is safe. */
static const char *process_tz_env(void) {
    static char tzbuf[128];

    int fd = vfs_open("/etc/timezone", O_RDONLY, 0);
    if (fd < 0) return NULL;
    char zone[96];
    int64_t n = vfs_read(fd, zone, sizeof(zone) - 1);
    vfs_close(fd);
    if (n <= 0) return NULL;
    zone[n] = '\0';
    for (int i = 0; zone[i]; i++) {
        if (zone[i] == '\n' || zone[i] == '\r' ||
            zone[i] == ' '  || zone[i] == '\t') {
            zone[i] = '\0';
            break;
        }
    }
    if (!zone[0]) return NULL;
    const char *p = "TZ=";
    int i = 0;
    for (; p[i]; i++) tzbuf[i] = p[i];
    for (int k = 0; zone[k] && i < (int)sizeof(tzbuf) - 1; k++)
        tzbuf[i++] = zone[k];
    tzbuf[i] = '\0';
    return tzbuf;
}

static const char **default_envp_full(void) {
    default_envp[6] = process_tz_env();
    return default_envp;
}

/* execve keeps the caller's environment, minus any TZ= it carries. A shell
 * spawned at boot keeps exporting whatever TZ= it started with, which would
 * otherwise shadow /etc/localtime for every program it ever starts and make
 * `basisctl tzset` look broken. The system zone from /etc/timezone is
 * authoritative; everything else passes through untouched. The returned array
 * is a static buffer consumed by build_user_stack before returning. */
#define EXEC_ENVP_MAX 32

static const char **envp_with_system_tz(const char **envp) {
    static const char *filtered[EXEC_ENVP_MAX + 1];
    int n = 0;
    if (envp) {
        for (int i = 0; envp[i] && n < EXEC_ENVP_MAX - 1; i++) {
            const char *e = envp[i];
            if (e[0] == 'T' && e[1] == 'Z' && e[2] == '=') continue;
            filtered[n++] = e;
        }
    }
    const char *tz = process_tz_env();
    if (tz && n < EXEC_ENVP_MAX - 1) filtered[n++] = tz;
    filtered[n] = NULL;
    return filtered;
}

struct process *process_create_from_elf(const char *path, const char *name, int argc, const char **argv) {
    struct vfs_node *file = vfs_lookup(path);
    if (!file) {
        serial_puts("[PROC] Executable file not found: ");
        serial_puts(path);
        serial_puts("\n");
        return NULL;
    }

    pml4_t *proc_pml4 = vmm_create_address_space();
    if (!proc_pml4) {
        serial_puts("[PROC] Failed to allocate PML4 for new process!\n");
        return NULL;
    }

    struct elf_load_info info;
    if (elf_load_file(file, proc_pml4, &info) != 0) {
        serial_puts("[PROC] Failed to parse and load ELF!\n");
        return NULL;
    }
    vdso_map(proc_pml4);

    uint64_t stack_base = USER_STACK_TOP - USER_STACK_SIZE;
    for (uint64_t v = stack_base; v < USER_STACK_TOP; v += PAGE_SIZE) {
        void *phys = pmm_alloc_page();
        if (!phys) {
            serial_puts("[PROC] Failed allocating user stack pages!\n");
            return NULL;
        }
        vmm_map_page(proc_pml4, v, (uint64_t)phys, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    }

    const char *default_argv[2];
    if (argc <= 0 || !argv) {
        argc = 1;
        default_argv[0] = name ? name : path;
        default_argv[1] = NULL;
        argv = default_argv;
    }

    uint64_t user_rsp;
    if (build_user_stack(proc_pml4, argc, argv, default_envp_full(), &info,
                         0, 0, 0, 0, &user_rsp) != 0) {
        serial_puts("[PROC] Failed to build user stack!\n");
        return NULL;
    }

    struct process *proc = (struct process *)kzalloc(sizeof(struct process));
    proc->pid = next_pid++;
    proc->pml4 = proc_pml4;
    proc->entry_point = info.entry_point;
    proc->user_stack_top = user_rsp;
    proc->brk_base = info.brk_base;
    proc->brk = info.brk_base;
    proc->mmap_next = USER_MMAP_BASE;
    kstrncpy(proc->name, name ? name : path, sizeof(proc->name));
    proc->ppid = current_process ? current_process->pid : 0;
    proc->tgid = proc->pid;
    proc->tid = proc->pid;
    proc->parent = current_process;
    proc->cwd[0] = '/';
    proc->cwd[1] = '\0';
    proc->umask = 022;
    proc->uid = 0;
    proc->euid = 0;
    proc->gid = 0;
    proc->egid = 0;
    proc->sid = proc->pid;
    proc->pgid = proc->pid;
    proc->has_ctty = 1;
    vfs_install_stdio(proc->fds);
    proc->next = process_list;
    process_list = proc;

    serial_puts("[PROC] Created process '");
    serial_puts(proc->name);
    serial_puts("' (PID ");
    serial_puthex(proc->pid);
    serial_puts(") Entry: ");
    serial_puthex(proc->entry_point);
    serial_puts(" RSP: ");
    serial_puthex(proc->user_stack_top);
    serial_puts("\n");

    return proc;
}

extern int set_kernel_context(void *ctx);
extern void return_to_kernel_context(void *ctx, int status);
extern void resume_userspace_from_frame(const uint64_t *frame);

static uint64_t kernel_ctx[8];

static void wrmsr_fs_base(uint64_t v) {
    uint32_t lo = (uint32_t)v;
    uint32_t hi = (uint32_t)(v >> 32);
    __asm__ volatile("wrmsr" :: "c"(0xC0000100ULL), "a"(lo), "d"(hi));
}

static uint64_t rdmsr_fs_base(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100ULL));
    return ((uint64_t)hi << 32) | lo;
}

/* Clone the caller (parent) into a forked child. The child is registered but
 * NOT started; it is executed when the parent wait4()s on it, resuming at the
 * fork syscall with a return value of 0 (see process_run_deferred). */
struct process *process_fork(struct process *parent, const uint64_t *syscall_frame) {
    if (!parent || !syscall_frame) return NULL;

    struct process *child = (struct process *)kzalloc(sizeof(struct process));
    if (!child) {
        serial_puts("[PROC] fork: kzalloc(PCB) failed\\n");
        return NULL;
    }

    child->pml4 = vmm_clone_address_space(parent->pml4);
    if (!child->pml4) {
        serial_puts("[PROC] fork: address space clone failed (OOM?)\\n");
        kfree(child);
        return NULL;
    }

    for (int i = 0; i < 15; i++) child->fork_frame[i] = syscall_frame[i];

    child->pid = next_pid++;
    child->ppid = parent->pid;
    child->tgid = child->pid;
    child->tid = child->pid;
    child->is_thread = 0;
    child->parent = parent;

    child->brk_base = parent->brk_base;
    child->brk = parent->brk;
    child->mmap_next = parent->mmap_next;
    child->entry_point = parent->entry_point;
    child->user_stack_top = parent->user_stack_top;

    kstrncpy(child->name, parent->name, sizeof(child->name));
    kstrncpy(child->cwd, parent->cwd, sizeof(child->cwd));
    child->umask = parent->umask;
    child->uid = parent->uid;
    child->euid = parent->euid;
    child->gid = parent->gid;
    child->egid = parent->egid;
    child->sid = parent->sid;
    child->has_ctty = parent->has_ctty;
    child->fs_base = parent->fs_base;
    child->gs_base = parent->gs_base;
    child->set_tid_addr = parent->set_tid_addr;

    /* Signals: pending bits are per-process (child starts clean), blocked
     * mask and dispositions are inherited. */
    child->sig_pending = 0;
    child->sig_blocked = parent->sig_blocked;
    for (int i = 0; i <= 64; i++) {
        child->sig_handlers[i] = parent->sig_handlers[i];
        child->sig_flags[i] = parent->sig_flags[i];
        child->sig_restorer[i] = parent->sig_restorer[i];
    }

    vfs_clone_table(child->fds, parent->fds);

    /* Kernel stack for nested runs: leave a 1 KB guard below the top so
     * syscall entry frames cannot clobber the deferred-runner's context. */
    child->kstack_top = (uint64_t)&child->kstack[sizeof(child->kstack)] - 1024;

    child->started = 0;
    child->nested = 1;

    child->next = process_list;
    process_list = child;

    return child;
}

/* Run a forked child to completion (nested inside the parent's wait4 syscall).
 * Returns the child's exit status. */
int process_run_deferred(struct process *child) {
    if (!child) return -1;
    struct process *parent = current_process;
    uint64_t saved_kstack = current_kstack_top;
    /* The parent is parked inside its own wait4 syscall; the child's syscalls
     * overwrite the global frame pointer, so restore it on the way out. */
    uint64_t saved_frame = current_syscall_frame;

    /* FS_BASE is a *live* per-CPU MSR, not part of the syscall frame. A forked
     * child inherits the caller's TLS pointer (its cloned address space maps
     * the same virtual addresses), but once it execve()s a different binary it
     * points FS at that binary's TLS block. That pointer is meaningless in the
     * parent's address space, so capture the parent's real value here and put
     * it back unconditionally on the way out — otherwise the parent's next
     * %fs:0x28 stack-canary read faults in an unmapped page. */
    uint64_t saved_fs = rdmsr_fs_base();

    child->started = 1;
    current_process = child;
    current_kstack_top = child->kstack_top;
    vmm_switch_address_space(child->pml4);

    int status = set_kernel_context(child->saved_kernel_ctx);
    if (status != 0) {
        /* Child exited (longjmp from process_exit): restore the parent's
         * execution environment. */
        current_kstack_top = saved_kstack;
        current_syscall_frame = saved_frame;
        current_process = parent;
        vmm_switch_address_space(parent ? parent->pml4 : vmm_get_kernel_address_space());
        wrmsr_fs_base(saved_fs);
        return child->exit_status;
    }

    /* Enter the child at its fork-return point (RAX = 0); never returns. */
    resume_userspace_from_frame(child->fork_frame);
    __builtin_unreachable();
}

/* Replace the process image (execve). On success this never returns: the
 * process enters ring 3 at the new entry point. */
int process_execve(struct process *p, const char *path,
                   int argc, const char **argv, const char **envp) {
    if (!p || !path) return -14; /* -EFAULT */

    struct vfs_node *file = vfs_lookup(path);
    if (!file) return -2; /* -ENOENT */

    pml4_t *new_pml4 = vmm_create_address_space();
    if (!new_pml4) return -12; /* -ENOMEM */

    struct elf_load_info info;
    if (elf_load_file(file, new_pml4, &info) != 0) {
        serial_puts("[PROC] execve: ELF load failed: ");
        serial_puts(path);
        serial_puts("\n");
        return -8; /* -ENOEXEC */
    }
    vdso_map(new_pml4);

    uint64_t stack_base = USER_STACK_TOP - USER_STACK_SIZE;
    for (uint64_t v = stack_base; v < USER_STACK_TOP; v += PAGE_SIZE) {
        void *phys = pmm_alloc_page();
        if (!phys) return -12;
        vmm_map_page(new_pml4, v, (uint64_t)phys, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    }

    if (argc <= 0 || !argv) {
        static const char *fallback[2];
        static char namebuf[64];
        const char *app = path;
        for (const char *q = path; *q; q++) {
            if (*q == '/') app = q + 1;
        }
        kstrncpy(namebuf, app, sizeof(namebuf));
        fallback[0] = namebuf;
        fallback[1] = NULL;
        argc = 1;
        argv = fallback;
    }

    uint64_t user_rsp;
    if (build_user_stack(new_pml4, argc, argv, envp_with_system_tz(envp), &info,
                         p->uid, p->euid, p->gid, p->egid, &user_rsp) != 0) {
        return -12;
    }

    /* Replace the image. NOTE: the old address space is intentionally leaked
     * until vmm_destroy_address_space() exists. */
    p->pml4 = new_pml4;
    p->entry_point = info.entry_point;
    p->brk_base = info.brk_base;
    p->brk = info.brk_base;
    p->mmap_next = USER_MMAP_BASE;
    kstrncpy(p->name, path, sizeof(p->name));

    /* POSIX: caught signals reset to default on exec */
    p->sig_pending = 0;
    for (int i = 0; i <= 64; i++) {
        p->sig_handlers[i] = 0;
        p->sig_flags[i] = 0;
        p->sig_restorer[i] = 0;
    }

    serial_puts("[PROC] execve '");
    serial_puts(p->name);
    serial_puts("' entry ");
    serial_puthex(info.entry_point);
    serial_puts("\n");

    vmm_switch_address_space(new_pml4);
    jump_to_ring3(info.entry_point, user_rsp);
    __builtin_unreachable();
}

int process_run(struct process *proc) {
    if (!proc) return -1;

    current_process = proc;

    serial_puts("[PROC] Switching to Ring 3 for '");
    serial_puts(proc->name);
    serial_puts("'...\n");

    int status = set_kernel_context(kernel_ctx);
    if (status != 0) {
        /* Longjmp from process_exit: the actual exit status was stored in
         * the PCB (see process_exit). The return value is just a sentinel. */
        int exit_status = proc->exit_status;
        current_process = NULL;
        vmm_switch_address_space(vmm_get_kernel_address_space());
        __asm__ volatile("sti"); /* Re-enable interrupts for kernel shell */
        serial_puts("[PROC] Process '");
        serial_puts(proc->name);
        serial_puts("' exited with status: ");
        serial_puthex((uint64_t)(int64_t)exit_status);
        serial_puts("\n");
        return exit_status;
    }

    /* Switch to process page table */
    vmm_switch_address_space(proc->pml4);

    /* Jump to Ring 3 via iretq */
    jump_to_ring3(proc->entry_point, proc->user_stack_top);
    return 0;
}

void process_exit(int status) {
    struct process *p = current_process;
    if (p) {
        /* POSIX exit statuses are 0-255. Negative values from exit(-1) are
         * clamped to 0 to avoid sign-extension issues in the shell's
         * wait-status handling (ash crashes on some negative paths). */
        p->exited = 1;
        p->exit_status = status < 0 ? 0 : status;
        /* Linux notifies the parent with SIGCHLD on every child state
         * change. Default disposition ignores it (see sig_fatal_default),
         * so supervisors that poll with wait4(WNOHANG) see no change while
         * SIGCHLD handlers/waitpid sleepers start working.
         * Deferred-model exception: a nested child only ever runs inside
         * its parent's wait4(), which reaps it synchronously on return.
         * Raising SIGCHLD here would make deliver_pending_signals rewrite
         * the parent's live frame to enter ash's SIGCHLD handler on the
         * very next return — and the rt_sigreturn restore hands the shell
         * a garbage RAX instead of the child's PID. So skip the raise on
         * the nested path; the parent already has everything via wait4. */
        if (p->parent && !p->parent->exited && !(p->nested && p->parent))
            p->parent->sig_pending |= 1ULL << (17 - 1); /* SIGCHLD */

        /* Release the process's descriptors. A kernel socket stays bound to
         * its name until the last fd referring to it closes, so without this
         * a compositor that exits keeps /tmp/wayland-0 claimed forever and
         * the next one fails with EADDRINUSE even though nothing is
         * listening. vfs_close works on current_process's table, which is
         * this PCB while the process is running its exit path. */
        for (int fd = 0; fd < MAX_FD_COUNT; fd++) {
            if (p->fds[fd].is_used) vfs_close(fd);
        }
    }

    /* set_kernel_context returns 0 for "first entry, continue"; any non-zero
     * value means "longjmp from process_exit". We always pass 1 as the
     * sentinel and read the actual exit status from the PCB afterwards.
     * (status + 1 was ambiguous: exit(-1) + 1 == 0 == the first-entry sentinel,
     * which made the runner re-enter ring 3 instead of returning to the
     * parent.) */
    if (p && p->nested && p->parent) {
        /* Forked child exiting inside the parent's wait4: longjmp back to
         * the deferred runner (process_run_deferred). */
        current_process = NULL;
        vmm_switch_address_space(vmm_get_kernel_address_space());
        __asm__ volatile("sti");
        return_to_kernel_context(p->saved_kernel_ctx, 1);
    }

    current_process = NULL;
    vmm_switch_address_space(vmm_get_kernel_address_space());
    __asm__ volatile("sti");
    return_to_kernel_context(kernel_ctx, 1);
}

/* Remove a reaped process from the global list (fixes ps ghost entries). */
void process_reap(struct process *p) {
    if (!p) return;
    struct process **pp = &process_list;
    while (*pp) {
        if (*pp == p) {
            *pp = p->next;
            p->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* Signal a process other than the caller.
 *
 * There is no preemption here: a child only executes inside its parent's
 * wait4(), so an asynchronous handler entry is impossible. Instead the signal
 * is recorded as pending and delivered the next time the target is about to
 * return to ring 3 (deliver_pending_signals), which gives us SIG_IGN,
 * caught-signal and default-disposition behaviour for free. SIGKILL is the
 * exception: it must always bite, even for a runaway process that never
 * issues another syscall, so it is turned straight into an exit status. */
static void kill_other(struct process *p, int sig) {
    if (!p || sig <= 0 || p->exited) return;

    if (sig == 9) {
        p->exited = 1;
        p->exit_status = 128 + 9;
        serial_puts("[PROC] PID ");
        serial_puthex(p->pid);
        serial_puts(" SIGKILLed\n");
        return;
    }
    if (p->sig_handlers[sig] == 1) return; /* SIG_IGN: nobody wants it */
    p->sig_pending |= 1ULL << (sig - 1);
}

/* Send a signal to a process or process group. Returns 0 or negative errno. */
int process_kill(uint32_t pid_raw, int sig) {
    if (sig < 0 || sig > 64) return -22; /* -EINVAL */

    int32_t spid = (int32_t)pid_raw;
    struct process *self = current_process;

    /* Permission (POSIX): euid 0 may signal anyone; otherwise the sender's
     * ruid/euid must match the target's ruid/euid. Applies to sig 0 too. */
    uint32_t self_uid = self ? self->uid : 0;
    uint32_t self_euid = self ? self->euid : 0;
    int privileged = (self_euid == 0);

    /* pid == 0 broadcasts to the caller's group; a negative pid names a
     * group explicitly (kill(-pgrp, sig)). */
    uint32_t grp = 0;
    if (spid == 0) grp = self ? self->pgid : 0;
    else if (spid < 0) grp = (uint32_t)(-spid);

    if (grp) {
        int n = 0, denied = 0;
        for (struct process *q = process_iter(NULL); q; q = process_iter(q)) {
            if (q->pgid != grp) continue;
            if (q == self) {
                if (sig > 0) process_raise(sig);
            } else {
                if (!privileged && self_uid != q->uid && self_euid != q->uid &&
                    self_uid != q->euid && self_euid != q->euid) { denied++; continue; }
                kill_other(q, sig);
            }
            n++;
        }
        if (n) return 0;
        if (denied) return -1; /* -EPERM: group exists, nothing signalable */
    }

    struct process *p = process_find((uint32_t)spid);
    if (!p) return -3; /* -ESRCH */

    if (!privileged && p != self &&
        self_uid != p->uid && self_euid != p->uid &&
        self_uid != p->euid && self_euid != p->euid) return -1; /* -EPERM */

    if (p == self) {
        /* Self-signal: record it and let the syscall return path enter the
         * handler (or apply the default disposition). */
        if (sig > 0) process_raise(sig);
        return 0;
    }

    kill_other(p, sig);
    return 0;
}
