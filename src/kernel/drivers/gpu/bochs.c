/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * bochs.c - QEMU display adapters (stdvga, bochs-display, QXL).
 *
 * All three expose the Bochs VBE extensions ("dispi"): a two-port register
 * window at 0x1CE/0x1CF that describes and programs the linear framebuffer.
 * It is the same interface the Bochs BIOS and QEMU's VGA implementation have
 * carried for decades, and it is what the VBE BIOS calls underneath when the
 * firmware sets a mode - which is why we can read back the live mode instead
 * of guessing at it.
 *
 * Two things this driver deliberately does:
 *
 *  - It adopts an already-enabled linear framebuffer rather than forcing a
 *    mode. The firmware has a working scanout by the time we run, and
 *    reprogramming it would blank the screen mid-boot for no reason.
 *  - It asks the device for its limits (the GETCAPS handshake) before
 *    accepting a mode request, so a caller asking for something the adapter
 *    cannot do gets a refusal instead of a corrupted scanout.
 */

#include <stdint.h>
#include <stddef.h>
#include "bochs.h"
#include "display.h"
#include "drivers/pci/pci.h"
#include "include/serial.h"
#include "mm/vmm.h"

/* ── The dispi register window ──────────────────────────────────────────── */

#define DISPI_PORT_INDEX 0x1CE
#define DISPI_PORT_DATA  0x1CF

/* Register indices. The order is fixed by the interface. */
#define DISPI_ID            0x0
#define DISPI_XRES          0x1
#define DISPI_YRES          0x2
#define DISPI_BPP           0x3
#define DISPI_ENABLE        0x4
#define DISPI_BANK          0x5
#define DISPI_VIRT_WIDTH    0x6
#define DISPI_VIRT_HEIGHT   0x7
#define DISPI_X_OFFSET      0x8
#define DISPI_Y_OFFSET      0x9
#define DISPI_VIDEO_MEM_64K 0xA

/* ENABLE bits. */
#define DISPI_DISABLED    0x00
#define DISPI_ENABLED     0x01
#define DISPI_GETCAPS     0x02  /* turns the next reads into a capability query */
#define DISPI_LFB_ENABLED 0x40
#define DISPI_NOCLEARMEM  0x80

/* The interface identifies itself with 0xB0C0..0xB0C5 (version in the low
 * nibble). Anything else at this register is not a Bochs-compatible adapter,
 * and we must not touch it. */
#define DISPI_ID_MASK 0xFFF0
#define DISPI_ID_BASE 0xB0C0

static void dispi_write(uint16_t index, uint16_t value) {
    outw(DISPI_PORT_INDEX, index);
    outw(DISPI_PORT_DATA, value);
}

static uint16_t dispi_read(uint16_t index) {
    outw(DISPI_PORT_INDEX, index);
    return inw(DISPI_PORT_DATA);
}

/* ── Driver state ───────────────────────────────────────────────────────── */

struct bochs_dev {
    struct pci_device *pci;
    uint64_t           lfb_phys;
    uint64_t           vram_size;

    /* Limits reported by GETCAPS. Zero until queried. */
    uint32_t           max_xres, max_yres, max_bpp;

    uint8_t            lfb_is_64bit;
};
static struct bochs_dev g_bochs;

/* Pitch comes from the virtual width, not the visible width: with panning
 * enabled those differ, and using the visible width would shear the image. */
static uint32_t bochs_pitch(uint32_t virt_width, uint32_t bpp) {
    return virt_width * ((bpp + 7) / 8);
}

/* ── Capability query ───────────────────────────────────────────────────── *
 *
 * The adapter overloads the mode registers: setting GETCAPS in ENABLE makes
 * the next XRES/YRES/BPP reads return the maximums instead of the current
 * mode. The enable state has to be restored afterwards or the display stops
 * scanning out, so the previous value is saved and put back. */
static void bochs_query_caps(void) {
    uint16_t saved = dispi_read(DISPI_ENABLE);

    dispi_write(DISPI_ENABLE, (uint16_t)(saved | DISPI_GETCAPS));
    g_bochs.max_xres = dispi_read(DISPI_XRES);
    g_bochs.max_yres = dispi_read(DISPI_YRES);
    g_bochs.max_bpp  = dispi_read(DISPI_BPP);
    dispi_write(DISPI_ENABLE, saved);

    /* A device that does not implement GETCAPS answers with the current mode
     * rather than the maximum. Treating that as the limit would be wrong in
     * the restrictive direction, so fall back to the VBE 2.0 floor instead. */
    if (g_bochs.max_xres < 640) g_bochs.max_xres = 640;
    if (g_bochs.max_yres < 480) g_bochs.max_yres = 480;
    if (g_bochs.max_bpp  < 16)  g_bochs.max_bpp  = 32;
}

/* ── Mode setting ───────────────────────────────────────────────────────── *
 *
 * The documented sequence. The order matters: a resolution write while the
 * display is enabled is ignored by some implementations, and clearing the
 * offsets keeps a previously-enabled panning window from cropping the new
 * mode. NOCLEARMEM is set so the screen does not flash black while we work -
 * the console repaints it anyway. */
static int bochs_set_mode(struct display *d, uint32_t width, uint32_t height,
                          uint32_t bpp) {
    if (!d) return -1;
    if (g_bochs.max_xres && (width > g_bochs.max_xres ||
                             height > g_bochs.max_yres ||
                             bpp > g_bochs.max_bpp))
        return -1;

    dispi_write(DISPI_ENABLE, DISPI_DISABLED);
    dispi_write(DISPI_XRES, (uint16_t)width);
    dispi_write(DISPI_YRES, (uint16_t)height);
    dispi_write(DISPI_BPP, (uint16_t)bpp);
    /* Single virtual screen: no panning, so stride == visible width. */
    dispi_write(DISPI_VIRT_WIDTH, (uint16_t)width);
    dispi_write(DISPI_X_OFFSET, 0);
    dispi_write(DISPI_Y_OFFSET, 0);
    dispi_write(DISPI_ENABLE,
                (uint16_t)(DISPI_ENABLED | DISPI_LFB_ENABLED | DISPI_NOCLEARMEM));

    /* Read back what the device actually accepted. An adapter with a smaller
     * VRAM than the request clamps silently, and reporting our request rather
     * than its answer would hand the console a pitch that does not match the
     * hardware. */
    uint32_t got_x = dispi_read(DISPI_XRES);
    uint32_t got_y = dispi_read(DISPI_YRES);
    uint32_t got_b = dispi_read(DISPI_BPP);
    uint32_t virt  = dispi_read(DISPI_VIRT_WIDTH);
    if (!virt) virt = got_x;

    if (got_x != width || got_y != height || got_b != bpp) {
        serial_puts("[GPU] bochs: adapter clamped mode to ");
        serial_puthex(got_x); serial_puts("x");
        serial_puthex(got_y); serial_puts("x");
        serial_puthex(got_b); serial_puts("\n");
    }

    uint64_t hhdm = vmm_get_hhdm_offset();
    display_fill_mode(&d->mode, (void *)(g_bochs.lfb_phys + hhdm),
                      g_bochs.lfb_phys, got_x, got_y,
                      bochs_pitch(virt, got_b), got_b);
    d->mode.mmio_base_phys = g_bochs.lfb_phys;
    d->mode.mmio_len       = g_bochs.vram_size;
    return 0;
}

/* ── Probe ─────────────────────────────────────────────────────────────── */

/* QEMU's display adapters. The dispi ID check below is what actually gates
 * this driver, but looking up the PCI device first means we never poke ports
 * at a machine that has no QEMU hardware at all. */
static struct pci_device *bochs_find_pci(void) {
    static const struct { uint16_t vendor, device; } ids[] = {
        { 0x1234, 0x1111 },  /* QEMU stdvga, and bochs-display */
        { 0x1B36, 0x0100 },  /* QXL (Red Hat) */
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        struct pci_device *d = pci_find_device(ids[i].vendor, ids[i].device);
        if (d) return d;
    }
    /* Fall back to any VGA-compatible display controller; the dispi check
     * will reject it if the interface is not there. */
    return pci_find_class(0x03, 0x00);
}

/* BAR0 holds the video memory. Prefetchable, and the low four bits are
 * attribute flags rather than address. */
static uint64_t bochs_bar0(struct pci_device *d, uint8_t *is_64bit) {
    *is_64bit = d->bars[0].is_64bit;
    return d->bars[0].base & ~0xFULL;
}

int bochs_probe(void) {
    struct pci_device *pci = bochs_find_pci();
    if (!pci) return 0;

    /* Authoritative interface check before touching anything else. */
    uint16_t id = dispi_read(DISPI_ID);
    if ((id & DISPI_ID_MASK) != DISPI_ID_BASE) return 0;

    serial_puts("[GPU] bochs dispi id ");
    serial_puthex(id);
    serial_puts(" on pci ");
    serial_puthex(pci->vendor_id); serial_puts(":");
    serial_puthex(pci->device_id); serial_puts("\n");

    /* Decoding has to be on before the aperture answers. Bus mastering is not
     * needed - the adapter never initiates transfers itself. */
    pci_enable_mmio(pci);

    uint8_t is64 = 0;
    uint64_t lfb = bochs_bar0(pci, &is64);
    if (!lfb) return -1;

    uint64_t vram = pci->bars[0].size ? pci->bars[0].size : (16ULL << 20);
    g_bochs.pci          = pci;
    g_bochs.lfb_phys     = lfb;
    g_bochs.vram_size    = vram;
    g_bochs.lfb_is_64bit = is64;
    bochs_query_caps();

    serial_puts("[GPU] bochs: lfb ");
    serial_puthex(lfb);
    serial_puts(" vram ");
    serial_puthex(vram);
    serial_puts(" max ");
    serial_puthex(g_bochs.max_xres); serial_puts("x");
    serial_puthex(g_bochs.max_yres); serial_puts("x");
    serial_puthex(g_bochs.max_bpp);  serial_puts("\n");

    struct display d;
    for (size_t i = 0; i < sizeof(d); i++) ((uint8_t *)&d)[i] = 0;
    const char *nm = "qemu-bochs";
    for (int i = 0; nm[i] && i < 23; i++) d.name[i] = nm[i];
    const char *sid = (pci->vendor_id == 0x1B36) ? "qxl" : "bochs-drm";
    for (int i = 0; sid[i] && i < 15; i++) d.id[i] = sid[i];

    d.can_modeset = 1;
    d.set_mode    = bochs_set_mode;

    /* Adopt the live mode when the firmware already left us a linear
     * framebuffer. Anything else (VGA text, or LFB switched off) means there
     * is no scanout we can paint to, so one has to be programmed. */
    uint16_t en = dispi_read(DISPI_ENABLE);
    if ((en & DISPI_ENABLED) && (en & DISPI_LFB_ENABLED)) {
        uint32_t x  = dispi_read(DISPI_XRES);
        uint32_t y  = dispi_read(DISPI_YRES);
        uint32_t b  = dispi_read(DISPI_BPP);
        uint32_t vw = dispi_read(DISPI_VIRT_WIDTH);
        if (!vw) vw = x;

        if (x && y && (b == 16 || b == 24 || b == 32)) {
            uint64_t hhdm = vmm_get_hhdm_offset();
            display_fill_mode(&d.mode, (void *)(lfb + hhdm), lfb, x, y,
                              bochs_pitch(vw, b), b);
            d.mode.mmio_base_phys = lfb;
            d.mode.mmio_len       = vram;
            serial_puts("[GPU] bochs: adopted live mode ");
            serial_puthex(x); serial_puts("x");
            serial_puthex(y); serial_puts("x");
            serial_puthex(b); serial_puts("\n");
            if (display_register(&d) < 0) return -1;
            return 1;
        }
    }

    /* No usable framebuffer yet: pick the largest standard mode the adapter
     * will take. 1280x800 keeps the console at a comfortable grid and fits
     * comfortably inside every QEMU stdvga build's VRAM. */
    static const struct { uint32_t w, h, bpp; } prefer[] = {
        { 1280, 800, 32 }, { 1024, 768, 32 }, { 800, 600, 32 }, { 640, 480, 32 },
    };
    for (size_t i = 0; i < sizeof(prefer) / sizeof(prefer[0]); i++) {
        if (prefer[i].w > g_bochs.max_xres || prefer[i].h > g_bochs.max_yres)
            continue;
        if (bochs_set_mode(&d, prefer[i].w, prefer[i].h, prefer[i].bpp) == 0) {
            serial_puts("[GPU] bochs: programmed mode\n");
            if (display_register(&d) < 0) return -1;
            return 1;
        }
    }

    serial_puts("[GPU] bochs: no mode could be established\n");
    return -1;
}

