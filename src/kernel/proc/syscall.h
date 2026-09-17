/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * syscall.h - x86_64 System Call interface (Linux x86_64 ABI compatible).
 */

#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include "mm/vmm.h"
#include "fs/vfs/vfs.h"

/* User-space address limit (same as Linux x86_64 canonical range). */
#define USER_MMAP_END   0x7FFF00000000ULL

/* Standard Linux x86_64 Syscall Numbers */
#define SYS_READ            0
#define SYS_WRITE           1
#define SYS_OPEN            2
#define SYS_CLOSE           3
#define SYS_STAT            4
#define SYS_FSTAT           5
#define SYS_LSTAT           6
#define SYS_POLL            7
#define SYS_PPOLL           271
#define SYS_LSEEK           8
#define SYS_MMAP            9
#define SYS_MPROTECT        10
#define SYS_MUNMAP          11
#define SYS_BRK             12
#define SYS_RT_SIGACTION    13
#define SYS_RT_SIGPROCMASK  14
#define SYS_RT_SIGRETURN    15
#define SYS_IOCTL           16
#define SYS_PREAD64         17
#define SYS_PWRITE64        18
#define SYS_READV           19
#define SYS_WRITEV          20
#define SYS_ACCESS          21
#define SYS_PIPE            22
#define SYS_SELECT          23
#define SYS_DUP             32
#define SYS_DUP2            33
#define SYS_PAUSE           34
#define SYS_NANOSLEEP       35
#define SYS_CLOCK_NANOSLEEP 230
#define SYS_PSELECT6        270
#define SYS_GETITIMER       36

/* Kernel-side timespec (matches POSIX layout for musl compatibility) */
struct ktimespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};
#define SYS_SETITIMER       38
#define SYS_GETPID          39
#define SYS_SETSID          112
#define SYS_GETRLIMIT       97
#define SYS_GETRUSAGE       98
#define SYS_SETPGID         109
#define SYS_SCHED_YIELD    24
#define SYS_SCHED_GETAFFINITY 204
#define SYS_SCHED_SETAFFINITY 203
#define SYS_GETPGID        121
#define SYS_TGKILL         234
#define SYS_TKILL          200
#define SYS_SIGALTSTACK    131
#define SYS_SYSINFO        99
#define SYS_SETRLIMIT      160
#define SYS_GETPGRP        111
#define SYS_WAITID         247
#define SYS_SENDFILE        40
#define SYS_RENAME          82
#define SYS_SOCKET          41
#define SYS_CONNECT         42
#define SYS_ACCEPT          43
#define SYS_SENDTO          44
#define SYS_RECVFROM        45
#define SYS_SENDMSG         46
#define SYS_RECVMSG         47
#define SYS_SHUTDOWN        48
#define SYS_BIND            49
#define SYS_LISTEN          50
#define SYS_GETSOCKNAME     51
#define SYS_GETPEERNAME     52
#define SYS_SOCKETPAIR      53
#define SYS_SETSOCKOPT      54
#define SYS_GETSOCKOPT      55
#define SYS_FORK            57
#define SYS_CLONE           56
#define SYS_EXECVE          59
#define SYS_EXIT            60
#define SYS_WAIT4           61
#define SYS_KILL            62
#define SYS_UNAME           63
#define SYS_FCNTL           72
#define SYS_FLOCK           73
#define SYS_FSYNC           74
#define SYS_FDATASYNC       75
#define SYS_TRUNCATE        76
#define SYS_FTRUNCATE       77
#define SYS_MEMFD_CREATE    319

/* epoll: wait/ctl/create1 (create1 is the one musl actually uses) */
#define SYS_EPOLL_WAIT      232
#define SYS_EPOLL_CTL       233
#define SYS_EPOLL_PWAIT     281
#define SYS_EPOLL_CREATE1   291

/* The event-loop channels libwayland builds on: eventfd for wakeups,
 * timerfd for the frame timer heap, signalfd for child/exit sources. */
#define SYS_EVENTFD         284
#define SYS_EVENTFD2        290
#define SYS_TIMERFD_CREATE  283
#define SYS_TIMERFD_SETTIME 286
#define SYS_TIMERFD_GETTIME 287
#define SYS_SIGNALFD        282
#define SYS_SIGNALFD4       289
#define SYS_ACCEPT4         288
#define SYS_GETDENTS        78
#define SYS_GETCWD          79
#define SYS_CHDIR           80
#define SYS_MKDIR           83
#define SYS_RMDIR           84
#define SYS_UNLINK          87
#define SYS_READLINK        89
#define SYS_CHMOD           90
#define SYS_FCHMOD          91
#define SYS_CHOWN           92
#define SYS_FCHOWN          93
#define SYS_LCHOWN          94
#define SYS_UMASK           95
#define SYS_GETTIMEOFDAY    96
/* Identity/credential syscalls (minimal model: uid/euid/gid/egid per PCB) */
#define SYS_GETUID          102
#define SYS_GETGID          104
#define SYS_SETUID          105
#define SYS_SETGID          106
#define SYS_GETEUID         107
#define SYS_GETEGID         108
#define SYS_SETREUID        113
#define SYS_SETREGID        114
#define SYS_SETRESUID       117
#define SYS_GETRESUID       118
#define SYS_SETRESGID       119
#define SYS_GETRESGID       120
#define SYS_VFORK           58
#define SYS_GETPPID         110
#define SYS_PRCTL           157
#define SYS_CLOCK_GETRES    229
#define SYS_ARCH_PRCTL      158
#define SYS_SYNC            162
#define SYS_REBOOT          169
#define SYS_GETTID          186
#define SYS_TIME            201
#define SYS_GETCPU          309
#define SYS_GETDENTS64      217
#define SYS_SET_TID_ADDRESS 218
#define SYS_FUTEX           202
#define SYS_CLONE3          435
#define SYS_CLOCK_GETTIME   228
#define SYS_CLOCK_SETTIME   227
#define SYS_EXIT_GROUP      231
#define SYS_OPENAT          257
#define SYS_MKDIRAT         258
#define SYS_FCHOWNAT        260
#define SYS_FUTIMESAT       261
#define SYS_NEWFSTATAT      262
#define SYS_UNLINKAT        263
#define SYS_READLINKAT      267
#define SYS_FCHMODAT        268
#define SYS_FACCESSAT       269
#define SYS_UTIMENSAT       280
#define SYS_PIPE2           293
#define SYS_GETRANDOM       318
#define SYS_RT_SIGPENDING   127
#define SYS_RT_SIGTIMEDWAIT 128
#define SYS_RT_SIGSUSPEND   130
#define SYS_PRLIMIT64       302
#define SYS_UTIME           132
#define SYS_UTIMES          235

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_EMPTY_PATH       0x1000

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

void syscall_init(void);
int64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);
int process_kill(uint32_t pid, int sig);

#endif /* KERNEL_SYSCALL_H */