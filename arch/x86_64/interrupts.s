.intel_syntax noprefix

.section .text
.code64

.global arch_enable_interrupts
arch_enable_interrupts:
    sti
    ret

.global arch_disable_interrupts
arch_disable_interrupts:
    cli
    ret

.global arch_halt
arch_halt:
    hlt
    ret

.global arch_inb
arch_inb:
    xor eax, eax
    mov dx, di
    in al, dx
    ret

.global arch_outb
arch_outb:
    mov dx, di
    mov al, sil
    out dx, al
    ret

.global arch_outw
arch_outw:
    mov dx, di
    mov ax, si
    out dx, ax
    ret

.global arch_io_wait
arch_io_wait:
    xor eax, eax
    out 0x80, al
    ret

.global arch_lidt
arch_lidt:
    lidt [rdi]
    ret

.global exception_hang_stub
exception_hang_stub:
    cli

exception_hang_loop:
    hlt
    jmp exception_hang_loop

.global isr_ignore_stub
.extern isr_default_handler
isr_ignore_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12

.global irq12_stub
.extern mouse_interrupt_handler
irq12_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    call mouse_interrupt_handler
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

    push r13
    push r14
    push r15
    cld
    call isr_default_handler
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

.global irq1_stub
.extern keyboard_interrupt_handler
irq1_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    call keyboard_interrupt_handler
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

/* ---------------------------------------------------------------
 * CPU Exception stubs (vectors 0-31)
 *
 * Exceptions that the CPU does NOT push an error code for get a
 * fake zero pushed first so the stack layout is always:
 *   [RSP+0]  vector
 *   [RSP+8]  error_code (real or zero)
 *   [RSP+16] CPU iret frame (RIP, CS, RFLAGS, RSP, SS)
 *
 * Exceptions that DO push an error code: 8, 10-14, 17, 21, 30
 * --------------------------------------------------------------- */

.macro EXCEPTION_NO_ERROR vector
.global exception_stub_\vector
exception_stub_\vector:
    push 0
    push \vector
    jmp exception_common
.endm

.macro EXCEPTION_WITH_ERROR vector
.global exception_stub_\vector
exception_stub_\vector:
    push \vector
    jmp exception_common
.endm

EXCEPTION_NO_ERROR   0   /* #DE Divide Error              */
EXCEPTION_NO_ERROR   1   /* #DB Debug                     */
EXCEPTION_NO_ERROR   2   /* NMI                           */
EXCEPTION_NO_ERROR   3   /* #BP Breakpoint                */
EXCEPTION_NO_ERROR   4   /* #OF Overflow                  */
EXCEPTION_NO_ERROR   5   /* #BR BOUND Range Exceeded      */
EXCEPTION_NO_ERROR   6   /* #UD Invalid Opcode            */
EXCEPTION_NO_ERROR   7   /* #NM Device Not Available      */
EXCEPTION_WITH_ERROR 8   /* #DF Double Fault              */
EXCEPTION_NO_ERROR   9   /* Coprocessor Segment Overrun   */
EXCEPTION_WITH_ERROR 10  /* #TS Invalid TSS               */
EXCEPTION_WITH_ERROR 11  /* #NP Segment Not Present       */
EXCEPTION_WITH_ERROR 12  /* #SS Stack Fault               */
EXCEPTION_WITH_ERROR 13  /* #GP General Protection        */
EXCEPTION_WITH_ERROR 14  /* #PF Page Fault                */
EXCEPTION_NO_ERROR   15  /* Reserved                      */
EXCEPTION_NO_ERROR   16  /* #MF x87 FP Exception          */
EXCEPTION_WITH_ERROR 17  /* #AC Alignment Check           */
EXCEPTION_NO_ERROR   18  /* #MC Machine Check             */
EXCEPTION_NO_ERROR   19  /* #XF SIMD FP Exception         */
EXCEPTION_NO_ERROR   20  /* #VE Virtualization Exception  */
EXCEPTION_WITH_ERROR 21  /* #CP Control Protection        */
EXCEPTION_NO_ERROR   22  /* Reserved                      */
EXCEPTION_NO_ERROR   23  /* Reserved                      */
EXCEPTION_NO_ERROR   24  /* Reserved                      */
EXCEPTION_NO_ERROR   25  /* Reserved                      */
EXCEPTION_NO_ERROR   26  /* Reserved                      */
EXCEPTION_NO_ERROR   27  /* Reserved                      */
EXCEPTION_NO_ERROR   28  /* Reserved                      */
EXCEPTION_NO_ERROR   29  /* Reserved                      */
EXCEPTION_WITH_ERROR 30  /* #SX Security Exception        */
EXCEPTION_NO_ERROR   31  /* Reserved                      */

/* Common handler: save all GPRs, pass frame pointer to C */
.extern exception_handler
exception_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp        /* first arg = pointer to exception_frame */
    call exception_handler
exception_common_halt:
    cli
    hlt
    jmp exception_common_halt