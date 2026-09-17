/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * mouse.c - PS/2 wheel mouse (IMPS/2) on the i8042 auxiliary port, IRQ12.
 *
 * The aux port sits behind the same controller the keyboard driver owns, so
 * init mirrors keyboard_init(): enable the port, drain stale bytes, reset the
 * device, run the 200/100/80 samples-per-second handshake that switches a
 * wheel mouse into 4-byte IMPS/2 packets, then stream. The IRQ12 handler
 * decodes 3- or 4-byte packets (9-bit sign-extended deltas), keeps a clamped
 * cursor for the test overlay, and mirrors motion/buttons into evdev.
 */

#include <stdint.h>
#include "mouse.h"
#include "dev/input/evdev.h"
#include "arch/x86_64/cpu/idt.h"
#include "arch/x86_64/cpu/apic.h"
#include "include/serial.h"

#define I8042_DATA 0x60
#define I8042_STAT 0x64
#define I8042_CMD  0x64

#define SR_OBF 0x01
#define SR_IBF 0x02
#define SR_AUX 0x20

#define CC_READ_CFG  0x20
#define CC_WRITE_CFG 0x60
#define CC_ENA_PORT2 0xA8
#define CC_WRITE_P2  0xD4

#define CFG_IRQ12    (1 << 1)

#define M_ACK         0xFA
#define M_ID_WHEEL    0x03

/* evdev button codes, mirrored so the cursor state needs no extra header. */
#define EV_BTN_LEFT   0x110
#define EV_BTN_RIGHT  0x111
#define EV_BTN_MIDDLE 0x112

/* Test-console cursor state. */
static volatile int cur_x = 0, cur_y = 0, cur_b = 0;
static volatile int cur_max_x = 1280, cur_max_y = 800;

/* Packet assembly: set once the IMPS/2 handshake succeeded, otherwise every
 * packet is 3 bytes. Button bits live in byte 0 either way. */
static volatile int     has_wheel = 0;
static volatile uint8_t pkt[4];
static volatile int     pkt_len = 0;

static inline uint8_t aux_inb(uint16_t p) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}

static inline void aux_outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p));
}

static void aux_ibf_wait(void) {
    uint32_t t = 200000;
    while (--t && (aux_inb(I8042_STAT) & SR_IBF));
}

static int aux_obf_wait(uint8_t want_aux, uint8_t *out) {
    uint32_t t = 200000;
    for (;;) {
        uint8_t st = aux_inb(I8042_STAT);
        if ((st & SR_OBF) && ((st & SR_AUX) != 0) == (want_aux != 0)) {
            *out = aux_inb(I8042_DATA);
            return 0;
        }
        if (!--t) return -1;
    }
}

static void aux_cmd(uint8_t c) {
    aux_ibf_wait();
    aux_outb(I8042_CMD, c);
}

static void aux_write_dev(uint8_t b) {
    aux_ibf_wait();
    aux_outb(I8042_CMD, CC_WRITE_P2);
    aux_ibf_wait();
    aux_outb(I8042_DATA, b);
}

/* Drain every pending output byte. Critical: a stale AUX byte parked in the
 * output buffer blocks IRQ1 delivery forever, because the keyboard handler is
 * required to skip AUX bytes - nothing else would ever consume it. */
static void mouse_flush(void) {
    uint32_t n = 64;
    while (n-- && (aux_inb(I8042_STAT) & SR_OBF))
        (void)aux_inb(I8042_DATA);
}

static int g_active = 0;

int mouse_active(void) { return g_active; }

/* Send one mouse command byte and wait for its ACK. Responses can queue up
 * (reset answers ACK + BAT + ID), so anything that is not the ACK gets
 * skipped rather than trusted: one late byte must not desync the sequence. */
static int mouse_cmd_ack(uint8_t cmd) {
    aux_write_dev(cmd);
    for (int i = 0; i < 8; i++) {
        uint8_t b = 0;
        if (aux_obf_wait(1, &b) != 0) return -1;
        if (b == M_ACK) return 0;
    }
    return -1;
}

static int mouse_set_rate(uint8_t rate) {
    if (mouse_cmd_ack(0xF3) != 0) return -1;
    return mouse_cmd_ack(rate);
}

static void mouse_emit_packet(void) {
    uint8_t b0 = pkt[0];
    int left   = (b0 & 0x01) != 0;
    int right  = (b0 & 0x02) != 0;
    int middle = (b0 & 0x04) != 0;

    /* Deltas are 9-bit two's complement: bit 4 holds X's sign, bit 5 Y's. */
    int dx = (int)pkt[1] - (((b0 & 0x10) != 0) ? 256 : 0);
    int dy = (int)pkt[2] - (((b0 & 0x20) != 0) ? 256 : 0);
    int wheel = (has_wheel && pkt_len == 4) ? (int)(int8_t)pkt[3] : 0;

    int nb = (left ? 1 : 0) | (right ? 2 : 0) | (middle ? 4 : 0);
    int ob = cur_b;
    cur_b = nb;
    if ((nb & 1) != (ob & 1)) evdev_post_btn(EV_BTN_LEFT, (nb & 1) ? 1 : 0);
    if ((nb & 2) != (ob & 2)) evdev_post_btn(EV_BTN_RIGHT, (nb & 2) ? 1 : 0);
    if ((nb & 4) != (ob & 4)) evdev_post_btn(EV_BTN_MIDDLE, (nb & 4) ? 1 : 0);

    /* PS/2 Y grows downward; screen coordinates do not. */
    if (dx) { cur_x += dx; if (cur_x < 0) cur_x = 0; }
    if (dy) { cur_y -= dy; if (cur_y < 0) cur_y = 0; }
    if (cur_x >= cur_max_x) cur_x = cur_max_x - 1;
    if (cur_y >= cur_max_y) cur_y = cur_max_y - 1;

    evdev_post_rel(dx, -dy, wheel);
}

static void mouse_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint8_t st = aux_inb(I8042_STAT);
    if (!(st & SR_OBF)) return;
    if (!(st & SR_AUX)) return;   /* keyboard byte: not ours */
    uint8_t b = aux_inb(I8042_DATA);

    /* Byte 0 always carries bit 3; anything else is a stray byte from the
     * init handshake, so resync instead of decoding a shifted packet. */
    if (pkt_len == 0 && (b & 0x08) == 0) return;

    pkt[pkt_len++] = b;
    if (pkt_len >= (has_wheel ? 4 : 3)) {
        mouse_emit_packet();
        pkt_len = 0;
    }
}

void mouse_init(void) {
    aux_cmd(CC_ENA_PORT2);
    mouse_flush();

    /* Controller config: bit 1 is the IRQ12 enable. keyboard_init left
     * XLATE|IRQ1 in place; this only ORs the mouse line in. */
    aux_cmd(CC_READ_CFG);
    uint8_t cfg = 0;
    if (aux_obf_wait(0, &cfg) == 0) {
        aux_cmd(CC_WRITE_CFG);
        aux_ibf_wait();
        aux_outb(I8042_DATA, (uint8_t)(cfg | CFG_IRQ12));
    }
    mouse_flush();

    /* The IRQ handler must exist before stream enable: the first packet can
     * land the moment the mouse starts reporting. */
    if (apic_available()) {
        apic_irq_unmask(12);
    } else {
        uint8_t mask = aux_inb(0xA1);
        mask &= ~(1 << 4); /* bit 4 of the slave PIC = IRQ12 */
        aux_outb(0xA1, mask);
    }
    register_interrupt_handler(0x2C, mouse_irq_handler); /* IRQ12 */

    if (mouse_cmd_ack(0xFF) != 0) {      /* reset */
        serial_puts("[MOUSE] no PS/2 mouse on the aux port\n");
        mouse_flush();
        return;
    }
    mouse_flush();                        /* BAT completion + device ID */

    int wheel = 0;
    uint8_t id = 0;
    if (mouse_set_rate(200) == 0 && mouse_set_rate(100) == 0 &&
        mouse_set_rate(80) == 0) {
        if (mouse_cmd_ack(0xF2) == 0 && aux_obf_wait(1, &id) == 0 &&
            id == M_ID_WHEEL)
            wheel = 1;
    }
    has_wheel = wheel;

    if (mouse_cmd_ack(0xF4) != 0) {      /* enable stream */
        serial_puts("[MOUSE] PS/2 enable-stream failed\n");
        mouse_flush();
        return;
    }

    g_active = 1;
    serial_puts(wheel ? "[MOUSE] IMPS/2 wheel mouse on IRQ12\n"
                      : "[MOUSE] PS/2 mouse on IRQ12\n");
}

void mouse_cursor_pos(int *x, int *y) {
    if (x) *x = cur_x;
    if (y) *y = cur_y;
}

int mouse_buttons(void) { return cur_b; }
