/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * fb.c - /dev/fb0 linear framebuffer device.
 *
 * Exposes the same buffer the kernel console paints: open/read/write work in
 * byte terms (offset = y * pitch + x * bytes_per_pixel), ioctl answers the
 * Linux fixed/variable screeninfo queries compositors probe, and the mmap
 * hook maps the pages straight into the caller, so a Wayland or X
 * compositor can paint without a copy per frame.
 *
 * The mode itself comes from Limine; this driver never reprograms the card
 * (no modesetting), which is exactly the fbdev contract.
 */

#include <stdint.h>
#include <stddef.h>
#include "fb.h"
#include "drivers/gpu/display.h"
#include "drivers/char/console.h"
#include "fs/vfs/vfs.h"
#include "proc/process.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "include/limine.h"
#include "include/serial.h"

/* Linux <linux/fb.h> ioctl numbers, hardcoded so no UAPI headers are needed. */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOGETCMAP         0x4604
#define FBIOPUTCMAP         0x4605
#define FBIOBLANK           0x4611

/* struct fb_fix_screeninfo, packed the way Linux lays it out. */
struct fb_fix_screeninfo {
    char     id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

/* Framebuffer geometry, shared by the read/write and ioctl paths. */
static struct limine_framebuffer *g_fb = NULL;
static struct vfs_node            g_fb_node;
static int                        g_fb_registered = 0;

static uint32_t fb_pitch(void) {
    return g_fb ? (uint32_t)g_fb->pitch : 0;
}

static uint32_t fb_bpp(void) {
    /* console_init already normalised this to a real depth. */
    return (g_fb && g_fb->bpp) ? (uint16_t)g_fb->bpp : 32;
}

static uint64_t fb_size(void) {
    if (!g_fb) return 0;
    return (uint64_t)g_fb->pitch * (uint64_t)g_fb->height;
}

/* Physical base of the buffer, used by the mmap hook. Limine hands us an
 * HHDM-mapped pointer, so phys = virt - hhdm_offset; the page walk is only a
 * fallback in case the address is ever not in HHDM. */
static uint64_t fb_phys_base(void) {
    if (!g_fb) return 0;
    uint64_t hhdm = vmm_get_hhdm_offset();
    if (hhdm && (uint64_t)g_fb->address > hhdm)
        return ((uint64_t)g_fb->address - hhdm) & PAGE_ADDR_MASK;
    uint64_t phys = vmm_virt_to_phys(vmm_get_kernel_address_space(),
                                     (uint64_t)g_fb->address);
    return phys ? (phys & PAGE_ADDR_MASK) : (uint64_t)g_fb->address;
}

static int64_t fb_read(struct vfs_node *node, uint64_t offset, size_t size,
                       void *buf) {
    (void)node;
    if (!g_fb || !buf) return -14; /* -EFAULT */
    uint64_t total = fb_size();
    if (offset >= total) return 0;
    if (offset + size > total) size = (size_t)(total - offset);
    uint8_t *src = (uint8_t *)g_fb->address + offset;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < size; i++) dst[i] = src[i];
    return (int64_t)size;
}

static int64_t fb_write(struct vfs_node *node, uint64_t offset, size_t size,
                        const void *buf) {
    (void)node;
    if (!g_fb || !buf) return -14; /* -EFAULT */
    uint64_t total = fb_size();
    if (offset >= total) return 0;
    if (offset + size > total) size = (size_t)(total - offset);
    uint8_t *dst = (uint8_t *)g_fb->address + offset;
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < size; i++) dst[i] = src[i];
    return (int64_t)size;
}

static int64_t fb_mmap(struct vfs_node *node, uint64_t vaddr, uint64_t size,
                       uint64_t offset, uint64_t pte_flags) {
    (void)node;
    if (!g_fb || size == 0) return -22; /* -EINVAL */
    uint64_t total = fb_size();
    if (offset >= total) return -22;
    if (offset + size > total) size = total - offset;

    uint64_t phys = fb_phys_base() + offset;
    uint64_t pte = pte_flags | PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE;
    /* Framebuffer RAM must not be cached: stale cache lines show up as
     * tearing and as pixels that never appear. */
    pte |= PAGE_PCD | PAGE_PWT;

    pml4_t *pml4 = current_process ? current_process->pml4
                                   : vmm_get_kernel_address_space();
    for (uint64_t o = 0; o < size; o += 0x1000)
        vmm_map_page(pml4, vaddr + o, phys + o, pte);
    return 0;
}

/* struct fb_var_screeninfo, packed the way Linux lays it out. */
struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct { uint32_t offset, length, msb_right; } red, green, blue, transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;
    uint32_t width;
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin;
    uint32_t right_margin;
    uint32_t upper_margin;
    uint32_t lower_margin;
    uint32_t hsync_len;
    uint32_t vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};

int fb_ioctl(unsigned long cmd, uint64_t arg) {
    if (!g_fb) return -19; /* -ENODEV */
    if (!arg) return -14;  /* -EFAULT */

    if (cmd == FBIOGET_FSCREENINFO) {
        struct fb_fix_screeninfo *f = (struct fb_fix_screeninfo *)arg;
        for (int i = 0; i < 16; i++) f->id[i] = 0;
        f->id[0] = 'E'; f->id[1] = 'I'; f->id[2] = 'G'; f->id[3] = 'E';
        f->id[4] = 'N'; f->id[5] = ' '; f->id[6] = 'F'; f->id[7] = 'B';
        f->smem_start  = fb_phys_base();
        f->smem_len    = (uint32_t)fb_size();
        f->type        = 0; /* FB_TYPE_PACKED_PIXELS */
        f->type_aux    = 0;
        f->visual      = 2; /* FB_VISUAL_TRUECOLOR */
        f->xpanstep = 0;
        f->ypanstep = 0;
        f->ywrapstep = 0;
        f->line_length = fb_pitch();
        f->mmio_start  = 0;
        f->mmio_len    = 0;
        f->accel       = 0; /* FB_ACCEL_NONE */
        f->capabilities = 0;
        f->reserved[0] = 0;
        f->reserved[1] = 0;
        return 0;
    }
    if (cmd == FBIOGET_VSCREENINFO || cmd == FBIOPUT_VSCREENINFO) {
        struct fb_var_screeninfo *v = (struct fb_var_screeninfo *)arg;
        if (cmd == FBIOGET_VSCREENINFO) {
            v->xres = (uint32_t)g_fb->width;
            v->yres = (uint32_t)g_fb->height;
            v->xres_virtual = v->xres;
            v->yres_virtual = v->yres;
            v->xoffset = 0;
            v->yoffset = 0;
            v->bits_per_pixel = fb_bpp();
            v->grayscale = 0;
            v->red.offset = (uint32_t)g_fb->red_mask_shift;
            v->red.length = (uint32_t)g_fb->red_mask_size;
            v->green.offset = (uint32_t)g_fb->green_mask_shift;
            v->green.length = (uint32_t)g_fb->green_mask_size;
            v->blue.offset = (uint32_t)g_fb->blue_mask_shift;
            v->blue.length = (uint32_t)g_fb->blue_mask_size;
            v->rotate = 0;
            v->colorspace = 0;
            for (int i = 0; i < 4; i++) v->reserved[i] = 0;
            return 0;
        }
        /* PUT: no modesetting unless the active display offers it. A PUT
         * reprograms the scanout, so the console has to be re-anchored on
         * the new geometry afterwards - otherwise it keeps painting the
         * old mode into a buffer that no longer matches. */
        struct display *ad = display_active();
        if (ad && ad->can_modeset && ad->set_mode &&
            ad->set_mode(ad, v->xres, v->yres, v->bits_per_pixel) == 0) {
            fb_repoint(&ad->mode.fb);
            console_init(&ad->mode.fb, 0xFFFFFF, 0x000000);
            return 0;
        }
        if (v->xres != (uint32_t)g_fb->width ||
            v->yres != (uint32_t)g_fb->height ||
            v->bits_per_pixel != fb_bpp())
            return -22; /* -EINVAL */
        return 0;
    }

    if (cmd == FBIOGETCMAP || cmd == FBIOPUTCMAP || cmd == FBIOBLANK)
        return 0; /* truecolor: no palette to get/set, nothing to blank */

    return -25; /* -ENOTTY */
}

void fb_init(struct limine_framebuffer *fb) {
    if (g_fb_registered) return;
    if (!fb) {
        serial_puts("[FB] no framebuffer response, /dev/fb0 unavailable\n");
        return;
    }
    g_fb = fb;

    g_fb_node.name[0] = 'f';
    g_fb_node.name[1] = 'b';
    g_fb_node.name[2] = '0';
    g_fb_node.name[3] = 0;
    g_fb_node.type = VFS_CHARDEVICE;
    g_fb_node.flags = 0;
    g_fb_node.permissions = 0660;
    g_fb_node.uid = 0;
    g_fb_node.gid = 0;
    g_fb_node.size = fb_size();
    g_fb_node.inode = 29; /* past the statics devfs hands out */
    g_fb_node.device = NULL;
    g_fb_node.read = fb_read;
    g_fb_node.write = fb_write;
    g_fb_node.mmap = fb_mmap;
    g_fb_node.mount_target = NULL;

    if (devfs_attach(&g_fb_node) != 0) {
        serial_puts("[FB] devfs has no room for fb0\n");
        return;
    }
    g_fb_registered = 1;
    serial_puts("[FB] /dev/fb0 ready (");
    serial_puthex((uint64_t)g_fb->width);
    serial_puts("x");
    serial_puthex((uint64_t)g_fb->height);
    serial_puts("x");
    serial_puthex(fb_bpp());
    serial_puts(")\n");
}

void fb_repoint(struct limine_framebuffer *fb) {
    if (!fb || !g_fb_registered) return;
    g_fb = fb;
    g_fb_node.size = fb_size();
    serial_puts("[FB] /dev/fb0 follows display (");
    serial_puthex((uint64_t)g_fb->width);
    serial_puts("x");
    serial_puthex((uint64_t)g_fb->height);
    serial_puts("x");
    serial_puthex(fb_bpp());
    serial_puts(")\n");
}

