/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * keyboard.c - PS/2 keyboard driver, BIOS & UEFI compatible.
 *
 * UEFI problem:
 *   In UEFI mode, the firmware leaves the i8042 keyboard controller in an
 *   indeterminate state. Depending on the firmware:
 *   - Clock may be inhibited (CLK1 bit set in config)
 *   - XLATE (Set2->Set1 hardware translation) may be OFF
 *   - Scan codes may be in Set 2 instead of the BIOS-standard Set 1
 *
 * My approach (UEFI-safe):
 *   1. Fully reinitialize the i8042 controller
 *   2. Explicitly request XLATE=1 (translate Set 2 -> Set 1 in hardware)
 *   3. If XLATE can't be confirmed, fall back to a complete Set 2 table
 *   4. getchar() uses: Serial (QEMU) > direct OBF poll > IRQ ring buffer
 *
 * This lets Eigen boot on BIOS, UEFI (VirtualBox), and QEMU identically.
 */

#include <stdint.h>
#include "keyboard.h"
#include "vterm_glue.h"
#include "mouse.h"
#include "dev/input/evdev.h"
#include "arch/x86_64/cpu/idt.h"
#include "arch/x86_64/cpu/apic.h"
#include "proc/process.h"
#include "fs/vfs/vfs.h"
#include "include/serial.h"

/* ── i8042 I/O ports ────────────────────────────────────────────────────── */
#define I8042_DATA  0x60
#define I8042_STAT  0x64
#define I8042_CMD   0x64

/* Status register bits */
#define SR_OBF  0x01   /* Output Buffer Full  - data waiting to be read  */
#define SR_IBF  0x02   /* Input  Buffer Full  - controller busy, wait    */
#define SR_SYS  0x04   /* System Flag         */
#define SR_CMD  0x08   /* 0=data to keyboard, 1=data to controller       */
#define SR_AUX  0x20   /* 1=byte came from the mouse (aux) port          */

/* Controller commands */
#define CC_READ_CFG    0x20   /* Read controller config byte    */
#define CC_WRITE_CFG   0x60   /* Write controller config byte   */
#define CC_DIS_PORT2   0xA7   /* Disable port 2 (mouse)         */
#define CC_ENA_PORT2   0xA8   /* Enable port 2                  */
#define CC_DIS_PORT1   0xAD   /* Disable port 1 (keyboard)      */
#define CC_ENA_PORT1   0xAE   /* Enable port 1                  */
#define CC_RESET       0xFF   /* Reset keyboard                 */

/* Config byte bits */
#define CFG_IRQ1    (1 << 0)  /* 1 = IRQ1 enabled for port 1    */
#define CFG_IRQ12   (1 << 1)  /* 1 = IRQ12 enabled for port 2   */
#define CFG_SYS     (1 << 2)  /* System flag - must remain 1    */
#define CFG_CLK1    (1 << 4)  /* 1 = port 1 clock DISABLED      */
#define CFG_CLK2    (1 << 5)  /* 1 = port 2 clock DISABLED      */
#define CFG_XLATE   (1 << 6)  /* 1 = translate Set 2 -> Set 1   */

/* Keyboard device commands */
#define KBD_RESET       0xFF
#define KBD_ENA_SCAN    0xF4
#define KBD_DIS_SCAN    0xF5
#define KBD_SET_LEDS    0xED
#define KBD_ACK         0xFA
#define KBD_RESEND      0xFE

/* Shift/modifier scancodes (Set 2) */
#define SC2_BREAK        0xF0
#define SC2_EXT          0xE0
#define SC2_LSHIFT_MAKE  0x12
#define SC2_RSHIFT_MAKE  0x59

/* ── Ring buffer ─────────────────────────────────────────────────────────── */
#define KBD_BUF_SIZE 256
static volatile char     kbuf[KBD_BUF_SIZE];
static volatile uint32_t khead = 0;
static volatile uint32_t ktail = 0;
static volatile int      shift = 0;
static volatile int      ctrl = 0;
static volatile int      caps = 0;           /* Caps Lock latch (toggles)   */
static volatile int      kbd_led_dirty = 0;  /* set in IRQ, cleared by leds */
static volatile int      kbd_irq_alive = 0;  /* 1 once IRQ1 delivery proven */

/* ── Scancode Set 1 tables (used when XLATE=1 is active) ────────────────── */
/* i8042 hardware translates Set 2 -> Set 1 transparently when XLATE=1.     */
/* Set 1 make codes (release codes have bit 7 set, we just ignore them).    */
static const char sc1_normal[256] = {
    [0x01] = '\x1b', /* Escape */
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b', /* Backspace */
    [0x0F] = '\t', /* Tab */
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n', /* Enter */
    [0x1D] = 0,    /* Left Ctrl - not printed */
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2A] = 0,    /* Left Shift */
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x36] = 0,   /* Right Shift */
    [0x37] = '*', /* Numpad * */
    [0x38] = 0,   /* Left Alt */
    [0x39] = ' ',
};

static const char sc1_shift[256] = {
    [0x01] = '\x1b', /* Escape */
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x37] = '*',
    [0x39] = ' ',
};

/* ── Scancode Set 2 tables (used when XLATE=0 / UEFI leaves raw Set 2) ──── */
/* Standard PS/2 Set 2 US keyboard make codes -> ASCII.                     */
/* Reference: https://wiki.osdev.org/PS/2_Keyboard (Scan Code Set 2)        */
static const char sc2_normal[256] = {
    [0x76] = '\x1b', /* Escape */
    /* Row 1 (number row) */
    [0x0E] = '`',
    [0x16] = '1', [0x1E] = '2', [0x26] = '3', [0x25] = '4',
    [0x2E] = '5', [0x36] = '6', [0x3D] = '7', [0x3E] = '8',
    [0x46] = '9', [0x45] = '0', [0x4E] = '-', [0x55] = '=',
    [0x66] = '\b', /* Backspace */
    /* Row 2 (QWERTY) */
    [0x0D] = '\t', /* Tab */
    [0x15] = 'q', [0x1D] = 'w', [0x24] = 'e', [0x2D] = 'r',
    [0x2C] = 't', [0x35] = 'y', [0x3C] = 'u', [0x43] = 'i',
    [0x44] = 'o', [0x4D] = 'p', [0x54] = '[', [0x5B] = ']',
    [0x5D] = '\\',
    /* Row 3 (ASDF) */
    [0x1C] = 'a', [0x1B] = 's', [0x23] = 'd', [0x2B] = 'f',
    [0x34] = 'g', [0x33] = 'h', [0x3B] = 'j', [0x42] = 'k',
    [0x4B] = 'l', [0x4C] = ';', [0x52] = '\'',
    [0x5A] = '\n', /* Enter */
    /* Row 4 (ZXCV) */
    [0x1A] = 'z', [0x22] = 'x', [0x21] = 'c', [0x2A] = 'v',
    [0x32] = 'b', [0x31] = 'n', [0x3A] = 'm',
    [0x41] = ',', [0x49] = '.', [0x4A] = '/',
    /* Space */
    [0x29] = ' ',
    /* Numpad */
    [0x7C] = '*',
};

static const char sc2_shift[256] = {
    [0x76] = '\x1b', /* Escape */
    [0x0E] = '~',
    [0x16] = '!', [0x1E] = '@', [0x26] = '#', [0x25] = '$',
    [0x2E] = '%', [0x36] = '^', [0x3D] = '&', [0x3E] = '*',
    [0x46] = '(', [0x45] = ')', [0x4E] = '_', [0x55] = '+',
    [0x66] = '\b',
    [0x0D] = '\t',
    [0x15] = 'Q', [0x1D] = 'W', [0x24] = 'E', [0x2D] = 'R',
    [0x2C] = 'T', [0x35] = 'Y', [0x3C] = 'U', [0x43] = 'I',
    [0x44] = 'O', [0x4D] = 'P', [0x54] = '{', [0x5B] = '}',
    [0x5D] = '|',
    [0x1C] = 'A', [0x1B] = 'S', [0x23] = 'D', [0x2B] = 'F',
    [0x34] = 'G', [0x33] = 'H', [0x3B] = 'J', [0x42] = 'K',
    [0x4B] = 'L', [0x4C] = ':', [0x52] = '"',
    [0x5A] = '\n',
    [0x1A] = 'Z', [0x22] = 'X', [0x21] = 'C', [0x2A] = 'V',
    [0x32] = 'B', [0x31] = 'N', [0x3A] = 'M',
    [0x41] = '<', [0x49] = '>', [0x4A] = '?',
    [0x29] = ' ',
};

/* ── Mode tracking ──────────────────────────────────────────────────────── */
/* 1 = hardware XLATE active, reading Set 1 codes                           */
/* 0 = no XLATE,  reading raw Set 2 codes                                   */
static int  kbd_xlate_on  = 1;
static int  sc2_break_pending = 0;
static int  sc2_ext_pending   = 0;

/* ── i8042 low-level helpers ─────────────────────────────────────────────── */
static void ibf_wait(void) {
    uint32_t t = 200000;
    while (--t && (inb(I8042_STAT) & SR_IBF));
}

static void obf_flush(void) {
    uint32_t n = 32;
    while (n-- && (inb(I8042_STAT) & SR_OBF))
        (void)inb(I8042_DATA);
}

static int obf_wait_read(uint8_t *out) {
    uint32_t t = 200000;
    while (--t && !(inb(I8042_STAT) & SR_OBF));
    if (!t) return -1;
    *out = inb(I8042_DATA);
    return 0;
}

static void ctrl_cmd(uint8_t c) {
    ibf_wait();
    outb(I8042_CMD, c);
}

static void ctrl_data(uint8_t d) {
    ibf_wait();
    outb(I8042_DATA, d);
}

/* ── Push a decoded character into the ring buffer ──────────────────────── */
static void kbd_push_char(char ch) {
    if (!ch) return;
    /* ISIG signals are raised immediately, even if no one is reading */
    tty_handle_signal(ch);
    vterm_keyboard_feed(ch);
    uint32_t nx = (khead + 1) % KBD_BUF_SIZE;
    if (nx != ktail) {
        kbuf[khead] = ch;
        khead = nx;
    }
}

/* Push an ANSI escape sequence (e.g. arrow keys) into the ring buffer. */
static void kbd_push_seq(const char *s) {
    while (*s) kbd_push_char(*s++);
}

/* Set 1 extended (E0-prefixed) keys -> ANSI sequences. */
static int sc1_ext = 0;

static void kbd_set1_extended(uint8_t b) {
    const char *seq = 0;
    uint16_t kc = 0;
    switch (b) {
        case 0x48: seq = "\x1b[A"; kc = 103; break; /* Up */
        case 0x50: seq = "\x1b[B"; kc = 108; break; /* Down */
        case 0x4D: seq = "\x1b[C"; kc = 106; break; /* Right */
        case 0x4B: seq = "\x1b[D"; kc = 105; break; /* Left */
        case 0x47: seq = "\x1b[H"; kc = 102; break; /* Home */
        case 0x4F: seq = "\x1b[F"; kc = 107; break; /* End */
        case 0x49: seq = "\x1b[5~"; kc = 104; break; /* PgUp */
        case 0x51: seq = "\x1b[6~"; kc = 109; break; /* PgDn */
        case 0x52: seq = "\x1b[2~"; kc = 110; break; /* Insert */
        case 0x53: seq = "\x1b[3~"; kc = 111; break; /* Delete */
        default: break;
    }
    if (seq) kbd_push_seq(seq);
    if (kc) evdev_post_key(kc, 1);
}

/* Set 2 extended (E0-prefixed) keys -> ANSI sequences. */
static void kbd_set2_extended(uint8_t b) {
    const char *seq = 0;
    switch (b) {
        case 0x75: seq = "\x1b[A"; break; /* Up */
        case 0x72: seq = "\x1b[B"; break; /* Down */
        case 0x74: seq = "\x1b[C"; break; /* Right */
        case 0x6B: seq = "\x1b[D"; break; /* Left */
        case 0x6C: seq = "\x1b[H"; break; /* Home */
        case 0x69: seq = "\x1b[F"; break; /* End */
        case 0x7D: seq = "\x1b[5~"; break; /* PgUp */
        case 0x7A: seq = "\x1b[6~"; break; /* PgDn */
        case 0x70: seq = "\x1b[2~"; break; /* Insert */
        case 0x71: seq = "\x1b[3~"; break; /* Delete */
        default: break;
    }
    if (seq) kbd_push_seq(seq);
}

/* ── Process one raw byte from the keyboard port ────────────────────────── */
/* Called from the IRQ1 handler or the OBF polling path.                    */
static char ctrl_map(char ch) {
    if (!ctrl || !ch) return ch;
    if (ch >= 'a' && ch <= 'z') return (char)(ch - 'a' + 1);
    if (ch >= 'A' && ch <= 'Z') return (char)(ch - 'A' + 1);
    if (ch == '[') return 0x1B;
    if (ch == '\\') return 0x1C;
    if (ch == ']') return 0x1D;
    if (ch == '^' || ch == '6') return 0x1E;
    if (ch == '_' || ch == '-') return 0x1F;
    if (ch == ' ') return 0x00;
    return ch;
}

/* Caps Lock and Shift each invert a letter's case, so holding both returns
 * the letter to lowercase - the behaviour every PC keyboard has. Only
 * alphabetic keys are affected: Shift+1 must still produce '!', which is
 * why this runs on the table result rather than on the scancode. */
static char apply_case(char ch, int shifted, int caps_on) {
    if (ch >= 'a' && ch <= 'z')
        return (shifted ^ caps_on) ? (char)(ch - 'a' + 'A') : ch;
    if (ch >= 'A' && ch <= 'Z')
        return (shifted ^ caps_on) ? ch : (char)(ch - 'A' + 'a');
    return ch;
}

/* Caps Lock LED refresh. Deliberately not done in the IRQ handler: it needs
 * several controller round-trips, and the ACK bytes it reads would otherwise
 * be delivered to us as if they were scancodes. Interrupts stay off for the
 * whole exchange so a keystroke cannot be consumed by it. */
static void kbd_update_leds(void) {
    if (!kbd_led_dirty) return;
    kbd_led_dirty = 0;
    if (!kbd_xlate_on) return; /* UEFI path: controller state is not ours */

    uint64_t flags;
    __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(flags)::"memory");

    uint8_t ack = 0;
    ibf_wait();
    outb(I8042_DATA, KBD_SET_LEDS);
    if (obf_wait_read(&ack) == 0 && ack == KBD_ACK) {
        ibf_wait();
        outb(I8042_DATA, caps ? 0x04 : 0x00); /* bit 2 = Caps Lock */
        (void)obf_wait_read(&ack);
    }

    __asm__ volatile("pushq %0\n\tpopfq" :: "r"(flags):"memory");
}

/* ── evdev mirror: Set 1 scancode -> Linux keycode ──────────────────────
 * The tty path and evdev are independent consumers of the same keystrokes:
 * the console reads translated characters, a compositor reads keycodes with
 * press/release pairs. Only the translate-ON path (XLATE=1) is mirrored for
 * now; the Set 2 fallback feeds the tty and nothing else. */
static const uint16_t sc1_keycode[128] = {
    [0x01] = 1,                            /* ESC */
    [0x02] = 2,  [0x03] = 3,  [0x04] = 4,  [0x05] = 5,  [0x06] = 6,
    [0x07] = 7,  [0x08] = 8,  [0x09] = 9,  [0x0A] = 10, [0x0B] = 11,
    [0x0C] = 12, [0x0D] = 13,              /* - = */
    [0x0E] = 14, [0x0F] = 15,              /* backspace, tab */
    [0x10] = 16, [0x11] = 17, [0x12] = 18, [0x13] = 19, [0x14] = 20,
    [0x15] = 21, [0x16] = 22, [0x17] = 23, [0x18] = 24, [0x19] = 25,
    [0x1A] = 26, [0x1B] = 27,              /* [ ] */
    [0x1C] = 28, [0x1D] = 29,              /* enter, left ctrl */
    [0x1E] = 30, [0x1F] = 31, [0x20] = 32, [0x21] = 33, [0x22] = 34,
    [0x23] = 35, [0x24] = 36, [0x25] = 37, [0x26] = 38, [0x27] = 39,
    [0x28] = 40, [0x29] = 41,              /* ; ' ` */
    [0x2A] = 42, [0x2B] = 43,              /* left shift, backslash */
    [0x2C] = 44, [0x2D] = 45, [0x2E] = 46, [0x2F] = 47, [0x30] = 48,
    [0x31] = 49, [0x32] = 50, [0x33] = 51, [0x34] = 52, [0x35] = 53,
    [0x36] = 54, [0x37] = 55, [0x38] = 56, [0x39] = 57, [0x3A] = 58,
};

/* Mirror one scancode into the evdev ring. `value` is 1 on press, 0 on
 * release - evdev clients expect both edges. */
static void kbd_evdev_key(uint8_t sc1, int value) {
    uint8_t code = (uint8_t)(sc1 & 0x7F);
    uint16_t kc = sc1_keycode[code];
    if (kc) evdev_post_key(kc, value);
}

static void kbd_process_byte(uint8_t b) {
    if (kbd_xlate_on) {
        /* ─── Set 1 (XLATE=1): BIOS standard path ─── */
        if (b == 0xE0) { sc1_ext = 1; return; }
        if (sc1_ext) {
            sc1_ext = 0;
            if (b == 0x1D) { ctrl = 1; return; }
            if (b == 0x9D) { ctrl = 0; return; }
            if (b & 0x80) return;
            kbd_set1_extended(b);
            return;
        }
        /* Shift make/break */
        if (b == 0x2A || b == 0x36) {
            shift = 1;
            kbd_evdev_key(b, 1);
            return;
        }
        if (b == 0xAA || b == 0xB6) {
            shift = 0;
            kbd_evdev_key(b, 0);
            return;
        }
        /* Caps Lock: 0x3A latches on make, the 0xBA break is a no-op. */
        if (b == 0x3A) {
            caps = !caps;
            kbd_led_dirty = 1;
            kbd_evdev_key(b, 1);
            return;
        }
        if (b == 0xBA) { kbd_evdev_key(b, 0); return; }
        if (b == 0x1D) { ctrl = 1; kbd_evdev_key(b, 1); return; }
        if (b == 0x9D) { ctrl = 0; kbd_evdev_key(b, 0); return; }
        if (b & 0x80) { kbd_evdev_key(b, 0); return; } /* key release */
        kbd_evdev_key(b, 1);
        /* Look up and push */
        char ch = shift ? sc1_shift[b] : sc1_normal[b];
        ch = apply_case(ch, shift, caps);
        ch = ctrl_map(ch);
        kbd_push_char(ch);
    } else {
        /* ─── Set 2 (XLATE=0): UEFI path ─── */
        if (b == SC2_BREAK) { sc2_break_pending = 1; return; }
        if (b == SC2_EXT)   { sc2_ext_pending   = 1; return; }

        if (sc2_break_pending) {
            /* Key release */
            if (b == SC2_LSHIFT_MAKE || b == SC2_RSHIFT_MAKE) {
                shift = 0;
            } else if (b == 0x14 && !sc2_ext_pending) {
                ctrl = 0;
            } else if (b == 0x14 && sc2_ext_pending) {
                ctrl = 0;
            }
            sc2_break_pending = 0;
            sc2_ext_pending   = 0;
            return;
        }

        /* Key make */
        /* Caps Lock (Set 2 make 0x58); its release arrives as 0x58 after
         * the 0xF0 break prefix and needs no action - the latch holds. */
        if (b == 0x58) {
            caps = !caps;
            kbd_led_dirty = 1;
            sc2_ext_pending = 0;
            return;
        }
        if (b == SC2_LSHIFT_MAKE || b == SC2_RSHIFT_MAKE) {
            shift = 1;
            sc2_ext_pending = 0;
            return;
        }
        if (b == 0x14 && !sc2_ext_pending) {
            ctrl = 1;
            return;
        }
        if (b == 0x14 && sc2_ext_pending) {
            ctrl = 1;
            sc2_ext_pending = 0;
            return;
        }

        if (!sc2_ext_pending) {
            char ch = shift ? sc2_shift[b] : sc2_normal[b];
            ch = apply_case(ch, shift, caps);
            ch = ctrl_map(ch);
            kbd_push_char(ch);
        } else {
            kbd_set2_extended(b);
        }
        sc2_ext_pending = 0;
    }
}

/* ── IRQ 1 handler ───────────────────────────────────────────────────────── */
static void keyboard_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint8_t st = inb(I8042_STAT);
    if (!(st & SR_OBF)) return;
    if (st & SR_AUX) {
        /* A byte from the mouse port. IRQ12 owns it while the mouse is live;
         * when it is not, this byte would sit in the buffer forever blocking
         * every keystroke, so drop it and keep the keyboard alive. */
        if (!mouse_active()) (void)inb(I8042_DATA);
        return;
    }
    kbd_irq_alive = 1; /* IRQ delivery proven working */
    kbd_process_byte(inb(I8042_DATA));
}

/* ── Keyboard initialization ─────────────────────────────────────────────── */
void keyboard_init(void) {
    /* Step 1: Disable both ports so the keyboard doesn't interfere */
    ctrl_cmd(CC_DIS_PORT1);
    ctrl_cmd(CC_DIS_PORT2);

    /* Step 2: Flush anything sitting in the output buffer
     * (UEFI may leave leftover bytes, including BAT result, self-test, etc.) */
    obf_flush();

    /* Step 3: Read current config */
    ctrl_cmd(CC_READ_CFG);
    uint8_t cfg_byte = 0;
    obf_wait_read(&cfg_byte);

    /* Step 4: Patch config:
     *   - Enable IRQ1            (bit 0 = 1)
     *   - Enable XLATE           (bit 6 = 1) -> translates Set 2 to Set 1
     *   - Clear clock-disabled   (bit 4 = 0) -> re-enable port 1 clock
     *   - Clear port 2 IRQ       (bit 1 = 0) -> we don't use mouse yet
     */
    cfg_byte |=  (CFG_IRQ1 | CFG_XLATE);
    cfg_byte &= ~(CFG_CLK1 | CFG_CLK2);

    ctrl_cmd(CC_WRITE_CFG);
    ctrl_data(cfg_byte);

    /* Step 5: Verify XLATE was accepted by reading config back */
    ctrl_cmd(CC_READ_CFG);
    uint8_t cfg_verify = 0;
    obf_wait_read(&cfg_verify);

    if (cfg_verify & CFG_XLATE) {
        kbd_xlate_on = 1;
        serial_puts("[KBD] XLATE=1 confirmed, using Set 1 scancode tables.\n");
    } else {
        kbd_xlate_on = 0;
        serial_puts("[KBD] XLATE=0 (UEFI mode), using Set 2 scancode tables.\n");
    }

    /* Step 6: Re-enable port 1 */
    ctrl_cmd(CC_ENA_PORT1);

    /* Step 7: Flush BAT byte that the keyboard sends when port is enabled */
    obf_flush();

    /* Step 8: Send Enable Scanning command to keyboard device */
    ibf_wait();
    outb(I8042_DATA, KBD_ENA_SCAN);
    uint8_t ack = 0;
    if (obf_wait_read(&ack) == 0 && ack == KBD_ACK) {
        serial_puts("[KBD] Keyboard scanning enabled (ACK received).\n");
    } else {
        serial_puts("[KBD] Warning: no ACK from keyboard after enable-scan.\n");
    }

    /* Step 9: Unmask IRQ1 (IOAPIC when present, else 8259A) */
    if (apic_available()) {
        apic_irq_unmask(1);
    } else {
        uint8_t mask = inb(0x21);
        mask &= ~(1 << 1); /* bit 1 = IRQ1 */
        outb(0x21, mask);
    }

    serial_puts("[KBD] PS/2 initialized (XLATE=");
    serial_puts(kbd_xlate_on ? "1" : "0");
    serial_puts(", scan enabled, IRQ 1 active).\n");

    /* Register IRQ1 handler (interrupt vector 0x21 = IRQ1) */
    register_interrupt_handler(0x21, keyboard_irq_handler);
}

/* ── keyboard_getchar ────────────────────────────────────────────────────── */
/* Blocking: tries 3 paths in priority order.                               */
/* A) COM1 serial (QEMU -serial stdio)                                      */
/* B) Direct OBF poll (works without IRQ delivery - VirtualBox UEFI)        */
/* C) IRQ ring buffer (standard IRQ path)                                   */

/* Peek: nonzero if a decoded keystroke (or raw byte) is waiting. */
int keyboard_has_char(void) {
    if (inb(0x3F8 + 5) & 0x01) return 1;  /* serial data ready */
    if (inb(I8042_STAT) & SR_OBF) return 1; /* PS/2 byte waiting */
    if (khead != ktail) return 1;           /* decoded char buffered */
    return 0;
}

/* Single non-blocking pass over the 3 input paths.
 * Returns the decoded byte (0-255), or -1 when nothing is waiting. */
int keyboard_trygetchar(void) {
    kbd_update_leds(); /* Caps Lock LED, done here and not in the IRQ */

    /* Path A: Serial port (QEMU) */
    if (inb(0x3F8 + 5) & 0x01) {
        int c = inb(0x3F8);
        tty_handle_signal((char)c);
        return c;
    }

    /* Path B: Direct OBF poll — ONLY until IRQ1 delivery is proven alive.
     * Once the IRQ handler has consumed a byte, polling paths must NEVER
     * touch the controller: QEMU keeps the last byte presentable in the
     * output register, so a post-IRQ poll reads a stale duplicate and the
     * keystroke doubles. With IRQs alive, poll paths drain the ring
     * buffer only (Path C below). */
    if (!kbd_irq_alive) {
        uint64_t flags;
        __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(flags)::"memory");
        if (inb(I8042_STAT) & SR_OBF) {
            uint8_t raw = inb(I8042_DATA);
            kbd_process_byte(raw);
            __asm__ volatile("pushq %0\n\tpopfq" :: "r"(flags):"memory");
            if (khead != ktail) {
                unsigned char ch = kbuf[ktail];
                ktail = (ktail + 1) % KBD_BUF_SIZE;
                tty_handle_signal((char)ch);
                return ch;
            }
            return -1;
        }
        __asm__ volatile("pushq %0\n\tpopfq" :: "r"(flags):"memory");
    }

    /* Path C: IRQ ring buffer */
    if (khead != ktail) {
        unsigned char ch = kbuf[ktail];
        ktail = (ktail + 1) % KBD_BUF_SIZE;
        tty_handle_signal((char)ch);
        return ch;
    }

    return -1;
}

void tty_poll_signal(void) {
    kbd_update_leds(); /* Caps Lock LED, done here and not in the IRQ */

    /* Serial first (QEMU headless path). Serial bytes are already ASCII,
     * not scancodes: ISIG control chars raise and are discarded, anything
     * else is buffered raw for the next tty reader (echoed on read). */
    if (inb(0x3F8 + 5) & 0x01) {
        int c = inb(0x3F8);
        if (c == 3 || c == 28 || c == 26) {
            tty_handle_signal((char)c);
        } else {
            kbd_push_char((char)c);
        }
        return;
    }
    /* PS/2, keyboard bytes only (bit 0x20 set means AUX/mouse data).
     * Gated on !kbd_irq_alive for the same stale-duplicate reason as
     * trygetchar Path B: once IRQ1 delivers, never poll the controller. */
    if (!kbd_irq_alive) {
        uint64_t flags;
        __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(flags)::"memory");
        uint8_t st = inb(I8042_STAT);
        if ((st & SR_OBF) && !(st & 0x20)) {
            kbd_process_byte(inb(I8042_DATA));
        }
        __asm__ volatile("pushq %0\n\tpopfq" :: "r"(flags):"memory");
    }
    /* Pending default-fatal signals (e.g. Ctrl-C) kill on the spot. */
    sig_check_current();
}

char keyboard_getchar(void) {    for (;;) {
        int c = keyboard_trygetchar();
        if (c >= 0) return (char)c;

        /* Yield CPU without halting - pause is a hint to the CPU that we
         * are in a spin-wait loop, reduces power and improves performance.
         * Do NOT use 'hlt' here - in UEFI mode with APIC, hlt may never
         * wake because IRQ1 is routed through the IOAPIC (not 8259 PIC). */
        __asm__ volatile("pause");
    }
}
