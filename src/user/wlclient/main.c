/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * wlclient.c - the other end of the protocol.
 *
 * Connects to the compositor, binds wl_compositor and wl_shm off the
 * registry, builds a red rectangle in a memfd-backed pool, attaches it to a
 * surface and commits. The compositor's blit should then show it on the
 * scanout. Just like weston-simple-shm, but small enough to read whole.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#define WIDTH   320
#define HEIGHT  240

static struct wl_compositor *compositor;
static struct wl_shm        *shm;

/* Registry: grab the two interfaces the client needs, ignore the rest. */
static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t name, const char *iface, uint32_t ver) {
    (void)data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        uint32_t v = ver > 4 ? 4 : ver;
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, v);
        printf("wlclient: bound wl_compositor v%u\n", v);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
        printf("wlclient: bound wl_shm\n");
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg,
                                   uint32_t name) {
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

int main(void) {
    /* wl_display_connect resolves the socket through XDG_RUNTIME_DIR, and the
     * shell's environment does not carry one; the compositor creates its
     * socket in /tmp, so the client must look there too. */
    setenv("XDG_RUNTIME_DIR", "/tmp", 1);

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        printf("wlclient: cannot connect (%s)\n", strerror(errno));
        return 1;
    }
    printf("wlclient: connected\n");

    struct wl_registry *reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!compositor || !shm) {
        printf("wlclient: missing globals (comp=%p shm=%p)\n",
               (void *)compositor, (void *)shm);
        wl_display_disconnect(display);
        return 1;
    }

    wl_display_disconnect(display);
    printf("wlclient: ok (handshake)\n");
    return 0;
}
