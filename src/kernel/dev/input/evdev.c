/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * evdev.c - /dev/input/event0 (keyboard) and event1 (mouse): Linux evdev.
 *
 * Each device owns a ring of struct input_event
 *   { uint64_t sec, usec; uint16_t type, code; int32_t value; }
 * which is exactly the layout userspace expects, so reads copy slots out
 * verbatim. EV_SYN closes every IRQ delivery so a client never observes a
 * half-updated axis pair.
 */

#include <stdint.h>
#include <stddef.h>
#include "evdev.h"
#include "fs/vfs/vfs.h"
#include "drivers/char/keyboard.h"
#include "mm/kheap.h"
#include "include/serial.h"

/* Linux <linux/input-event-codes.h>, the subset evdev clients probe. */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04
#define SYN_REPORT 0

/* Linux O_NONBLOCK. Spelled out rather than included: this file takes no
 * libc headers. */
#define O_NONBLOCK 0x800

#define REL_X      0x00
#define REL_Y      0x01
#define REL_WHEEL  0x08

#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

struct input_event {
    uint64_t sec;
    uint64_t usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

#define EVDEV_SLOTS 256

struct evdev {
    struct input_event slots[EVDEV_SLOTS];
    volatile uint32_t  head;
    volatile uint32_t  tail;
};

static uint64_t ev_now_usec(void);

/* Device state. Declared before the queue helpers that reference it. */
static struct evdev    g_kbd;
static struct evdev    g_mouse;
static struct vfs_node g_evdev0_node;
static struct vfs_node g_evdev1_node;
static int             g_evdev_registered = 0;

static void ev_push(struct evdev *d, uint16_t type, uint16_t code,
                    int32_t value) {
    uint32_t nx = (d->head + 1) % EVDEV_SLOTS;
    if (nx == d->tail) d->tail = (d->tail + 1) % EVDEV_SLOTS;
    d->slots[d->head].sec = 0;
    d->slots[d->head].usec = ev_now_usec();
    d->slots[d->head].type = type;
    d->slots[d->head].code = code;
    d->slots[d->head].value = value;
    d->head = nx;
}

void evdev_post_rel(int dx, int dy, int wheel) {
    if (dx) ev_push(&g_mouse, EV_REL, REL_X, dx);
    if (dy) ev_push(&g_mouse, EV_REL, REL_Y, dy);
    if (wheel) ev_push(&g_mouse, EV_REL, REL_WHEEL, wheel);
    if (dx || dy || wheel) ev_push(&g_mouse, EV_SYN, SYN_REPORT, 0);
}

void evdev_post_key(uint16_t code, int32_t value) {
    ev_push(&g_kbd, EV_KEY, code, value);
    ev_push(&g_kbd, EV_SYN, SYN_REPORT, 0);
}

/* Buttons belong to the pointing device, not the keyboard. */
void evdev_post_btn(uint16_t code, int32_t value) {
    ev_push(&g_mouse, EV_KEY, code, value);
    ev_push(&g_mouse, EV_SYN, SYN_REPORT, 0);
}

void evdev_post_abs(int slot, int value) {
    (void)slot;
    ev_push(&g_mouse, EV_ABS, 0, value);
}

void evdev_post_syn(void) {
    ev_push(&g_mouse, EV_SYN, SYN_REPORT, 0);
}

int evdev_any_ready(void) {
    return g_kbd.head != g_kbd.tail || g_mouse.head != g_mouse.tail;
}

/* Readiness for poll/select on an event node: its own ring only, so mouse
 * motion never reports the keyboard fd readable and vice versa. */
int evdev_poll(struct vfs_node *node, int events) {
    struct evdev *d = (node == &g_evdev1_node) ? &g_mouse : &g_kbd;
    int mask = 0;
    if ((events & 0x001) && d->head != d->tail) mask |= 0x001;
    if (events & 0x004) mask |= 0x004;
    return mask & events;
}

int64_t evdev_read(struct vfs_node *node, uint64_t offset, size_t size,
                   void *buf) {
    (void)offset;
    if (!buf || size == 0) return 0;
    struct evdev *d = (node == &g_evdev1_node) ? &g_mouse : &g_kbd;

    /* Event streams are consumed whole: one call returns complete
     * input_event records only. Actual evdev clients always read in event
     * multiples; requests smaller than a single event cannot carry any data
     * and get -EINVAL (same as Linux). */
    size_t evsz = sizeof(struct input_event);
    size_t n = size / evsz;
    if (n == 0) return -22; /* -EINVAL: smaller than a single event */

    /* Block while the ring is empty, like a real event device: a compositor
     * parks on read() rather than spinning, and Ctrl-C still escapes. A
     * descriptor opened O_NONBLOCK must not park, though - clients such as
     * libwayland's event loop drain the device until it reports EAGAIN, so
     * blocking here would stall the whole loop after the first packet. */
    while (d->head == d->tail) {
        if (vfs_read_flags() & O_NONBLOCK) return -11; /* -EAGAIN */
        if (has_unblocked_pending()) return -4;        /* -EINTR */
        tty_poll_signal();
        __asm__ volatile("pause");
    }

    uint8_t *dst = (uint8_t *)buf;
    size_t got = 0;
    while (got < n && d->head != d->tail) {
        struct input_event tmp;
        uint8_t *src = (uint8_t *)&d->slots[d->tail];
        uint8_t *t = (uint8_t *)&tmp;
        for (size_t i = 0; i < sizeof(tmp); i++) t[i] = src[i];
        uint8_t *o = dst + got * sizeof(tmp);
        for (size_t i = 0; i < sizeof(tmp); i++) o[i] = t[i];
        d->tail = (d->tail + 1) % EVDEV_SLOTS;
        got++;
    }
    return (int64_t)(got * sizeof(struct input_event));
}

static void ev_name(struct vfs_node *nd, const char *s) {
    int i = 0;
    for (; s[i] && i < 127; i++) nd->name[i] = s[i];
    nd->name[i] = 0;
}

/* /dev/input: a directory node the two event nodes hang off. Owning it here
 * keeps the input tree self-contained - no mount, no devfs special case. */
static struct vfs_node g_input_dir;

static struct vfs_node *evdev_input_finddir(struct vfs_node *node,
                                            const char *name) {
    (void)node;
    if (name[0] == 0) return NULL;
    const char *a = g_evdev0_node.name, *b = g_evdev1_node.name;
    int i = 0;
    while (name[i] && name[i] == a[i]) i++;
    if (name[i] == 0 && a[i] == 0) return &g_evdev0_node;
    i = 0;
    while (name[i] && name[i] == b[i]) i++;
    if (name[i] == 0 && b[i] == 0) return &g_evdev1_node;
    return NULL;
}

static struct vfs_dirent *evdev_input_readdir(struct vfs_node *node,
                                             uint32_t index) {
    static struct vfs_dirent dent;
    (void)node;
    if (index > 1) return NULL;
    struct vfs_node *nd = index == 0 ? &g_evdev0_node : &g_evdev1_node;
    for (int i = 0; i < 128; i++) dent.name[i] = 0;
    for (int i = 0; nd->name[i] && i < 127; i++) dent.name[i] = nd->name[i];
    dent.ino = nd->inode;
    dent.type = VFS_CHARDEVICE;
    dent.size = 0;
    return &dent;
}

void evdev_init(void) {
    if (g_evdev_registered) return;

    for (int dev = 0; dev < 2; dev++) {
        struct vfs_node *nd = (dev == 0) ? &g_evdev0_node : &g_evdev1_node;
        ev_name(nd, dev == 0 ? "event0" : "event1");
        nd->type = VFS_CHARDEVICE;
        nd->flags = 0;
        nd->permissions = 0640;
        nd->uid = 0;
        nd->gid = 0;
        nd->size = 0;
        nd->inode = (uint64_t)(30 + dev);
        nd->device = NULL;
        nd->read = evdev_read;
        nd->write = NULL;
        nd->mmap = NULL;
        nd->mount_target = NULL;
    }

    ev_name(&g_input_dir, "input");
    g_input_dir.type = VFS_DIRECTORY;
    g_input_dir.flags = 0;
    g_input_dir.permissions = 0755;
    g_input_dir.uid = 0;
    g_input_dir.gid = 0;
    g_input_dir.size = 0;
    g_input_dir.inode = 32;
    g_input_dir.device = NULL;
    g_input_dir.finddir = evdev_input_finddir;
    g_input_dir.readdir = evdev_input_readdir;
    g_input_dir.mount_target = NULL;

    if (devfs_attach(&g_input_dir) != 0) {
        serial_puts("[INPUT] devfs has no room for /dev/input\n");
        return;
    }
    g_evdev_registered = 1;
    serial_puts("[INPUT] evdev ready (/dev/input/event0=kbd event1=mouse)\n");
}

static uint64_t ev_now_usec(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    /* TSC rate is platform-specific; this is a monotonic stamp, not wall
     * time. Userspace only needs ordering, which the counter gives. */
    return ((uint64_t)hi << 32) | lo;
}
