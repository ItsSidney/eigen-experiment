/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * serial.h - Early serial (COM1) debug output.
 * Used before any proper console or display driver exists.
 */

#ifndef KERNEL_SERIAL_H
#define KERNEL_SERIAL_H

#include <stdint.h>
#include <stddef.h>

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %b0, %w1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %w1, %b0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %w0, %w1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %w1, %w0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %w1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %w1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

static inline void serial_init(void) {
    outb(COM1 + 1, 0x00); /* disable interrupts */
    outb(COM1 + 3, 0x80); /* DLAB on */
    outb(COM1 + 0, 0x03); /* 38400 baud lo */
    outb(COM1 + 1, 0x00); /* 38400 baud hi */
    outb(COM1 + 3, 0x03); /* 8N1, DLAB off */
    outb(COM1 + 2, 0xC7); /* enable FIFO, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B); /* RTS/DSR set */
}

/* Implemented in drivers/char/serial.c (with the klog ring). */
void serial_putc(char c);
void serial_puts(const char *s);
void serial_puthex(uint64_t val);
/* Raw UART byte: no timestamp, no klog. For console forwarding, so
 * displayed output is never re-logged into the ring. */
void serial_putc_raw(char c);

/* In-memory kernel log ring (see serial.c). */
void klog_push(char c);
uint64_t klog_len(void);
int64_t klog_read(uint64_t off, size_t size, void *buf);

#endif /* KERNEL_SERIAL_H */
