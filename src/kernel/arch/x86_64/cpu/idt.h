/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * idt.h - Interrupt Descriptor Table definitions and frame structure.
 */

#ifndef KERNEL_IDT_H
#define KERNEL_IDT_H

#include <stdint.h>

struct interrupt_frame {
    /* Saved general purpose registers */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    /* Pushed by isr stub */
    uint64_t int_no;
    uint64_t err_code;

    /* Pushed by CPU hardware */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

typedef void (*isr_handler_t)(struct interrupt_frame *frame);

void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);
void register_interrupt_handler(uint8_t n, isr_handler_t handler);
uint64_t idt_get_irq_count(uint8_t irq);

/* CPU exception snapshot for the recovery console. Recorded for every
 * vector < 32 (Ring-3 kills and Ring-0 panics alike); the ring holds
 * the 8 most recent, newest last. Static storage: safe in ISR context. */
#define EXC_RING_SIZE 8

struct exc_record {
    uint64_t vector;
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
    uint64_t err_code;
    uint64_t cr2;      /* fault address for #PF, else 0 */
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t cr3;
    uint64_t ticks;    /* PIT ticks at capture */
    uint32_t pid;      /* faulting process, 0 if none */
};

/* Number of records stored (capped at EXC_RING_SIZE). */
uint64_t exc_count(void);
/* Copy record i (0 = oldest retained) into out. Returns 0 on success. */
int exc_get(uint64_t i, struct exc_record *out);
/* Short mnemonic for a vector: #DE, #PF, #GP, ... */
const char *exc_mnemonic(uint64_t vec);

#endif /* KERNEL_IDT_H */
