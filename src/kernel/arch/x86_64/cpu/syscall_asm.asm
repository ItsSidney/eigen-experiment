; Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
; All rights reserved.
;
; syscall_asm.asm - Low-level x86_64 fast syscall entry and MSR configuration.

bits 64

global syscall_enable_msrs
global syscall_entry
global current_syscall_frame
global current_kstack_top
extern syscall_dispatch

section .bss
align 16
kernel_syscall_stack:
    resb 16384
kernel_syscall_stack_top:

user_rsp_save:
    resq 1

current_syscall_frame:
    resq 1

section .data
align 8
current_kstack_top:
    dq kernel_syscall_stack_top

section .text

; void syscall_enable_msrs(void)
syscall_enable_msrs:
    ; 1. Enable SCE (Syscall Enable) in IA32_EFER (0xC0000080)
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1               ; bit 0 = SCE
    wrmsr

    ; 2. Configure STAR MSR (0xC0000081)
    ; [47:32] = Kernel CS (0x08), Kernel SS is implicitly 0x10
    ; [63:48] = User Base Selector (0x10).
    ; sysret loads SS from STAR[63:48] + 8 = 0x18 (| 3 = 0x1B)
    ; sysret loads CS from STAR[63:48] + 16 = 0x20 (| 3 = 0x23)
    mov ecx, 0xC0000081
    xor eax, eax
    mov edx, 0x00100008     ; high dword: user 0x10, low dword: kernel 0x08
    wrmsr

    ; 3. Configure LSTAR MSR (0xC0000082) - Syscall Target RIP
    mov ecx, 0xC0000082
    lea rax, [rel syscall_entry]
    mov rdx, rax
    shr rdx, 32
    wrmsr

    ; 4. Configure SFMASK MSR (0xC0000084) - RFLAGS mask (mask out IF=0x200, DF=0x400)
    mov ecx, 0xC0000084
    mov eax, 0x00000200     ; Clear IF (interrupts disabled on entry)
    xor edx, edx
    wrmsr

    ret

; Syscall Entry Point (invoked by 'syscall' instruction in Ring 3)
align 16
syscall_entry:
    ; Save user RSP and switch to kernel syscall stack
    mov [rel user_rsp_save], rsp
    mov rsp, [rel current_kstack_top]

    ; Re-enable interrupts so the PIT timer IRQ fires during blocking syscalls
    ; (SFMASK cleared IF on entry; we need ticks to advance in net/sleep loops)
    sti

    ; Push all user registers to protect userland state across C calls
    push qword [rel user_rsp_save]  ; 1: user RSP
    push r11                        ; 2: user RFLAGS
    push rcx                        ; 3: user RIP
    push r15                        ; 4
    push r14                        ; 5
    push r13                        ; 6
    push r12                        ; 7
    push rbp                        ; 8
    push rbx                        ; 9
    push r10                        ; 10
    push r9                         ; 11
    push r8                         ; 12
    push rdx                        ; 13
    push rsi                        ; 14
    push rdi                        ; 15

    mov [rel current_syscall_frame], rsp

    ; Set up arguments for syscall_dispatch(num, a1, a2, a3, a4, a5, a6)
    ; In Linux x86_64 ABI:
    ; User passed: rax=num, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6
    ; System V C calling convention needs:
    ; rdi=num, rsi=a1, rdx=a2, rcx=a3, r8=a4, r9=a5, [rsp]=a6
    ;
    ; 15 quadwords pushed = 120 bytes.
    ; Pushing r9 (a6) adds 8 bytes = 128 bytes (16-byte aligned before call).
    push r9             ; 7th C arg: a6
    mov r9, r8          ; user a5 -> r9 (6th C arg)
    mov r8, r10         ; user a4 -> r8 (5th C arg)
    mov rcx, rdx        ; user a3 -> rcx (4th C arg)
    mov rdx, rsi        ; user a2 -> rdx (3rd C arg)
    mov rsi, rdi        ; user a1 -> rsi (2nd C arg)
    mov rdi, rax        ; user num -> rdi (1st C arg)

    cld
    call syscall_dispatch
    add rsp, 8          ; clean up 7th argument (a6) from stack

    ; Return value from syscall_dispatch is in RAX

    ; Disable interrupts before restoring user registers and sysret
    ; (sysret restores RFLAGS from r11, which had IF set in user mode —
    ;  IF will be re-enabled atomically as part of sysret itself)
    cli

    ; Restore all user registers (RAX preserved with return value)
    pop rdi                         ; 15
    pop rsi                         ; 14
    pop rdx                         ; 13
    pop r8                          ; 12
    pop r9                          ; 11
    pop r10                         ; 10
    pop rbx                         ; 9
    pop rbp                         ; 8
    pop r12                         ; 7
    pop r13                         ; 6
    pop r14                         ; 5
    pop r15                         ; 4
    pop rcx                         ; 3: user RIP
    pop r11                         ; 2: user RFLAGS
    pop rsp                         ; 1: user RSP

    ; Return to user space Ring 3 (64-bit mode)
    o64 sysret

; void resume_userspace_from_frame(const uint64_t *frame)
; rdi = pointer to 15-quadword saved syscall frame (in forked child PCB).
;
; Layout note: syscall_entry pushes registers downward, so the FIRST push
; (user RSP) sits at the HIGHEST address. Memory layout, low -> high:
;   [ 0]=rdi [ 1]=rsi [ 2]=rdx [ 3]=r8  [ 4]=r9  [ 5]=r10 [ 6]=rbx
;   [ 7]=rbp [ 8]=r12 [ 9]=r13 [10]=r14 [11]=r15 [12]=rip [13]=rflags
;   [14]=user_rsp
; Enters ring 3 at the point of the fork/clone syscall with RAX = 0 (child).
global resume_userspace_from_frame
extern current_syscall_frame
resume_userspace_from_frame:
    mov [rel current_syscall_frame], rdi
    mov rax, [rdi + 112]    ; user RSP   (frame[14])
    mov r11, [rdi + 104]    ; user RFLAGS(frame[13])
    mov rcx, [rdi + 96]     ; user RIP   (frame[12])
    mov r15, [rdi + 88]     ; frame[11]
    mov r14, [rdi + 80]     ; frame[10]
    mov r13, [rdi + 72]     ; frame[9]
    mov r12, [rdi + 64]     ; frame[8]
    mov rbp, [rdi + 56]     ; frame[7]
    mov rbx, [rdi + 48]     ; frame[6]
    mov r10, [rdi + 40]     ; frame[5]
    mov r9,  [rdi + 32]     ; frame[4]
    mov r8,  [rdi + 24]     ; frame[3]
    mov rdx, [rdi + 16]     ; frame[2]
    mov rsi, [rdi + 8]      ; frame[1]
    mov rdi, [rdi]          ; frame[0]
    mov rsp, rax            ; switch to user stack
    xor eax, eax            ; fork()/clone() returns 0 in the child
    o64 sysret

section .note.GNU-stack noalloc noexec nowrite progbits
