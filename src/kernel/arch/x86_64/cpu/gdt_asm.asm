; Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
; All rights reserved.
;
; gdt_asm.asm - GDT load and IDT load stubs.
; Called from C after the GDT/IDT tables are set up in memory.

bits 64

global gdt_load
global idt_load
global tss_load

; void gdt_load(struct gdtr *gdtr, uint16_t kcode, uint16_t kdata)
; rdi = gdtr pointer, rsi = kcode selector, rdx = kdata selector
gdt_load:
    lgdt [rdi]
    ; Far return trick: push CS:RIP onto stack, then retfq to reload CS.
    push    rsi
    lea     rax, [rel .reload_cs]
    push    rax
    retfq
.reload_cs:
    ; Reload all data segment registers with kdata.
    mov     ax, dx
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    ret

; void idt_load(struct idtr *idtr)
; rdi = idtr pointer
idt_load:
    lidt [rdi]
    ret

; void tss_load(uint16_t selector)
; rdi = TSS segment selector
tss_load:
    mov     ax, di
    ltr     ax
    ret

; void jump_to_ring3(uint64_t entry, uint64_t user_rsp)
; rdi = user entry point, rsi = user stack pointer
global jump_to_ring3
jump_to_ring3:
    mov     ax, 0x1B        ; User data segment (UDATA = 0x18 | RPL 3)
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax

    push    qword 0x1B      ; SS: User Data (0x18 | 3)
    push    rsi             ; RSP: User stack pointer
    push    qword 0x202     ; RFLAGS: Interrupts enabled (IF = 1)
    push    qword 0x23      ; CS: User Code (0x20 | 3)
    push    rdi             ; RIP: User entry point

    ; Clear registers for clean user environment
    xor     rax, rax
    xor     rbx, rbx
    xor     rcx, rcx
    xor     rdx, rdx
    xor     rbp, rbp
    xor     r8,  r8
    xor     r9,  r9
    xor     r10, r10
    xor     r11, r11
    xor     r12, r12
    xor     r13, r13
    xor     r14, r14
    xor     r15, r15

    iretq

; void jump_to_ring3_fork(void *frame)
; rdi = pointer to struct syscall_frame
global jump_to_ring3_fork
jump_to_ring3_fork:
    mov     ax, 0x1B        ; User data segment (UDATA = 0x18 | RPL 3)
    mov     ds, ax
    mov     es, ax

    ; Push iretq stack frame
    push    qword 0x1B              ; SS: User Data
    push    qword [rdi + 112]       ; RSP: User stack pointer
    push    qword [rdi + 104]       ; RFLAGS: User rflags
    push    qword 0x23              ; CS: User Code
    push    qword [rdi + 96]        ; RIP: User instruction pointer

    ; Restore registers from frame
    mov     rsi, [rdi + 8]
    mov     rdx, [rdi + 16]
    mov     r8,  [rdi + 24]
    mov     r9,  [rdi + 32]
    mov     r10, [rdi + 40]
    mov     rbx, [rdi + 48]
    mov     rbp, [rdi + 56]
    mov     r12, [rdi + 64]
    mov     r13, [rdi + 72]
    mov     r14, [rdi + 80]
    mov     r15, [rdi + 88]

    ; Return 0 to child process in RAX
    xor     rax, rax

    ; Finally restore RDI
    mov     rdi, [rdi + 0]

    iretq

; int set_kernel_context(void *ctx)
; rdi = pointer to uint64_t ctx[8]
; Saves the CALLER's execution context so return_to_kernel_context can
; longjmp back to the instruction after the call. ctx[0] holds the caller's
; RSP (rsp+8 here, because the call pushed our return address); ctx[56]
; holds that return address.
global set_kernel_context
set_kernel_context:
    lea     rax, [rsp + 8]          ; caller's RSP (skip our return address)
    mov     [rdi + 0],  rax
    mov     [rdi + 8],  rbp
    mov     [rdi + 16], rbx
    mov     [rdi + 24], r12
    mov     [rdi + 32], r13
    mov     [rdi + 40], r14
    mov     [rdi + 48], r15
    mov     rax, [rsp]              ; our return address
    mov     [rdi + 56], rax
    xor     rax, rax                ; returns 0
    ret

; void return_to_kernel_context(void *ctx, int status)
; rdi = pointer to uint64_t ctx[8], rsi = return status (non-zero sentinel)
; Restores the context saved by set_kernel_context. RSP is the caller's RSP
; (before set_kernel_context's return address was pushed), so `ret` at the
; end pops the correct return address from the caller's stack.
global return_to_kernel_context
return_to_kernel_context:
    mov     rsp, [rdi + 0]
    mov     rbp, [rdi + 8]
    mov     rbx, [rdi + 16]
    mov     r12, [rdi + 24]
    mov     r13, [rdi + 32]
    mov     r14, [rdi + 40]
    mov     r15, [rdi + 48]
    mov     rax, rsi
    jmp     [rdi + 56]
