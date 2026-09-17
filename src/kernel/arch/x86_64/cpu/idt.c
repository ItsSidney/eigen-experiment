/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * idt.c - Interrupt Descriptor Table setup, PIC remapping, and dispatch.
 */

#include <stdint.h>
#include <stddef.h>
#include "idt.h"
#include "arch/x86_64/cpu/gdt.h"
#include "arch/x86_64/cpu/apic.h"
#include "proc/process.h"
#include "mm/vmm.h"
#include "include/serial.h"
#include "drivers/char/console.h"
#include "drivers/timer/pit.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idtr idtr;
static isr_handler_t interrupt_handlers[256];
static volatile uint64_t irq_counts[16];

/* Exception history ring for the recovery console. */
static struct exc_record exc_ring[EXC_RING_SIZE];
static uint64_t exc_total = 0;

uint64_t exc_count(void) {
    return exc_total < EXC_RING_SIZE ? exc_total : EXC_RING_SIZE;
}

int exc_get(uint64_t i, struct exc_record *out) {
    if (!out || i >= exc_count()) return -1;
    uint64_t start = exc_total < EXC_RING_SIZE ? 0 : exc_total - EXC_RING_SIZE;
    *out = exc_ring[(start + i) % EXC_RING_SIZE];
    return 0;
}

static void exc_record(const struct interrupt_frame *frame) {
    struct exc_record *r = &exc_ring[exc_total % EXC_RING_SIZE];
    r->vector = frame->int_no;
    r->rip = frame->rip;
    r->rsp = frame->rsp;
    r->rflags = frame->rflags;
    r->err_code = frame->err_code;
    r->cr2 = 0;
    if (frame->int_no == 14)
        __asm__ volatile("mov %%cr2, %0" : "=r"(r->cr2));
    r->rax = frame->rax;
    r->rbx = frame->rbx;
    r->rcx = frame->rcx;
    r->rdx = frame->rdx;
    r->rsi = frame->rsi;
    r->rdi = frame->rdi;
    r->rbp = frame->rbp;
    __asm__ volatile("mov %%cr3, %0" : "=r"(r->cr3));
    r->ticks = timer_get_ticks();
    r->pid = current_process ? current_process->pid : 0;
    exc_total++;
}

uint64_t idt_get_irq_count(uint8_t irq) {
    if (irq >= 16) return 0;
    return irq_counts[irq];
}

const char *exc_mnemonic(uint64_t vec) {
    static const char *m[] = {
        "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
        "#DF", "CSO", "#TS", "#NP", "#SS", "#GP", "#PF", "RSV",
        "#MF", "#AC", "#MC", "#XM", "#VE", "#CP", "RSV", "RSV",
        "RSV", "RSV", "RSV", "RSV", "#HV", "#VC", "#SX", "RSV",
    };
    if (vec < 32) return m[vec];
    return "???";
}

extern void idt_load(struct idtr *idtr);

/* External assembly stubs */
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

extern void isr32(void);
extern void isr33(void);
extern void isr34(void);
extern void isr35(void);
extern void isr36(void);
extern void isr37(void);
extern void isr38(void);
extern void isr39(void);
extern void isr40(void);
extern void isr41(void);
extern void isr42(void);
extern void isr43(void);
extern void isr44(void);
extern void isr45(void);
extern void isr46(void);
extern void isr47(void);

extern void isr128(void);
extern void isr255(void);

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low  = (uint16_t)(base & 0xFFFF);
    idt[num].selector    = sel;
    idt[num].ist         = 0;
    idt[num].type_attr   = flags;
    idt[num].offset_mid  = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].offset_high = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    idt[num].zero        = 0;
}

void register_interrupt_handler(uint8_t n, isr_handler_t handler) {
    interrupt_handlers[n] = handler;
}

/* Remap legacy 8259 PIC so IRQs 0-15 do not conflict with CPU exceptions 0-31 */
static void pic_remap(void) {
    /* ICW1: start init sequence in cascade mode */
    outb(0x20, 0x11);
    outb(0xA0, 0x11);

    /* ICW2: master PIC vector offset 32 (0x20), slave 40 (0x28) */
    outb(0x21, 0x20);
    outb(0xA1, 0x28);

    /* ICW3: tell master PIC slave is at IRQ2 (0000 0100b), slave cascade identity 2 */
    outb(0x21, 0x04);
    outb(0xA1, 0x02);

    /* ICW4: 8086 mode */
    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    /* Mask all interrupts initially (0xFF) */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

static const char *exception_names[] = {
    "Division By Zero",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

static void print_diag(const char *s) {
    serial_puts(s);
    console_puts(s);
}

static void print_diag_hex(uint64_t val) {
    serial_puthex(val);
    const char *hex = "0123456789ABCDEF";
    console_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        console_putc(hex[(val >> i) & 0xF]);
    }
}

void isr_dispatch(struct interrupt_frame *frame) {
    if (frame->int_no < 32) {
        exc_record(frame);
        print_diag("\n[CPU EXCEPTION] ");
        print_diag(exception_names[frame->int_no]);
        print_diag(" (Vector ");
        print_diag_hex(frame->int_no);
        print_diag(")\n");

        print_diag("  RIP: "); print_diag_hex(frame->rip);
        print_diag("  CS: ");  print_diag_hex(frame->cs);
        print_diag("  RFLAGS: "); print_diag_hex(frame->rflags);
        print_diag("\n");

        print_diag("  RSP: "); print_diag_hex(frame->rsp);
        print_diag("  SS: ");  print_diag_hex(frame->ss);
        print_diag("  ERR: "); print_diag_hex(frame->err_code);
        print_diag("\n");

        if (frame->int_no == 14) {
            uint64_t cr2, cr3;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            print_diag("  CR2 (Faulting Address): "); print_diag_hex(cr2);
            print_diag("\n");

            /* A kernel-mode write onto a present read-only page and a write
             * onto a missing page look identical in the register dump, so
             * walk the live table here: CR3 vs the process's PML4 shows which
             * address space was actually current, and the entry chain shows
             * whether the page was absent, read-only, or a huge mapping. */
            print_diag("  CR3: "); print_diag_hex(cr3 & PAGE_ADDR_MASK);
            if (current_process) {
                print_diag("  proc PML4: ");
                print_diag_hex((uint64_t)current_process->pml4);
                print_diag("  proc: ");
                print_diag(current_process->name);
            }
            print_diag("\n");

            uint64_t hhdm = vmm_get_hhdm_offset();
            uint64_t *pml4 = (uint64_t *)(hhdm + (cr3 & PAGE_ADDR_MASK));
            uint64_t e4 = pml4[(cr2 >> 39) & 0x1FF];
            print_diag("  PML4E: "); print_diag_hex(e4);
            if (e4 & PAGE_PRESENT) {
                uint64_t *pdpt = (uint64_t *)(hhdm + (e4 & PAGE_ADDR_MASK));
                uint64_t e3 = pdpt[(cr2 >> 30) & 0x1FF];
                print_diag("  PDPTE: "); print_diag_hex(e3);
                if ((e3 & PAGE_PRESENT) && !(e3 & PAGE_HUGE)) {
                    uint64_t *pd = (uint64_t *)(hhdm + (e3 & PAGE_ADDR_MASK));
                    uint64_t e2 = pd[(cr2 >> 21) & 0x1FF];
                    print_diag("  PDE: "); print_diag_hex(e2);
                    if ((e2 & PAGE_PRESENT) && !(e2 & PAGE_HUGE)) {
                        uint64_t *pt = (uint64_t *)(hhdm + (e2 & PAGE_ADDR_MASK));
                        print_diag("  PTE: "); print_diag_hex(pt[(cr2 >> 12) & 0x1FF]);
                    }
                }
            }
            print_diag("\n");
        }

        print_diag("  RAX: "); print_diag_hex(frame->rax);
        print_diag("  RBX: "); print_diag_hex(frame->rbx);
        print_diag("  RCX: "); print_diag_hex(frame->rcx);
        print_diag("  RDX: "); print_diag_hex(frame->rdx);
        print_diag("\n");

        print_diag("  RSI: "); print_diag_hex(frame->rsi);
        print_diag("  RDI: "); print_diag_hex(frame->rdi);
        print_diag("  RBP: "); print_diag_hex(frame->rbp);
        print_diag("\n");

        print_diag("[PANIC] System halted due to unhandled CPU exception.\n");

        /* Ring-3 faults kill only the offending process (Unix semantics:
         * exit status 128+signal), then resume its parent or the shell.
         * Ring-0 faults still halt the machine. */
        if ((frame->cs & 3) == 3 && current_process) {
            int sig = 11; /* SIGSEGV */
            if (frame->int_no == 0) sig = 8;   /* SIGFPE */
            else if (frame->int_no == 6) sig = 4; /* SIGILL */
            print_diag("[PROC] Killed by signal ");
            print_diag_hex((uint64_t)sig);
            print_diag("\n");
            process_exit(128 + sig);
            return; /* unreachable */
        }

        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
    }

    /* Spurious LAPIC vector needs no EOI */
    if (frame->int_no == 255) return;

    /* Check if an IRQ handler was registered */
    if (interrupt_handlers[frame->int_no]) {
        interrupt_handlers[frame->int_no](frame);
    }

    /* Send EOI if hardware IRQ (32-47) */
    if (frame->int_no >= 32 && frame->int_no < 48) {
        irq_counts[frame->int_no - 32]++;
        apic_eoi();
        if (frame->int_no >= 40) {
            outb(0xA0, 0x20); /* Slave PIC EOI */
        }
        outb(0x20, 0x20);     /* Master PIC EOI */
    }
}

void idt_init(void) {
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;

    for (int i = 0; i < 256; i++) {
        interrupt_handlers[i] = NULL;
    }

    /* CPU Exceptions: Present (0x80), Ring 0 (0x00), 64-bit Interrupt Gate (0x0E) -> 0x8E */
    idt_set_gate(0,  (uint64_t)isr0,  GDT_SEG_KCODE, 0x8E);
    idt_set_gate(1,  (uint64_t)isr1,  GDT_SEG_KCODE, 0x8E);
    idt_set_gate(2,  (uint64_t)isr2,  GDT_SEG_KCODE, 0x8E);
    idt_set_gate(3,  (uint64_t)isr3,  GDT_SEG_KCODE, 0x8E);
    idt_set_gate(4,  (uint64_t)isr4,  GDT_SEG_KCODE, 0x8E);
    idt_set_gate(5,  (uint64_t)isr5,  GDT_SEG_KCODE, 0x8E);
    idt_set_gate(6,  (uint64_t)isr6,  GDT_SEG_KCODE, 0x8E);
    idt_set_gate(7,  (uint64_t)isr7,  GDT_SEG_KCODE, 0x8E);
    idt_set_gate(8,  (uint64_t)isr8,  GDT_SEG_KCODE, 0x8E);
    idt_set_gate(9,  (uint64_t)isr9,  GDT_SEG_KCODE, 0x8E);
    idt_set_gate(10, (uint64_t)isr10, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(11, (uint64_t)isr11, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(12, (uint64_t)isr12, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(13, (uint64_t)isr13, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(14, (uint64_t)isr14, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(15, (uint64_t)isr15, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(16, (uint64_t)isr16, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(17, (uint64_t)isr17, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(18, (uint64_t)isr18, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(19, (uint64_t)isr19, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(20, (uint64_t)isr20, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(21, (uint64_t)isr21, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(22, (uint64_t)isr22, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(23, (uint64_t)isr23, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(24, (uint64_t)isr24, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(25, (uint64_t)isr25, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(26, (uint64_t)isr26, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(27, (uint64_t)isr27, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(28, (uint64_t)isr28, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(29, (uint64_t)isr29, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(30, (uint64_t)isr30, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(31, (uint64_t)isr31, GDT_SEG_KCODE, 0x8E);

    /* Remap PIC to 32..47 */
    pic_remap();

    /* Hardware IRQs (32..47) */
    idt_set_gate(32, (uint64_t)isr32, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(33, (uint64_t)isr33, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(34, (uint64_t)isr34, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(35, (uint64_t)isr35, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(36, (uint64_t)isr36, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(37, (uint64_t)isr37, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(38, (uint64_t)isr38, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(39, (uint64_t)isr39, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(40, (uint64_t)isr40, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(41, (uint64_t)isr41, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(42, (uint64_t)isr42, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(43, (uint64_t)isr43, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(44, (uint64_t)isr44, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(45, (uint64_t)isr45, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(46, (uint64_t)isr46, GDT_SEG_KCODE, 0x8E);
    idt_set_gate(47, (uint64_t)isr47, GDT_SEG_KCODE, 0x8E);

    /* Syscall gate (vector 0x80): DPL = 3 (0xEE) so user space ring 3 can invoke it */
    idt_set_gate(128, (uint64_t)isr128, GDT_SEG_KCODE, 0xEE);

    /* LAPIC spurious (0xFF): present, no EOI in dispatch */
    idt_set_gate(255, (uint64_t)isr255, GDT_SEG_KCODE, 0x8E);

    idt_load(&idtr);
    serial_puts("[IDT] Loaded (256 gates initialized, PIC remapped to 0x20/0x28)\n");
}
