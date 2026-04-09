/*
 * Copyright (C) 2026 TG11
 *
 * Low-level SYSCALL entry point.
 *
 * Register state when syscall_entry is reached (Intel ABI for SYSCALL):
 *   rax = syscall number
 *   rdi = argument 1
 *   rsi = argument 2
 *   rdx = argument 3
 *   r10 = argument 4
 *   rcx = user RIP (saved by CPU for SYSRETQ)
 *   r11 = user RFLAGS (saved by CPU for SYSRETQ)
 *   rsp = user stack pointer (NOT changed by SYSCALL)
 *
 * All user-mode GPRs are saved on the kernel stack at entry and restored
 * before returning to ring 3 via IRETQ.  The return value from
 * syscall_dispatch is delivered in rax; all other registers are preserved
 * exactly as the user left them.
 */
.intel_syntax noprefix
.section .text

.global syscall_entry
.extern syscall_dispatch
syscall_entry:
    /* Save user RSP; load kernel RSP */
    mov  [syscall_user_rsp_slot], rsp
    mov  rsp, [syscall_kernel_rsp_slot]

    /* Preserve ALL user-mode GPRs (rax slot will be overwritten with the
       return value before we pop it back). */
    push r11           /* [+104] user RFLAGS (saved by CPU) */
    push rcx           /* [+96]  user RIP    (saved by CPU) */
    push r15           /* [+88]  */
    push r14           /* [+80]  */
    push r13           /* [+72]  */
    push r12           /* [+64]  */
    push r10           /* [+56]  */
    push r9            /* [+48]  */
    push r8            /* [+40]  */
    push rbp           /* [+32]  */
    push rbx           /* [+24]  */
    push rdx           /* [+16]  user rdx / a3 */
    push rsi           /* [+8]   user rsi / a2 */
    push rdi           /* [+0]   user rdi / a1 */

    /*
     * Shuffle to C calling convention:
     *   syscall_dispatch(num, a1, a2, a3, a4)
     *       C ABI:  rdi   rsi  rdx  rcx  r8
     *
     * Source registers still hold their original user-mode values.
     */
    mov  r8,  r10      /* r8  = a4 (original r10) */
    mov  rcx, rdx      /* rcx = a3 (original rdx) */
    mov  rdx, rsi      /* rdx = a2 (original rsi) */
    mov  rsi, rdi      /* rsi = a1 (original rdi) */
    mov  rdi, rax      /* rdi = num               */

    call syscall_dispatch   /* return value in rax */

    /* Restore ALL user-mode GPRs — rax is intentionally NOT restored
       because it carries the syscall return value to user-space. */
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rbx
    pop  rbp
    pop  r8
    pop  r9
    pop  r10
    pop  r12
    pop  r13
    pop  r14
    pop  r15
    pop  rcx           /* user RIP    */
    pop  r11           /* user RFLAGS */

    /* Return to ring 3 via IRETQ.  Build the frame without clobbering
       any GPR so that rax keeps the syscall return value. */
    push 0x23                                   /* user SS  selector  */
    push QWORD PTR [syscall_user_rsp_slot]      /* user RSP           */
    push r11                                    /* user RFLAGS        */
    push 0x2B                                   /* user CS  selector  */
    push rcx                                    /* user RIP           */
    iretq

.section .bss

/* kernel RSP loaded at task_exec_user() time; used by both SYSCALL and
   interrupt delivery from ring 3 (TSS RSP0 is kept in sync). */
.global syscall_kernel_rsp_slot
syscall_kernel_rsp_slot:
    .quad 0

/* temporary storage for user RSP during a SYSCALL */
.global syscall_user_rsp_slot
syscall_user_rsp_slot:
    .quad 0
