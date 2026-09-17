/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * display.c - display driver registry and probe chain.
 *
 * Boot order matters here. display_init() runs after the PCI bus is
 * enumerated and walks the driver list, letting each driver try to claim the
 * hardware it recognises. The first claim wins: a real driver knows how to
 * set modes and where the device's registers live, which beats the
 * bootloader's static handover.
 *
 * If nothing claims the card the bootloader framebuffer is adopted instead.
 * That is not a failure path - on a machine with an unported GPU it is the
 * only way to get a console - so it is logged as an observation, not an
 * error, and the system boots normally.
 */

#include <stdint.h>
#include <stddef.h>
#include "display.h"
#include "bochs.h"
#include "include/serial.h"
#include "mm/vmm.h"

static struct display g_displays[DISPLAY_MAX];
static int            g_count = 0;

/* Index of the display the console paints to. -1 until something is picked. */
static int            g_active = -1;

/* Index of the fallback slot, -1 when there is none. The active display is a
 * driver only when it differs from this slot. */
static int            g_fallback_slot = -1;

void display_fill_mode(struct display_fb *out, void *virt, uint64_t phys,
                       uint32_t width, uint32_t height, uint32_t pitch,
                       uint32_t bpp) {
    if (!out) return;
    out->fb.address = virt;
    out->fb.width   = width;
    out->fb.height  = height;
    out->fb.pitch   = pitch;
    out->fb.bpp     = (uint16_t)bpp;

    /* Everything we drive is true-colour packed least-significant-bit first;
     * the console uses these masks to place each channel. 32bpp is 8/8/8 with
     * a leading X byte, 16bpp is the usual 5/6/5. */
    out->fb.memory_model = 1; /* DirectColor */
    if (bpp == 16) {
        out->fb.red_mask_size   = 5; out->fb.red_mask_shift   = 11;
        out->fb.green_mask_size = 6; out->fb.green_mask_shift = 5;
        out->fb.blue_mask_size  = 5; out->fb.blue_mask_shift  = 0;
    } else {
        out->fb.red_mask_size   = 8; out->fb.red_mask_shift   = 16;
        out->fb.green_mask_size = 8; out->fb.green_mask_shift = 8;
        out->fb.blue_mask_size  = 8; out->fb.blue_mask_shift  = 0;
    }

    out->phys_base     = phys;
    out->size          = (uint64_t)pitch * height;
    out->mmio_base_phys = 0;
    out->mmio_len       = 0;
}

static int display_usable(const struct display *d) {
    if (!d || !d->mode.fb.address) return 0;
    if (!d->mode.fb.width || !d->mode.fb.height) return 0;
    if (!d->mode.fb.pitch) return 0;
    if (!d->mode.fb.bpp) return 0;
    return 1;
}

int display_register(const struct display *d) {
    if (!display_usable(d)) return -1;
    if (g_count >= DISPLAY_MAX) return -1;

    g_displays[g_count] = *d;
    int slot = g_count++;

    /* First registration becomes active: drivers probe in preference order,
     * so the earliest claim is the most capable one. */
    if (g_active < 0) g_active = slot;

    serial_puts("[GPU] ");
    serial_puts(g_displays[slot].name);
    serial_puts(": ");
    serial_puthex(g_displays[slot].mode.fb.width);
    serial_puts("x");
    serial_puthex(g_displays[slot].mode.fb.height);
    serial_puts("x");
    serial_puthex(g_displays[slot].mode.fb.bpp);
    serial_puts(g_displays[slot].can_modeset ? " modeset+fb" : " fb-only");
    serial_puts("\n");
    return slot;
}

struct display *display_active(void) {
    if (g_active < 0 || g_active >= g_count) return NULL;
    return &g_displays[g_active];
}

void display_select_fallback(struct limine_framebuffer *fb) {
    if (g_fallback_slot >= 0 || !fb) return;

    struct display d;
    for (size_t i = 0; i < sizeof(d); i++) ((uint8_t *)&d)[i] = 0;

    /* Copy the bootloader's description verbatim: it already carries the
     * real masks, and second-guessing them here would be the one place a
     * wrong guess is invisible. */
    d.mode.fb = *fb;
    d.mode.size = (uint64_t)fb->pitch * fb->height;
    if (fb->address) {
        uint64_t hhdm = vmm_get_hhdm_offset();
        d.mode.phys_base = (hhdm && (uint64_t)fb->address > hhdm)
                         ? ((uint64_t)fb->address - hhdm) : 0;
    }
    d.can_modeset = 0;
    d.set_mode = NULL;

    const char *n = "boot-fb";
    for (int i = 0; n[i] && i < 23; i++) d.name[i] = n[i];
    const char *s = "limine-fb";
    for (int i = 0; s[i] && i < 15; i++) d.id[i] = s[i];

    if (display_register(&d) >= 0) {
        g_fallback_slot = g_count - 1;
        /* Active stays on the fallback until display_adopt_driver() runs:
         * nobody with a real scanout has claimed it yet. */
        if (g_active < 0) g_active = g_fallback_slot;
        serial_puts("[GPU] bootloader framebuffer staged\n");
    }
}

void display_adopt_driver(void) {
    /* A driver display exists only if something registered past the
     * fallback. The fallback is always slot g_fallback_slot, and drivers
     * register in probe order, so slot 0 when there was no fallback, or
     * anything that is not the fallback slot when there was one. */
    if (g_fallback_slot < 0) return;            /* fallback never staged */
    if (g_active != g_fallback_slot) return;    /* already driving */
    for (int i = 0; i < g_count; i++) {
        if (i == g_fallback_slot) continue;
        g_active = i;
        serial_puts("[GPU] adopting driver display ");
        serial_puts(g_displays[i].name);
        serial_puts("\n");
        return;
    }
    serial_puts("[GPU] no driver claimed the display; using the "
                "bootloader framebuffer\n");
}

int display_count(void) { return g_count; }

struct display *display_get(int index) {
    if (index < 0 || index >= g_count) return NULL;
    return &g_displays[index];
}

int display_driver_owned(void) {
    return g_active >= 0 && g_fallback_slot >= 0 && g_active != g_fallback_slot;
}

void display_init(void) {
    /* Preference order. A driver returns 1 when it claimed the hardware, 0
     * when the card is not one it knows, and -1 on a real failure; a failure
     * is logged but must not stop the next driver from trying, because a
     * half-initialised device is more common than a truly absent one. */
    static int (*const probe[])(void) = {
        bochs_probe,        /* QEMU stdvga / bochs-display / QXL */
    };

    for (size_t i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
        int r = probe[i]();
        if (r > 0) break; /* claimed; later drivers must not steal it */
        if (r < 0) {
            serial_puts("[GPU] probe error, continuing\n");
        }
    }
}