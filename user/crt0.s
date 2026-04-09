/*
 * Copyright (C) 2026 TG11
 *
 * C runtime startup for TG11-OS user programs.
 *
 * Stack layout at entry (SysV AMD64 ABI):
 *   [RSP]     = argc
 *   [RSP+8]   = argv[0]
 *   [RSP+16]  = argv[1]
 *   ...
 *   [RSP+8*argc+8] = NULL (argv terminator)
 */
.intel_syntax noprefix
.section .text

.global _start
.extern main

_start:
    /* Clear frame pointer for clean stack traces */
    xor rbp, rbp

    /* argc is at [rsp], argv starts at rsp+8 */
    mov  rdi, [rsp]          /* rdi = argc */
    lea  rsi, [rsp + 8]      /* rsi = &argv[0] */

    /* Align stack to 16 bytes before call (ABI requirement) */
    and  rsp, -16

    call main

    /* exit(main return value) */
    mov  rdi, rax             /* exit code = main() return value */
    mov  rax, 60              /* SYS_EXIT */
    syscall

    /* Should never reach here */
    ud2
