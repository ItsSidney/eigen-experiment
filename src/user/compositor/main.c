/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * compositor.c - Eigen's Wayland compositor.
 *
 * Owns /dev/fb0, exports wl_compositor + wl_shm, and blits committed surface
 * buffers into the scanout with a stride-aware row copy. Because a separate
 * client process cannot cooperate with a non-preemptive kernel's event loop,
 * a built-in test surface (memfd + mmap, moved by the evdev mouse through
 * wl_event_loop_add_fd) exercises the exact buffer and input path a real
 * client will use.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

/* memfd_create is declared in <sys/mman.h> only under _GNU_SOURCE, and the
 * declaration drifts between libcs. Declaring it here keeps the build
 * self-contained; the kernel implements it (shmem objects). */
int memfd_create(const char *name, unsigned int flags);

/* evdev / fbdev wire formats, spelled out so no host kernel headers are
 * pulled in (toolchain/musl/include/linux is a symlink to the host's). */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define REL_X  0x00
#define REL_Y  0x01

struct fb_bitfield { uint32_t offset, length, msb_right; };

struct fb_fix_screeninfo {
    char     id[16];
    uint64_t smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset, bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags, pixclock;
    uint32_t left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};

struct input_event {
    uint64_t sec, usec;
    uint16_t type, code;
    int32_t  value;
};

/* Cursor feedback: the kernel hands raw mouse packets to us but draws no
 * pointer itself, so the compositor owns the visible cursor. The crosshair
 * lives one layer above the test box (redrawn on every present) and gives
 * immediate proof that mouse packets are arriving even when the box is not
 * under the finger yet. */
static int32_t g_cursor_x = 640, g_cursor_y = 400;
static int      g_kbd_seen = 0, g_mouse_seen = 0;

/* ── state ─────────────────────────────────────────────────────────────── */

struct fb_state {
    int      fd;
    void    *map;
    size_t   size;
    uint32_t width, height, bpp, pitch;
};
static struct fb_state g_fb;

struct surface {
    struct wl_resource *resource;
    struct wl_resource *pending;
    int32_t             x, y;
};
static struct surface g_surfaces[16];

struct test_surface {
    int      fd;
    uint8_t *data;
    size_t   size;
    uint32_t w, h, stride;
    int32_t  x, y;
    struct wl_event_source *kbd, *mse;
};
static struct test_surface g_ts;

static struct wl_display *g_display;

/* ── framebuffer ───────────────────────────────────────────────────────── */

static int fb_open(void) {
    g_fb.fd = open("/dev/fb0", O_RDWR);
    if (g_fb.fd < 0) { perror("compositor: open /dev/fb0"); return -1; }

    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    if (ioctl(g_fb.fd, FBIOGET_FSCREENINFO, &finfo) != 0 ||
        ioctl(g_fb.fd, FBIOGET_VSCREENINFO, &vinfo) != 0) {
        perror("compositor: fb ioctl");
        close(g_fb.fd);
        return -1;
    }
    g_fb.width  = vinfo.xres;
    g_fb.height = vinfo.yres;
    g_fb.bpp    = vinfo.bits_per_pixel;
    g_fb.pitch  = finfo.line_length;
    g_fb.size   = finfo.smem_len ? finfo.smem_len
                                 : (size_t)g_fb.pitch * g_fb.height;
    g_fb.map = mmap(NULL, g_fb.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    g_fb.fd, 0);
    if (g_fb.map == MAP_FAILED) {
        perror("compositor: mmap /dev/fb0");
        close(g_fb.fd);
        return -1;
    }
    return 0;
}

/* Plot one pixel, clipped to the scanout. Colour is 0x00RRGGBB and lands in
 * memory as the usual little-endian B,G,R(,A) order. Used for the cursor and
 * the input activity ticks; surface blits write whole rows instead. */
static void ts_px(int32_t x, int32_t y, uint32_t colour) {
    if (!g_fb.map || g_fb.map == MAP_FAILED) return;
    if (x < 0 || y < 0 || x >= (int32_t)g_fb.width || y >= (int32_t)g_fb.height)
        return;
    uint8_t *p = (uint8_t *)g_fb.map + (size_t)y * g_fb.pitch +
                 (size_t)x * (g_fb.bpp / 8);
    p[0] = (uint8_t)(colour & 0xFF);
    p[1] = (uint8_t)((colour >> 8) & 0xFF);
    p[2] = (uint8_t)((colour >> 16) & 0xFF);
    if (g_fb.bpp == 32) p[3] = 0xFF;
}


static void fb_fill(uint32_t colour) {
    for (uint32_t y = 0; y < g_fb.height; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)g_fb.map +
                                     (size_t)y * g_fb.pitch);
        for (uint32_t x = 0; x < g_fb.width; x++) row[x] = colour;
    }
}

/* Blit a wl_shm buffer into the scanout. The client's stride is authoritative
 * (rows may be padded); XRGB8888 and the fb0 x8r8g8b8 layout coincide, so the
 * copy is byte-for-byte. */
static void fb_blit_shm(struct wl_resource *buffer, int32_t dx, int32_t dy) {
    struct wl_shm_buffer *shm = wl_shm_buffer_get(buffer);
    if (!shm) return;

    int32_t bw = wl_shm_buffer_get_width(shm);
    int32_t bh = wl_shm_buffer_get_height(shm);
    int32_t stride = wl_shm_buffer_get_stride(shm);

    wl_shm_buffer_begin_access(shm);
    const uint8_t *src = (const uint8_t *)wl_shm_buffer_get_data(shm);

    for (int32_t y = 0; y < bh; y++) {
        int32_t ty = dy + y;
        if (ty < 0 || ty >= (int32_t)g_fb.height) continue;
        const uint8_t *srow = src + (size_t)y * stride;
        uint8_t *drow = (uint8_t *)g_fb.map + (size_t)ty * g_fb.pitch;
        for (int32_t x = 0; x < bw; x++) {
            int32_t tx = dx + x;
            if (tx < 0 || tx >= (int32_t)g_fb.width) continue;
            const uint8_t *s = srow + (size_t)x * 4;
            uint8_t *d = drow + (size_t)tx * (g_fb.bpp / 8);
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
    wl_shm_buffer_end_access(shm);
}

/* Blit the in-process test surface (a plain mapped buffer, no wl_resource).
 * Every present redraws three layers: test box, activity ticks, cursor.
 * The ticks (white = keyboard packet arrived, green = mouse packet arrived)
 * and the white crosshair prove input is reaching us even when the box has
 * not moved yet; the console also writes to this scanout, so drawing the
 * cursor last keeps it above stray text. */
static void ts_present(void) {
    uint32_t x, y;
    int32_t d;

    for (y = 0; y < g_ts.h; y++) {
        int32_t ty = g_ts.y + (int32_t)y;
        if (ty < 0 || ty >= (int32_t)g_fb.height) continue;
        const uint8_t *srow = g_ts.data + (size_t)y * g_ts.stride;
        uint8_t *drow = (uint8_t *)g_fb.map + (size_t)ty * g_fb.pitch;
        for (x = 0; x < g_ts.w; x++) {
            int32_t tx = g_ts.x + (int32_t)x;
            if (tx < 0 || tx >= (int32_t)g_fb.width) continue;
            const uint8_t *s = srow + x * 4;
            uint8_t *d = drow + (size_t)tx * (g_fb.bpp / 8);
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            if (g_kbd_seen)
                ts_px(8 + (int32_t)x, (int32_t)g_fb.height - 16 + (int32_t)y,
                      0x00FFFFFFu);
            if (g_mouse_seen)
                ts_px(24 + (int32_t)x, (int32_t)g_fb.height - 16 + (int32_t)y,
                      0x0000FF00u);
        }
    for (d = -9; d <= 9; d++) {
        ts_px(g_cursor_x, g_cursor_y + d, 0x00FFFFFFu);
        ts_px(g_cursor_x + d, g_cursor_y, 0x00FFFFFFu);
    }
}

/* ── wl_surface / wl_compositor ────────────────────────────────────────── */

static void surface_present(struct surface *s) {
    if (!s || !s->pending) return;
    fb_blit_shm(s->pending, s->x >> 8, s->y >> 8); /* wl_fixed -> pixels */
    wl_buffer_send_release(s->pending);
    s->pending = NULL;
}

static void surf_destroy(struct wl_client *client, struct wl_resource *r) {
    (void)client; wl_resource_destroy(r);
}

static void surf_attach(struct wl_client *client, struct wl_resource *r,
                        struct wl_resource *buffer, int32_t x, int32_t y) {
    (void)client;
    struct surface *s = wl_resource_get_user_data(r);
    if (!s) return;
    s->pending = buffer;
    s->x = x;
    s->y = y;
}

static void surf_damage(struct wl_client *client, struct wl_resource *r,
                        int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)client; (void)r; (void)x; (void)y; (void)w; (void)h;
}

static void surf_frame(struct wl_client *client, struct wl_resource *r,
                       uint32_t callback) {
    struct wl_resource *cb = wl_resource_create(client,
                                                &wl_callback_interface, 1,
                                                callback);
    if (!cb) { wl_client_post_no_memory(client); return; }
    wl_callback_send_done(cb, 0);
    wl_resource_destroy(cb);
    (void)r;
}

static void surf_commit(struct wl_client *client, struct wl_resource *r) {
    (void)client;
    surface_present(wl_resource_get_user_data(r));
}

static void surf_region(struct wl_client *client, struct wl_resource *r,
                        struct wl_resource *region) {
    (void)client; (void)r; (void)region;
}

static void surf_damage_buffer(struct wl_client *client, struct wl_resource *r,
                               int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)client; (void)r; (void)x; (void)y; (void)w; (void)h;
}

static void surf_set_txform(struct wl_client *client, struct wl_resource *r,
                            int32_t t) { (void)client; (void)r; (void)t; }

static void surf_set_scale(struct wl_client *client, struct wl_resource *r,
                           int32_t sc) { (void)client; (void)r; (void)sc; }

static const struct wl_surface_interface surface_impl = {
    .destroy              = surf_destroy,
    .attach               = surf_attach,
    .damage               = surf_damage,
    .frame                = surf_frame,
    .set_opaque_region    = surf_region,
    .set_input_region     = surf_region,
    .commit               = surf_commit,
    .set_buffer_transform = surf_set_txform,
    .set_buffer_scale     = surf_set_scale,
    .damage_buffer        = surf_damage_buffer,
};

static void compositor_create_surface(struct wl_client *client,
                                      struct wl_resource *r, uint32_t id) {
    struct wl_resource *sr = wl_resource_create(client,
                                                &wl_surface_interface, 4, id);
    if (!sr) { wl_client_post_no_memory(client); return; }
    struct surface *s = NULL;
    for (int i = 0; i < 16; i++) {
        if (!g_surfaces[i].resource) { s = &g_surfaces[i]; break; }
    }
    if (!s) {
        wl_resource_destroy(sr);
        wl_client_post_no_memory(client);
        return;
    }
    s->resource = sr;
    s->pending  = NULL;
    s->x = s->y = 0;
    wl_resource_set_implementation(sr, &surface_impl, s, NULL);
    (void)r;
}

static void compositor_create_region(struct wl_client *client,
                                     struct wl_resource *r, uint32_t id) {
    struct wl_resource *rr = wl_resource_create(client, &wl_region_interface,
                                                1, id);
    if (!rr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(rr, NULL, NULL, NULL);
    (void)r;
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region  = compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id) {
    if (version > 4) version = 4;
    printf("compositor: client bound wl_compositor v%u\n", version);
    fflush(stdout);
    struct wl_resource *r = wl_resource_create(client,
                                               &wl_compositor_interface,
                                               version, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &compositor_impl, data, NULL);
}
/* ── test surface: memfd + evdev through the event loop ─────────────────── */

static void ts_fill(void) {
    for (uint32_t y = 0; y < g_ts.h; y++) {
        uint8_t *row = g_ts.data + (size_t)y * g_ts.stride;
        for (uint32_t x = 0; x < g_ts.w; x++) {
            uint32_t *px = (uint32_t *)(row + x * 4);
            if (y < 4 || x < 4 || y >= g_ts.h - 4 || x >= g_ts.w - 4)
                *px = 0x00FFFFFF;
            else
                *px = 0xFF000000 | ((y * 255 / g_ts.h) << 16) |
                      (x * 255 / g_ts.w);
        }
    }
}

static int on_mouse(int fd, uint32_t mask, void *data) {
    (void)mask; (void)data;
    struct input_event ev;
    int moved = 0;
    for (;;) {
        int r = (int)read(fd, &ev, sizeof(ev));
        if (r != (int)sizeof(ev)) break;
        if (ev.type == EV_REL && ev.code == REL_X) {
            g_ts.x += ev.value * 4;
            g_cursor_x += ev.value * 4;
            moved = 1;
        } else if (ev.type == EV_REL && ev.code == REL_Y) {
            g_ts.y += ev.value * 4;
            g_cursor_y += ev.value * 4;
            moved = 1;
        }
    }
    if (moved)
        g_mouse_seen = 1;
    if (g_ts.x < 0) g_ts.x = 0;
    if (g_ts.y < 0) g_ts.y = 0;
    if (g_ts.x + (int32_t)g_ts.w > (int32_t)g_fb.width)
        g_ts.x = (int32_t)g_fb.width - (int32_t)g_ts.w;
    if (g_ts.y + (int32_t)g_ts.h > (int32_t)g_fb.height)
        g_ts.y = (int32_t)g_fb.height - (int32_t)g_ts.h;
    if (g_cursor_x < 0) g_cursor_x = 0;
    if (g_cursor_y < 0) g_cursor_y = 0;
    if (g_cursor_x >= (int32_t)g_fb.width)
        g_cursor_x = (int32_t)g_fb.width - 1;
    if (g_cursor_y >= (int32_t)g_fb.height)
        g_cursor_y = (int32_t)g_fb.height - 1;
    if (moved) {
        fb_fill(0x00202020);
        ts_present();
    }
    return 0;
}

static int on_keyboard(int fd, uint32_t mask, void *data) {
    (void)mask; (void)data;
    struct input_event ev;
    int pressed = 0;
    while (read(fd, &ev, sizeof(ev)) == (int)sizeof(ev)) {
        if (ev.type == EV_KEY && (ev.value == 1 || ev.value == 2)) {
            printf("compositor: key %u, quitting\n", ev.code);
            fflush(stdout);
            wl_display_terminate(g_display);
            return 0;
        }
        pressed = 1;
    }
    /* Key events that did not terminate still light the activity tick, which
     * distinguishes "kernel delivers keys" from "kernel delivers nothing". */
    if (pressed) {
        g_kbd_seen = 1;
        ts_present();
    }
    return 0;
}

static int ts_init(struct wl_event_loop *loop) {
    memset(&g_ts, 0, sizeof(g_ts));
    g_ts.w = 320;
    g_ts.h = 240;
    g_ts.stride = g_ts.w * 4;
    g_ts.x = 100;
    g_ts.y = 100;

    g_ts.fd = memfd_create("eigen-test-surface", 0);
    if (g_ts.fd < 0) { perror("compositor: memfd_create"); return -1; }
    g_ts.size = (size_t)g_ts.stride * g_ts.h;
    if (ftruncate(g_ts.fd, (off_t)g_ts.size) != 0) {
        perror("compositor: ftruncate");
        close(g_ts.fd);
        return -1;
    }
    g_ts.data = mmap(NULL, g_ts.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     g_ts.fd, 0);
    if (g_ts.data == MAP_FAILED) {
        perror("compositor: mmap pool");
        close(g_ts.fd);
        return -1;
    }
    ts_fill();

    int kfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    int mfd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (kfd < 0 || mfd < 0) {
        perror("compositor: open evdev");
        if (kfd >= 0) close(kfd);
        if (mfd >= 0) close(mfd);
        return -1;
    }
    g_ts.kbd = wl_event_loop_add_fd(loop, kfd, WL_EVENT_READABLE,
                                    on_keyboard, NULL);
    g_ts.mse = wl_event_loop_add_fd(loop, mfd, WL_EVENT_READABLE,
                                    on_mouse, NULL);
    if (!g_ts.kbd || !g_ts.mse) {
        printf("compositor: cannot add evdev sources\n");
        return -1;
    }
    return 0;
}

static void ts_cleanup(void) {
    if (g_ts.data && g_ts.data != MAP_FAILED) munmap(g_ts.data, g_ts.size);
    if (g_ts.fd >= 0) close(g_ts.fd);
    memset(&g_ts, 0, sizeof(g_ts));
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(void) {
    if (fb_open() != 0) return 1;
    printf("compositor: /dev/fb0 %ux%u %u bpp pitch %u\n",
           g_fb.width, g_fb.height, g_fb.bpp, g_fb.pitch);

    fb_fill(0x00202020);
    setenv("XDG_RUNTIME_DIR", "/tmp", 1);

    g_display = wl_display_create();
    if (!g_display) {
        printf("compositor: cannot create display\n");
        return 1;
    }

    const char *socket = wl_display_add_socket_auto(g_display);
    if (!socket) {
        printf("compositor: cannot create socket\n");
        wl_display_destroy(g_display);
        return 1;
    }

    if (wl_display_init_shm(g_display) != 0) {
        printf("compositor: cannot init shm\n");
        wl_display_destroy(g_display);
        return 1;
    }

    if (!wl_global_create(g_display, &wl_compositor_interface, 4,
                          NULL, compositor_bind)) {
        printf("compositor: cannot export wl_compositor\n");
        wl_display_destroy(g_display);
        return 1;
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(g_display);
    if (ts_init(loop) != 0)
        printf("compositor: test surface init failed\n");
    else
        ts_present();

    printf("compositor: ready on %s (mouse moves the box, any key quits)\n",
           socket);
    fflush(stdout);

    wl_display_run(g_display);

    ts_cleanup();
    wl_display_destroy(g_display);
    if (g_fb.map && g_fb.map != MAP_FAILED) {
        munmap(g_fb.map, g_fb.size);
        close(g_fb.fd);
    }
    printf("compositor: exited\n");
    return 0;
}