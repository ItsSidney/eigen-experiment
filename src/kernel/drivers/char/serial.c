/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * serial.c - COM1 output plus the in-memory kernel log ring.
 *
 * Every byte sent to serial is also appended to a static 64KB ring
 * (oldest output overwritten first), which backs /proc/kmsg for the
 * recovery console's dmesg screen. Static storage only: serial runs
 * before the heap exists and from interrupt context, so the push path
 * must never allocate or block.
 */

#include <stdint.h>
#include "include/serial.h"
#include "drivers/timer/pit.h"

#define KLOG_SIZE (64 * 1024)

static char klog_buf[KLOG_SIZE];
static uint64_t klog_head = 0; /* total bytes ever logged */
static int klog_line_start = 1;

void klog_push(char c) {
    if (c == '\r') return; /* serial framing only; keep \n stream clean */
    klog_buf[klog_head % KLOG_SIZE] = c;
    klog_head++;
}

/* Bytes currently retained (capped at KLOG_SIZE). */
uint64_t klog_len(void) {
    return klog_head < KLOG_SIZE ? klog_head : KLOG_SIZE;
}

/* Copy up to size bytes starting at absolute offset off into buf.
 * Returns bytes copied; 0 once off reaches the head. */
int64_t klog_read(uint64_t off, size_t size, void *buf) {
    uint64_t total = klog_len();
    uint64_t start = klog_head - total;
    if (off < start) off = start;
    if (off >= klog_head) return 0;
    if (off + size > klog_head) size = (size_t)(klog_head - off);
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < size; i++)
        dst[i] = (uint8_t)klog_buf[(off + i) % KLOG_SIZE];
    return (int64_t)size;
}

static void klog_stamp(void) {
    uint32_t freq = timer_get_freq();
    if (freq == 0) freq = 100;
    uint64_t ticks = timer_get_ticks();
    uint64_t sec = ticks / freq;
    uint64_t usec = (ticks % freq) * 1000000ULL / freq;
    /* "[    0.001042] " — fixed width, no libc. */
    char tmp[16];
    int pos = 0;
    tmp[pos++] = '[';
    uint64_t div = 10000; /* 5-wide seconds, space-padded */
    if (sec > 99999) sec = 99999;
    int started = 0;
    while (div > 0) {
        int d = (int)(sec / div);
        if (d || started || div == 1) { tmp[pos++] = (char)('0' + d); started = 1; }
        else tmp[pos++] = ' ';
        sec %= div;
        div /= 10;
    }
    tmp[pos++] = '.';
    for (uint64_t u = 100000; u > 0; u /= 10)
        tmp[pos++] = (char)('0' + (usec / u) % 10);
    tmp[pos++] = ']';
    tmp[pos++] = ' ';
    for (int i = 0; i < pos; i++) klog_push(tmp[i]);
}

void serial_putc_raw(char c) {
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, (uint8_t)c);
}

void serial_putc(char c) {
    if (klog_line_start && c != '\n' && c != '\r') {
        klog_stamp();
        klog_line_start = 0;
    }
    klog_push(c);
    if (c == '\n') klog_line_start = 1;
    serial_putc_raw(c);
}

void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_puthex(uint64_t val) {
    const char *digits = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_putc(digits[(val >> i) & 0xF]);
}
