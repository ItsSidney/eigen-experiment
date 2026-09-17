/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * fbtest.c - /dev/fb0 + evdev smoke test.
 *
 * Opens the framebuffer, mmaps it, draws a test pattern (colour bars,
 * gradient, border), then moves a software cursor with the mouse and quits
 * on any key press or after a fixed timeout. Proves that open, ioctl,
 * mmap and evdev reads all work end-to-end before a compositor is built
 * on top of them.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>

/* fbdev + evdev wire formats, spelled out locally so the build never depends
 * on host kernel headers (musl-gcc falls through to /usr/include otherwise).
 * These match <linux/fb.h> and <linux/input-event-codes.h> exactly. */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

struct fb_bitfield {
    uint32_t offset, length, msb_right;
};

struct fb_fix_screeninfo {
    char     id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type, type_aux, visual;
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

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define REL_X      0x00
#define REL_Y      0x01
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

struct input_event {
    uint64_t sec, usec;
    uint16_t type, code;
    int32_t  value;
};

struct fb_info {
    int      fd;
    void    *map;
    size_t   map_size;
    unsigned width, height, bpp, pitch;
    uint8_t  red_off, green_off, blue_off, red_len, green_len, blue_len;
};

static uint32_t pack_pixel(const struct fb_info *f,
                           uint8_t r, uint8_t g, uint8_t b) {
    uint32_t px = 0;
    if (f->red_len)   px |= ((uint32_t)r >> (8 - f->red_len))   << f->red_off;
    if (f->green_len) px |= ((uint32_t)g >> (8 - f->green_len)) << f->green_off;
    if (f->blue_len)  px |= ((uint32_t)b >> (8 - f->blue_len))  << f->blue_off;
    return px;
}

static void put_px(struct fb_info *f, unsigned x, unsigned y, uint32_t c) {
    if (x >= f->width || y >= f->height) return;
    uint8_t *row = (uint8_t *)f->map + (uint64_t)y * f->pitch;
    memcpy(row + (uint64_t)x * (f->bpp / 8), &c, f->bpp / 8);
}

static void fill_rect(struct fb_info *f, unsigned x0, unsigned y0,
                      unsigned w, unsigned h, uint32_t c) {
    for (unsigned y = y0; y < y0 + h && y < f->height; y++)
        for (unsigned x = x0; x < x0 + w && x < f->width; x++)
            put_px(f, x, y, c);
}

static int fb_open(struct fb_info *f) {
    memset(f, 0, sizeof(*f));
    f->fd = open("/dev/fb0", O_RDWR);
    if (f->fd < 0) { perror("open /dev/fb0"); return -1; }

    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    if (ioctl(f->fd, FBIOGET_FSCREENINFO, &finfo) != 0) {
        perror("FBIOGET_FSCREENINFO"); close(f->fd); return -1;
    }
    if (ioctl(f->fd, FBIOGET_VSCREENINFO, &vinfo) != 0) {
        perror("FBIOGET_VSCREENINFO"); close(f->fd); return -1;
    }

    f->width     = vinfo.xres;
    f->height    = vinfo.yres;
    f->bpp       = vinfo.bits_per_pixel;
    f->pitch     = finfo.line_length;
    f->red_off   = vinfo.red.offset;
    f->green_off = vinfo.green.offset;
    f->blue_off  = vinfo.blue.offset;
    f->red_len   = vinfo.red.length;
    f->green_len = vinfo.green.length;
    f->blue_len  = vinfo.blue.length;

    size_t sz = finfo.smem_len ? (size_t)finfo.smem_len
                               : (size_t)f->pitch * f->height;
    f->map = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, 0);
    if (f->map == MAP_FAILED) {
        perror("mmap /dev/fb0"); close(f->fd); return -1;
    }
    f->map_size = sz;
    return 0;
}

static void fb_close(struct fb_info *f) {
    if (f->map && f->map != MAP_FAILED) munmap(f->map, f->map_size);
    if (f->fd >= 0) close(f->fd);
}

/* Draw the test pattern: colour bars, a gradient and a white border. */
static void draw_pattern(struct fb_info *f) {

    /* Colour bars across the top third. */
    unsigned bar_w = f->width / 8;
    static const uint8_t cols[8][3] = {
        { 255,   0,   0 }, {   0, 255,   0 }, {   0,   0, 255 },
        { 255, 255,   0 }, {   0, 255, 255 }, { 255,   0, 255 },
        { 255, 255, 255 }, { 128, 128, 128 },
    };
    for (int i = 0; i < 8; i++) {
        uint32_t c = pack_pixel(f, cols[i][0], cols[i][1], cols[i][2]);
        fill_rect(f, i * bar_w, 0, bar_w, f->height / 3, c);
    }

    /* Horizontal red->blue gradient in the middle third. */
    unsigned mid_h = f->height / 3;
    for (unsigned x = 0; x < f->width; x++) {
        uint8_t r = (uint8_t)(x * 255 / (f->width ? f->width : 1));
        uint32_t c = pack_pixel(f, r, 0, 255 - r);
        for (unsigned y = f->height / 3;
             y < f->height / 3 + mid_h && y < f->height; y++)
            put_px(f, x, y, c);
    }

    /* White border frame. */
    uint32_t white = pack_pixel(f, 255, 255, 255);
    for (unsigned x = 0; x < f->width; x++) {
        put_px(f, x, 0, white);
        put_px(f, x, f->height - 1, white);
    }
    for (unsigned y = 0; y < f->height; y++) {
        put_px(f, 0, y, white);
        put_px(f, f->width - 1, y, white);
    }
}

/* Cursor: a 12x18 crosshair drawn straight into the mapped buffer. */
static void draw_cursor(struct fb_info *f, int mx, int my, uint32_t c) {
    for (int dy = 0; dy < 18; dy++) {
        for (int dx = 0; dx < 12; dx++) {
            if (dx != 0 && dy != 8 && !(dx < 8 && dy == 0)) continue;
            int px = mx + dx, py = my + dy;
            if (px < 0 || py < 0) continue;
            put_px(f, (unsigned)px, (unsigned)py, c);
        }
    }
}

#define MOUSE_SCALE 4
#define RUN_SECONDS 20

/* Consume pending mouse motion into the cursor, applying a small scale so
 * pointer deltas feel right on high-res modes. Returns after redrawing the
 * cursor at its new position (redraw the pattern behind it first). */
static void move_cursor(struct fb_info *f, int mfd, int *mx, int *my) {
    struct input_event ev;
    for (;;) {
        int r = (int)read(mfd, &ev, sizeof(ev));
        if (r != (int)sizeof(ev)) break;
        if (ev.type == EV_REL && ev.code == REL_X)
            *mx += ev.value * MOUSE_SCALE;
        else if (ev.type == EV_REL && ev.code == REL_Y)
            *my += ev.value * MOUSE_SCALE;
        else if (ev.type == EV_KEY &&
                 (ev.code == BTN_LEFT || ev.code == BTN_RIGHT ||
                  ev.code == BTN_MIDDLE) && ev.value)
            printf("click\n");
    }
    if (*mx < 0) *mx = 0;
    if (*my < 0) *my = 0;
    if (*mx >= (int)f->width)  *mx = (int)f->width - 1;
    if (*my >= (int)f->height) *my = (int)f->height - 1;
}

static int wait_with_timeout(int kfd, int mfd, int seconds) {
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(kfd, &rset);
    FD_SET(mfd, &rset);
    int nfds = kfd > mfd ? kfd : mfd;
    struct timeval tv = { seconds, 0 };
    return select(nfds + 1, &rset, NULL, NULL, &tv);
}

int main(void) {
    struct fb_info f;
    if (fb_open(&f) != 0) return 1;
    printf("fbtest: %ux%u %u bpp pitch %u\n",
           f.width, f.height, f.bpp, f.pitch);

    draw_pattern(&f);
    uint32_t cursor_c = pack_pixel(&f, 255, 255, 0);

    int kfd = open("/dev/input/event0", O_RDONLY);
    int mfd = open("/dev/input/event1", O_RDONLY);
    if (kfd < 0 || mfd < 0) {
        perror("open /dev/input/event0|1");
        fb_close(&f);
        return 1;
    }

    int mx = (int)f.width / 2, my = (int)f.height / 2;
    draw_cursor(&f, mx, my, cursor_c);
    fflush(stdout);

    for (;;) {
        int r = wait_with_timeout(kfd, mfd, RUN_SECONDS);
        if (r < 0) {
            perror("select");
            break;
        }
        if (r == 0) {
            printf("fbtest: timeout, done\n");
            break;
        }
        /* Any key press quits; mouse motion moves the cursor. */
        struct input_event kev;
        if (read(kfd, &kev, sizeof(kev)) == (int)sizeof(kev)) {
            printf("fbtest: key %u=%d, done\n", kev.code, kev.value);
            break;
        }
        draw_pattern(&f);
        move_cursor(&f, mfd, &mx, &my);
        draw_cursor(&f, mx, my, cursor_c);
    }

    /* Leave the console readable again when we hand the screen back. */
    fill_rect(&f, 0, 0, f.width, f.height, pack_pixel(&f, 0, 0, 0));
    fb_close(&f);
    close(kfd);
    close(mfd);
    printf("fbtest: ok\n");
    return 0;
}
