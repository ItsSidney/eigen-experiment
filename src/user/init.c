/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * init.c - Initial Ring-3 userspace test program.
 * Runs in pure unprivileged user mode and invokes kernel syscalls.
 */

#include <stdint.h>
#include <stddef.h>

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

static inline int64_t sys_write(int fd, const void *buf, size_t count) {
    int64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(1ULL), "D"(fd), "S"(buf), "d"(count)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t sys_writev(int fd, const struct iovec *iov, int iovcnt) {
    int64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(20ULL), "D"(fd), "S"(iov), "d"(iovcnt)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline uint64_t sys_brk(uint64_t brk) {
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(12ULL), "D"(brk)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline void *sys_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t offset) {
    register int64_t r10 __asm__("r10") = flags;
    register int64_t r8  __asm__("r8")  = fd;
    register int64_t r9  __asm__("r9")  = offset;
    void *ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(9ULL), "D"(addr), "S"(len), "d"(prot), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t sys_munmap(void *addr, size_t len) {
    int64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(11ULL), "D"(addr), "S"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t sys_uname(struct utsname *buf) {
    int64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(63ULL), "D"(buf)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t sys_getpid(void) {
    int64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(39ULL)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline void sys_exit(int status) {
    __asm__ volatile(
        "syscall"
        :
        : "a"(60ULL), "D"(status)
        : "rcx", "r11", "memory"
    );
    for (;;) ;
}

static void print(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    sys_write(1, s, len);
}

void _start(void) {
    print("\n[USERLAND] >>> Hello from pure Ring 3 User Space! <<<\n");

    /* 1. Test SYS_getpid */
    int64_t pid = sys_getpid();
    if (pid == 1) {
        print("[USERLAND] [TEST 1/5] sys_getpid() -> PID 1 [PASS]\n");
    }

    /* 2. Test SYS_uname */
    struct utsname u;
    if (sys_uname(&u) == 0) {
        print("[USERLAND] [TEST 2/5] sys_uname() -> OS: ");
        print(u.sysname);
        print(" Kernel: ");
        print(u.version);
        print(" [PASS]\n");
    }

    /* 3. Test SYS_writev */
    static const char p1[] = "[USERLAND] [TEST 3/5] sys_writev() -> ";
    static const char p2[] = "vector I/O chunk 1 + ";
    static const char p3[] = "chunk 2 [PASS]\n";
    struct iovec iov[3];
    iov[0].iov_base = (void *)p1; iov[0].iov_len = sizeof(p1) - 1;
    iov[1].iov_base = (void *)p2; iov[1].iov_len = sizeof(p2) - 1;
    iov[2].iov_base = (void *)p3; iov[2].iov_len = sizeof(p3) - 1;
    sys_writev(1, iov, 3);

    /* 4. Test SYS_brk (Heap allocation) */
    uint64_t initial_brk = sys_brk(0);
    if (initial_brk != 0) {
        uint64_t new_brk = sys_brk(initial_brk + 8192); /* allocate 2 pages */
        if (new_brk >= initial_brk + 8192) {
            /* Write to heap memory */
            volatile uint32_t *heap_mem = (volatile uint32_t *)initial_brk;
            heap_mem[0] = 0xDEADBEEF;
            heap_mem[1000] = 0xCAFEBABE;
            if (heap_mem[0] == 0xDEADBEEF && heap_mem[1000] == 0xCAFEBABE) {
                print("[USERLAND] [TEST 4/5] sys_brk() -> heap expansion and R/W verified [PASS]\n");
            }
        }
    }

    /* 5. Test SYS_mmap / SYS_munmap */
    /* PROT_READ=1 | PROT_WRITE=2 = 3. MAP_PRIVATE=2 | MAP_ANON=0x20 = 0x22 */
    void *mmap_ptr = sys_mmap(NULL, 16384, 3, 0x22, -1, 0);
    if ((int64_t)mmap_ptr > 0) {
        volatile uint64_t *mem = (volatile uint64_t *)mmap_ptr;
        mem[0] = 0x123456789ABCDEF0ULL;
        mem[2047] = 0x0FEDCBA987654321ULL;
        if (mem[0] == 0x123456789ABCDEF0ULL && mem[2047] == 0x0FEDCBA987654321ULL) {
            sys_munmap(mmap_ptr, 16384);
            print("[USERLAND] [TEST 5/5] sys_mmap() & sys_munmap() -> 16KB anon R/W [PASS]\n");
        }
    }

    print("[USERLAND] >>> All Ring 3 User Space tests passed successfully! <<<\n\n");
    sys_exit(0);
}
