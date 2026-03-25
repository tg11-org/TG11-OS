/**
 * Copyright (C) 2026 TG11
 *
 * Cooperative kernel task scheduler and user-mode execution.
 *
 * Tasks are kernel threads sharing the kernel address space.  Switching is
 * cooperative only — a task must call task_yield() to give up the CPU.
 * User-mode (ring 3) ELF programs are launched by the current kernel task
 * via task_exec_user(); that call blocks until the user program calls
 * the SYS_EXIT syscall.
 */
#ifndef TG11_TASK_H
#define TG11_TASK_H

#define TASK_MAX         8
#define TASK_STACK_PAGES 8        /* pages per kernel task stack = 32 KB */

#define TASK_STATE_READY   0
#define TASK_STATE_RUNNING 1
#define TASK_STATE_ZOMBIE  2

typedef struct task {
    /* CRITICAL: rsp must remain at offset 0 — task_switch.s reads [rdi+0] */
    unsigned long  rsp;
    void          *stack_base;    /* bottom of virt_alloc'd stack; NULL for task 0 */
    unsigned int   id;
    int            state;
    void         (*entry)(void *);
    void          *arg;
    char           name[24];
    struct task   *next;          /* circular linked list for round-robin */
} task_t;

/* Initialise the scheduler; call once from kernel_main before enabling IRQs. */
void  task_init(void);

/* Create a new kernel task.  Returns task ID on success, -1 on failure.   */
int   task_create(const char *name, void (*entry)(void *), void *arg);

/* Cooperatively yield the CPU to the next ready task.                      */
void  task_yield(void);

/* Terminate the calling task.  For a user-mode task the calling kernel
 * task resumes.  For a kernel task the task is marked ZOMBIE and the
 * scheduler picks the next ready task.                                      */
void  task_exit(void);

/* Return a pointer to the currently running task.                           */
task_t *task_current(void);

/* Print a summary table of all tasks to the terminal.                       */
void  task_print_list(void);

/* Kill a task by ID (except the kernel shell task). Returns 0 on success. */
int   task_kill(unsigned int id);
int   task_kill_all(void);

/* Run an already-loaded ELF user program in ring 3.  Blocks until the user
 * program exits via SYS_EXIT.  The ELF's pages must already be mapped with
 * PAGE_FLAG_USER before this is called.                                      */
void  task_exec_user(unsigned long entry, unsigned long user_rsp);

/* Called from the CPU exception handler when a ring-3 fault occurs.
 * Returns control to the blocked task_exec_user() call.                     */
void  task_user_fault_exit(void);

/* ── Assembly helpers (not for direct C use) ── */
void  task_switch(task_t *from, task_t *to);
void  task_save_and_enter_user(unsigned long entry, unsigned long user_rsp,
                                unsigned long *saved_rsp);
void  task_restore_kernel(unsigned long saved_rsp);

#endif /* TG11_TASK_H */
