/* clone_test - raw clone/futex mechanics (vfork discipline: the child
 * shares the user stack, so it stores a flag and exits, nothing else).
 *
 * Explicit child stacks need a real clone wrapper (musl __clone), not raw
 * syscall(): musl's syscall() resumes mid-marshaller and a fresh rsp
 * orphans its spill frame. Same breakage exists on Linux.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/time.h>

#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#ifndef SYS_futex
#define SYS_futex 202
#endif

static int fails = 0;
#define CHECK(c, msg) do { \
    if (c) { printf("PASS %s\n", msg); } \
    else { printf("FAIL %s\n", msg); fails++; } \
} while (0)

static volatile int flag;
static int runs;

struct kclone_args {
    unsigned long long flags;
    unsigned long long pidfd;
    unsigned long long child_tid;
    unsigned long long parent_tid;
    unsigned long long exit_signal;
    unsigned long long stack;
    unsigned long long stack_size;
    unsigned long long tls;
    unsigned long long set_tid;
    unsigned long long set_tid_size;
    unsigned long long cgroup;
};

int main(void) {
    runs++;
    /* Plain fork-like clone. */
    flag = 0;
    long r = syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0);
    if (r == 0) {
        flag = 42;
        syscall(SYS_exit, 7);
    }
    CHECK(r > 0, "clone-ret-tid");
    CHECK(flag == 0, "clone-no-vm-copy");

    /* Shared-VM clone: child write is visible to the parent. */
    flag = 0;
    int vmptid = 0;
    r = syscall(SYS_clone, SIGCHLD | CLONE_VM | CLONE_FS | CLONE_FILES |
                CLONE_SIGHAND | CLONE_PARENT_SETTID, 0, &vmptid, 0, 0);
    if (r == 0) {
        flag = 42;
        syscall(SYS_exit, 7);
    }
    printf("dbg vmclone r=%ld ptid=%d flag=%d\n", r, vmptid, flag);
    CHECK(r > 0, "clone-vm-ret");
    CHECK(vmptid == r, "clone-vm-ptid");
    CHECK(flag == 42, "clone-vm-shared");

    /* Thread clone with own TLS base; threads are not waitable. */
    flag = 0;
    int ptid = 0, ctid = 0;
    unsigned long tp;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tp));
    static unsigned long child_tp[32];
    r = syscall(SYS_clone, SIGCHLD | CLONE_VM | CLONE_FS | CLONE_FILES |
                CLONE_SIGHAND | CLONE_THREAD | CLONE_SETTLS |
                CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID,
                0, &ptid, &ctid, child_tp);
    if (r == 0) {
        child_tp[0] = 0x51AB51AB51AB51ABUL;
        unsigned long mine;
        __asm__ volatile("mov %%fs:0, %0" : "=r"(mine));
        flag = (mine == 0x51AB51AB51AB51ABUL) ? 11 : 12;
        syscall(SYS_exit, 0);
    }
    CHECK(r > 0, "clone-thread-ret");
    CHECK(ptid == r, "clone-parent-settid");
    CHECK(ctid == 0, "clone-child-cleartid");
    CHECK(flag == 11, "clone-thread-tls");
    int st = 0;
    CHECK(waitpid(r, &st, WNOHANG) == -1, "clone-thread-nowait");

    /* clone3 with fork-like flags. */
    flag = 0;
    struct kclone_args ca;
    memset(&ca, 0, sizeof(ca));
    ca.flags = SIGCHLD;
    r = syscall(SYS_clone3, &ca, sizeof(ca));
    if (r == 0) {
        flag = 43;
        syscall(SYS_exit, 8);
    }
    CHECK(r > 0, "clone3-ret");
    CHECK(flag == 0, "clone3-copy");

    /* Futex: mismatched word is EAGAIN, wake is 0. */
    int fw = 0;
    CHECK(syscall(SYS_futex, &fw, 0, 1, 0) == -1, "futex-eagain");
    CHECK(syscall(SYS_futex, &fw, 1, 0, 0) == 0, "futex-wake");
    struct timeval t0, t1;
    gettimeofday(&t0, 0);
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 50000000;
    long fr = syscall(SYS_futex, &fw, 0, 0, &ts);
    gettimeofday(&t1, 0);
    long elapsed = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;
    CHECK(fr == -1 && elapsed >= 30 && elapsed < 2000, "futex-timeout");

    printf(fails ? "CLONE-TEST FAIL %d\n" : "CLONE-TEST PASS\n", fails);
    return fails != 0;
}
