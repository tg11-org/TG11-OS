.intel_syntax noprefix
.section .text

/*
 * void task_switch(task_t *from, task_t *to)
 *
 * rdi = from  (offset 0 in task_t is rsp field)
 * rsi = to
 *
 * Saves callee-saved registers on the current stack, stores the resulting
 * rsp in from->rsp, then loads rsp from to->rsp and restores registers.
 * On the very first switch into a new task, ret falls into task_trampoline.
 */
.global task_switch
task_switch:
    pushfq
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov  [rdi], rsp
    mov  rsp, [rsi]
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbp
    pop  rbx
    popfq
    ret


/*
 * void task_save_and_enter_user(unsigned long entry,
 *                               unsigned long user_rsp,
 *                               unsigned long user_arg0,
 *                               unsigned long *saved_rsp,
 *                               unsigned long argv1_ptr)
 *
 * rdi = user RIP
 * rsi = user RSP
 * rdx = user arg0 (restored into user RDI) — typically argc
 * rcx = pointer where current kernel RSP is saved
 * r8 = argv[1] pointer (pre-computed in kernel from stack alias)
 *
 * Saves the current kernel stack pointer into *saved_rsp, builds an iretq
 * frame on the stack, and transitions to ring 3.  Never returns directly;
 * task_restore_kernel() is called (via SYS_EXIT) to resume the caller.
 */
.global task_save_and_enter_user
task_save_and_enter_user:
    mov  r10, rdi            /* preserve user RIP */
    mov  r11, rsi            /* preserve user RSP */

    /* Save kernel RSP (which currently points at the return address that the
       'call' instruction pushed, so task_restore_kernel / ret will go back
       to the instruction after the call in task_exec_user). */
    mov  [rcx], rsp

    /* Set initial user RDI from caller-provided arg0 (argc). */
    mov  rdi, rdx
    /* Set initial user RSI from pre-computed argv[1] (kernel-computed). */
    mov  rsi, r8

    /* Build iretq frame: SS, RSP, RFLAGS, CS, RIP */
    mov  rax, 0x23           /* user SS selector (GDT[4], RPL=3) */
    push rax
    push r11                 /* user RSP */
    pushfq
    pop  rax
    or   rax, 0x200          /* ensure Interrupt Enable is set */
    and  rax, ~0x3000        /* clear IOPL bits */
    push rax                 /* user RFLAGS */
    mov  rax, 0x2B           /* user CS selector (GDT[5], RPL=3) */
    push rax
    push r10                 /* user RIP */

    iretq                    /* transition to ring 3 — never returns here */


/*
 * void task_restore_kernel(unsigned long saved_rsp)
 *
 * rdi = saved kernel RSP (from task_save_and_enter_user)
 *
 * Restores the kernel stack pointer and executes ret, which returns to
 * task_exec_user as if task_save_and_enter_user had returned normally.
 */
.global task_restore_kernel
task_restore_kernel:
    mov  rsp, rdi
    ret
