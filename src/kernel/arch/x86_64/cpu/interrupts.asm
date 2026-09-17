; Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
; All rights reserved.
;
; interrupts.asm - x86_64 low-level interrupt service routine entry points.

bits 64

extern isr_dispatch

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0        ; Dummy error code
    push qword %1       ; Interrupt number
    jmp common_isr_stub
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1       ; Interrupt number (error code already pushed by CPU)
    jmp common_isr_stub
%endmacro

; 0-31: CPU Exceptions
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; 32-47: Hardware IRQs (Master and Slave PIC remapped)
%assign i 32
%rep 16
ISR_NOERR i
%assign i i+1
%endrep

; 128: Syscall / Software Interrupt (0x80)
ISR_NOERR 128

; 255: LAPIC spurious vector (no EOI)
ISR_NOERR 255

common_isr_stub:
    ; Save all general purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; First argument (RDI) in SysV ABI is pointer to stack frame
    mov rdi, rsp
    cld
    call isr_dispatch

    ; Restore registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Pop interrupt number and error code
    add rsp, 16

    iretq
