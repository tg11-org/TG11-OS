.intel_syntax noprefix

.section .text
.code64
.global long_mode_start
.extern kernel_main
.extern mb2_info_ptr

long_mode_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, offset stack_top64
    mov edi, [mb2_info_ptr]
    call kernel_main

hang64:
    hlt
    jmp hang64

.section .bss
.align 16
stack_bottom64:
    .skip 16384
stack_top64: