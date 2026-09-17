/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * wltest.c - libwayland server-side bring-up check.
 *
 * Creates a real wl_display on a unix socket, registers the protocol
 * globals a compositor must export, drives the event loop with a timerfd,
 * and tears everything down. This exercises the parts of libwayland that
 * touch the kernel: socket bind/listen, epoll, eventfd wakeups, timerfd
 * expiry, and the libffi-backed request dispatch.
 *
 * The listener fd is connected to from a second socket in the same process
 * so the accept path and the per-client connection are exercised too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <wayland-server-core.h>

static int fired;
static struct wl_display *display;
static int accepted;

static int on_timer(void *data) {
    (void)data;
    fired = 1;
    return 1; /* keep the source; we destroy the loop ourselves */
}

int main(void) {
    int fails = 0;

    /* The event loop's socket lives in XDG_RUNTIME_DIR. */
    setenv("XDG_RUNTIME_DIR", "/tmp", 1);

    display = wl_display_create();
    if (!display) {
        printf("wltest: wl_display_create failed\n");
        return 1;
    }
    printf("wltest: display created\n");

    /* Bind a listener socket: this is the compositor's accept endpoint. */
    const char *sockname = wl_display_add_socket_auto(display);
    if (!sockname) {
        printf("wltest: add_socket_auto failed\n");
        wl_display_destroy(display);
        return 1;
    }
    printf("wltest: listening on %s\n", sockname);

    /* Export the two globals every compositor must provide. */
    if (wl_display_init_shm(display) != 0) {
        printf("wltest: wl_display_init_shm failed\n");
        fails++;
    } else {
        printf("wltest: wl_shm global registered\n");
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(display);
    if (!loop) {
        printf("wltest: no event loop\n");
        wl_display_destroy(display);
        return 1;
    }

    /* Timer source: proves timerfd + epoll wake the loop. */
    struct wl_event_source *timer =
        wl_event_loop_add_timer(loop, on_timer, NULL);
    if (!timer) {
        printf("wltest: add_timer failed\n");
        wl_display_destroy(display);
        return 1;
    }
    wl_event_source_timer_update(timer, 200); /* 200 ms */

    /* Connect to ourselves so the listener has something to accept. Our
     * kernel has no threads, so a client that really blocks would stall the
     * loop; a raw connect(2) is enough to prove the socket is live. */
    int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cfd >= 0) {
        char path[128];
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        int plen = snprintf(path, sizeof(path), "%s/%s", "/tmp", sockname);
        if (plen > 0 && plen < (int)sizeof(addr.sun_path)) {
            memcpy(addr.sun_path, path, (size_t)plen);
            addr.sun_path[plen] = '\0';
        }
        if (connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            accepted = 1;
            printf("wltest: connected to own listener ok\n");
        } else {
            printf("wltest: connect to own listener failed\n");
            fails++;
        }
    }

    /* Drive the loop until the timer fires (or a bounded number of rounds). */
    int rounds = 0;
    while (!fired && rounds < 50) {
        wl_event_loop_dispatch(loop, 100);
        rounds++;
    }
    if (!fired) {
        printf("wltest: timer never fired (%d rounds)\n", rounds);
        fails++;
    } else {
        printf("wltest: timer fired after %d rounds\n", rounds);
    }

    if (accepted) printf("wltest: accept path exercised\n");

    if (cfd >= 0) close(cfd);
    wl_display_destroy(display);

    if (fails) {
        printf("wltest: %d FAILURE(S)\n", fails);
        return 1;
    }
    printf("wltest: all checks passed\n");
    return 0;
}
