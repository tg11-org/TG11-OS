/*
 * Copyright (C) 2026 TG11
 *
 * Low-level SYSCALL entry point.
 *
 * Register state when syscall_entry is reached (Intel ABI for SYSCALL):
 *   rax = syscall number
 *   rdi = argument 1
 *   rsi = argument 2
 *   rcx = user RIP (saved by CPU for SYSRETQ)
 *   r11 = user RFLAGS (saved by CPU for SYSRETQ)
 *   rsp = user stack pointer (NOT changed by SYSCALL)
 *
 * We immediately save user RSP, load the kernel RSP, then shuffle
 * arguments into the C calling convention before calling syscall_dispatch.
 */
.intel_syntax noprefix
.section .text

.global syscall_entry
.extern syscall_dispatch
syscall_entry:
    /* Save user RSP; load kernel RSP atomically w.r.t. single-CPU re-use */
    mov  [syscall_user_rsp_slot], rsp
    mov  rsp, [syscall_kernel_rsp_slot]

    /* Preserve user RIP/RFLAGS across the C call */
    push r11           /* user RFLAGS */
    push rcx           /* user RIP    */

    /*
     * Shuffle to C calling convention:
     *   syscall_dispatch(unsigned long num, unsigned long a1, unsigned long a2)
     *                    rdi              , rsi              , rdx
     *
     * On entry: rax=num, rdi=a1, rsi=a2
     */
    mov  r10, rdi      /* r10 = original a1 (rdi is about to be overwritten) */
    mov  rdi, rax      /* rdi = num   */
    mov  rax, rsi      /* rax = a2 (temp save before we clobber rsi) */
    mov  rsi, r10      /* rsi = a1   */
    mov  rdx, rax      /* rdx = a2   */
    call syscall_dispatch

    /* Restore user context and return to ring 3 */
    pop  rcx           /* user RIP    */
    pop  r11           /* user RFLAGS */
    mov  rsp, [syscall_user_rsp_slot]
    sysretq

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
