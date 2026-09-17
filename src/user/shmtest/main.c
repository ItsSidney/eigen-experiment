/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * shmtest.c - memfd + shared mmap + epoll smoke test.
 *
 * What Wayland needs in one binary:
 *   1. memfd_create + ftruncate    -> an anonymous buffer pool object
 *   2. mmap it TWICE               -> two mappings must hit the same frames,
 *                                     otherwise the compositor cannot see the
 *                                     client's pixels
 *   3. epoll_create1 + ctl + wait  -> an event loop that wakes on input
 *
 * Prints one line per check so a failure names itself.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>

/* Syscall numbers (x86_64), spelled out to avoid header drift. */
#define SYS_MEMFD_CREATE 319
#define SYS_EPOLL_CREATE1 291
#define SYS_EPOLL_CTL 233
#define SYS_EPOLL_WAIT 232
#define SYS_FTRUNCATE 77

#define EPOLLIN  0x001u
#define EPOLL_CTL_ADD 1

struct epoll_event_k {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

static long memfd_create(const char *name, unsigned flags) {
    return syscall(SYS_MEMFD_CREATE, name, flags);
}
static long epoll_create1(int flags) { return syscall(SYS_EPOLL_CREATE1, flags); }
static long epoll_ctl(int epfd, int op, int fd, struct epoll_event_k *ev) {
    return syscall(SYS_EPOLL_CTL, epfd, op, fd, ev);
}
static long epoll_wait(int epfd, struct epoll_event_k *ev, int max, int tmo) {
    return syscall(SYS_EPOLL_WAIT, epfd, ev, max, tmo);
}

#define POOL_BYTES 8192

int main(void) {
    int fails = 0;

    /* 1. Anonymous shared object. */
    int fd = (int)memfd_create("wp-pool", 0);
    if (fd < 0) {
        printf("shmtest: memfd_create failed (%d)\n", fd);
        return 1;
    }
    printf("shmtest: memfd fd=%d\n", fd);

    int tr = (int)syscall(SYS_FTRUNCATE, fd, POOL_BYTES);
    if (tr != 0) {
        printf("shmtest: ftruncate failed (%d)\n", tr);
        return 1;
    }
    printf("shmtest: ftruncate %d bytes ok\n", POOL_BYTES);

    /* 2. Two independent mappings must alias the same physical frames. */
    void *a = mmap(NULL, POOL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    void *b = mmap(NULL, POOL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (a == MAP_FAILED || b == MAP_FAILED) {
        printf("shmtest: mmap failed (a=%p b=%p)\n", a, b);
        return 1;
    }
    printf("shmtest: maps a=%p b=%p\n", a, b);

    const char *msg = "wayland-pool";
    memcpy(a, msg, strlen(msg) + 1);
    if (strcmp((char *)b, msg) == 0) {
        printf("shmtest: SHARED mapping ok (write via a visible at b)\n");
    } else {
        printf("shmtest: sharing BROKEN (b reads '%s')\n", (char *)b);
        fails++;
    }

    /* Also verify the fd path sees the same bytes: read() must return what
     * the mapping wrote, which proves the object is the single source. */
    uint8_t chk[16];
    memset(chk, 0, sizeof(chk));
    lseek(fd, 0, SEEK_SET);
    int n = (int)read(fd, chk, sizeof(chk) - 1);
    if (n > 0 && strcmp((char *)chk, msg) == 0) {
        printf("shmtest: read(fd) sees mapped data ok\n");
    } else {
        printf("shmtest: read(fd) mismatch ('%s')\n", (char *)chk);
        fails++;
    }

    /* 3. epoll loop: watch stdin, then prove a timeout and a wake both work. */
    int ep = (int)epoll_create1(0);
    if (ep < 0) {
        printf("shmtest: epoll_create1 failed (%d)\n", ep);
        return 1;
    }
    struct epoll_event_k ev;
    ev.events = EPOLLIN;
    ev.data   = 0x1234;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, 0, &ev) != 0) {
        printf("shmtest: epoll_ctl failed\n");
        return 1;
    }
    printf("shmtest: epoll fd=%d watching stdin\n", ep);

    struct epoll_event_k out[4];
    long r = epoll_wait(ep, out, 4, 3000);
    if (r == 0)
        printf("shmtest: epoll_wait timeout ok (no input)\n");
    else if (r > 0 && (out[0].events & EPOLLIN) && out[0].data == 0x1234)
        printf("shmtest: epoll_wait woke on stdin, data=%llx ok\n",
               (unsigned long long)out[0].data);
    else
        printf("shmtest: epoll_wait unexpected (%ld)\n", r);

    munmap(a, POOL_BYTES);
    munmap(b, POOL_BYTES);
    close(ep);
    close(fd);

    if (fails) { printf("shmtest: %d FAILURE(S)\n", fails); return 1; }
    printf("shmtest: all checks passed\n");
    return 0;
}
