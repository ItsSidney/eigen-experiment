/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * display.h - display driver registry.
 *
 * The kernel used to assume exactly one framebuffer, handed over by the
 * firmware via Limine. That works on any machine but it also means the
 * console is stuck with whatever mode the bootloader happened to set, and
 * there is nowhere for an actual GPU driver to hand its scanout to.
 *
 * This layer is the seam: a driver probes its hardware, builds a description
 * of the scanout it owns, and registers it. The console and /dev/fb0 then ask
 * for the active display instead of reaching for a global. It mirrors the
 * split Linux draws between a DRM driver (owns the hardware, sets modes) and
 * the fbdev shim (presents one linear buffer to the rest of the system).
 *
 * Drivers register in probe order and the first one that claims the hardware
 * wins; display_select_fallback() takes whatever the bootloader left us when
 * no driver recognises the card, so a boot is never left without a console.
 */

#ifndef KERNEL_DRIVERS_GPU_DISPLAY_H
#define KERNEL_DRIVERS_GPU_DISPLAY_H

#include <stdint.h>
#include <stddef.h>
#include "include/limine.h"

#define DISPLAY_MAX 8

/* How the scanout is laid out in memory. Everything the console needs to
 * paint a pixel and everything fbdev reports comes from these fields. */
struct display_fb {
    /* Framebuffer description in the shape the rest of the kernel already
     * consumes. address is a kernel (HHDM) pointer, never a physical one. */
    struct limine_framebuffer fb;

    uint64_t phys_base;      /* scanout physical base, for mmap */
    uint64_t size;           /* bytes of scanout the driver will vouch for */

    /* Registers, when the driver has a memory-mapped block. fbdev reports
     * these so a compositor can find the device; 0 when unknown. */
    uint64_t mmio_base_phys;
    uint64_t mmio_len;
};

struct display;

/* Optional modeset entry point. Drivers that can reprogram the display fill
 * this in; the ones that only inherit a firmware mode leave it NULL and the
 * registry never calls it. Returns 0 on success. */
typedef int (*display_set_mode_fn)(struct display *d, uint32_t width,
                                   uint32_t height, uint32_t bpp);

struct display {
    char        name[24];      /* "bochs", "vmware-svga", "i915", ... */
    char        id[16];        /* fb_fix_screeninfo.id, Linux-style */
    struct display_fb mode;

    int         can_modeset;
    display_set_mode_fn set_mode;

    /* Driver-private, untouched by the registry. */
    void       *priv;
};

/* Register a display. The registry copies the struct, so drivers may keep the
 * original on their stack during probe. Returns the slot index, or -1 when
 * the table is full or the description is unusable. */
int  display_register(const struct display *d);

/* Fill in a scanout description. Drivers call this instead of poking the
 * limine_framebuffer fields themselves, so mask/format handling stays in one
 * place and every driver reports the same thing. virt is a kernel (HHDM)
 * pointer to the scanout; phys is the same buffer's physical base, which
 * /dev/fb0 needs for mmap. */
void display_fill_mode(struct display_fb *out, void *virt, uint64_t phys,
                       uint32_t width, uint32_t height, uint32_t pitch,
                       uint32_t bpp);

/* The display the system should paint to: the first driver-registered one, or
 * the bootloader framebuffer when no driver claimed the card. Never NULL once
 * display_select_fallback() has run. */
struct display *display_active(void);

/* Adopt the bootloader framebuffer as a last-resort display. Runs at the same
 * point as fb_init(): the console and the node both need *something* to
 * paint before the probe chain claims its own. Idempotent. */
void display_select_fallback(struct limine_framebuffer *fb);

/* Replace the fallback with the first driver-registered display. Called after
 * the probe chain runs; when no driver claimed anything this is a no-op and
 * the fallback stays active. */
void display_adopt_driver(void);

/* Enumerate what was found; index < display_count(). */
int             display_count(void);
struct display *display_get(int index);

/* Run the driver probe chain. Called once, after the PCI bus is up. */
void display_init(void);

/* True when any driver (not the firmware fallback) owns the scanout. */
int display_driver_owned(void);

#endif /* KERNEL_DRIVERS_GPU_DISPLAY_H */