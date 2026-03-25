/**
 * Copyright (C) 2026 TG11
 *
 * Cooperative task scheduler and user-mode execution support.
 */
#include "task.h"
#include "terminal.h"
#include "memory.h"
#include "arch.h"

/* ── Storage ─────────────────────────────────────────────────────── */

static task_t tasks[TASK_MAX];
static task_t *current_task;

/*
 * Kernel stack used during syscalls and for TSS RSP0 (interrupt
 * delivery from ring 3).  Placed in BSS so it starts zeroed.
 */
static unsigned long syscall_kstack[2048];
#define SYSCALL_KSTACK_TOP ((unsigned long)(syscall_kstack + 2048))

/*
 * When the current kernel task is executing a ring-3 user program,
 * this holds the kernel RSP saved by task_save_and_enter_user().
 * task_exit() (via SYS_EXIT) restores it so task_exec_user() returns.
 */
static unsigned long user_task_saved_rsp = 0;

/* Declared in arch/x86_64/syscall.s */
extern unsigned long syscall_kernel_rsp_slot;

/* Declared in arch/x86_64/gdt.c */
void gdt_tss_set_rsp0(unsigned long rsp0);

static task_t *find_task_by_id(unsigned int id)
{
    unsigned int i;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].id == id) return &tasks[i];
    }
    return (void *)0;
}

static void unlink_task(task_t *t)
{
    task_t *p;
    if (t == (void *)0 || t == &tasks[0]) return;
    p = &tasks[0];
    do {
        if (p->next == t) {
            p->next = t->next;
            return;
        }
        p = p->next;
    } while (p != &tasks[0]);
}

static void reclaim_task_slot(task_t *t)
{
    if (t == (void *)0 || t == &tasks[0]) return;
    if (t->stack_base != (void *)0) {
        virt_free_pages(t->stack_base, TASK_STACK_PAGES);
    }
    t->rsp        = 0;
    t->stack_base = (void *)0;
    t->id         = 0;
    t->state      = TASK_STATE_ZOMBIE;
    t->entry      = (void *)0;
    t->arg        = (void *)0;
    t->name[0]    = '\0';
    t->next       = (void *)0;
}

/* ── Internal helpers ─────────────────────────────────────────────── */

static void copy_name(char *dst, const char *src, unsigned long max)
{
    unsigned long i = 0;
    while (src[i] != '\0' && i + 1 < max) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void write_uint(unsigned int v)
{
    char buf[12];
    unsigned int i = 0;
    if (v == 0) { terminal_write("0"); return; }
    while (v > 0) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    /* reverse */
    {
        unsigned int lo = 0, hi = i - 1;
        while (lo < hi) { char t = buf[lo]; buf[lo] = buf[hi]; buf[hi] = t; lo++; hi--; }
    }
    buf[i] = '\0';
    terminal_write(buf);
}

/*
 * Each new task's first call into the scheduler pops six saved
 * registers from its stack and then rets into this trampoline.
 * We then call the task's entry function and mark it done.
 */
static void task_trampoline(void)
{
    current_task->entry(current_task->arg);
    task_exit();
}

/*
 * Switch to the next READY task in the circular list.
 * The caller has already updated current_task->state and moved
 * current_task if needed.
 */
static void do_switch_to_next(task_t *old)
{
    task_t *next = old->next;
    while (next != old && next->state != TASK_STATE_READY)
        next = next->next;

    if (next == old) {
        /* If old is still runnable, just keep running it. */
        if (old->state == TASK_STATE_READY || old->state == TASK_STATE_RUNNING) {
            old->state = TASK_STATE_RUNNING;
            current_task = old;
            return;
        }
        /* No runnable tasks left. */
        for (;;) arch_halt();
    }

    next->state = TASK_STATE_RUNNING;
    current_task = next;
    task_switch(old, next);
}

/* ── Public API ───────────────────────────────────────────────────── */

void task_init(void)
{
    unsigned int i;
    const char *n = "kernel";

    for (i = 0; i < TASK_MAX; i++) {
        tasks[i].rsp        = 0;
        tasks[i].stack_base = (void *)0;
        tasks[i].id         = 0;
        tasks[i].state      = TASK_STATE_ZOMBIE;
        tasks[i].entry      = (void *)0;
        tasks[i].arg        = (void *)0;
        tasks[i].name[0]    = '\0';
        tasks[i].next       = (void *)0;
    }

    /* task 0 — the main kernel thread (uses the existing kernel stack) */
    tasks[0].id    = 1;
    tasks[0].state = TASK_STATE_RUNNING;
    copy_name(tasks[0].name, n, sizeof(tasks[0].name));
    tasks[0].next  = &tasks[0]; /* circular: only member */

    current_task = &tasks[0];
}

int task_create(const char *name, void (*entry)(void *), void *arg)
{
    static unsigned int next_id = 2; /* id 1 is task0 */
    unsigned int i;
    task_t *t = (void *)0;
    unsigned long *sp;
    unsigned long stack_top;

    /* Find a free slot (id == 0 means unused). */
    for (i = 1; i < TASK_MAX; i++) {
        if (tasks[i].id == 0) { t = &tasks[i]; break; }
    }
    /* Reclaim a zombie slot if no unused slot is available. */
    if (t == (void *)0) {
        for (i = 1; i < TASK_MAX; i++) {
            if (tasks[i].state == TASK_STATE_ZOMBIE && tasks[i].id != 0) {
                unlink_task(&tasks[i]);
                reclaim_task_slot(&tasks[i]);
                t = &tasks[i];
                break;
            }
        }
    }
    if (t == (void *)0) return -1;

    t->stack_base = virt_alloc_pages(TASK_STACK_PAGES);
    if (t->stack_base == (void *)0) return -1;

    stack_top = (unsigned long)t->stack_base + (unsigned long)(TASK_STACK_PAGES * 4096);

    /*
     * Build the initial context so task_switch() can pop six
     * callee-saved registers and ret into task_trampoline.
     *
     *  stack_top - 8  : alignment pad (skipped; not popped)
     *  stack_top - 16 : task_trampoline  ← return address for ret
     *  stack_top - 24 : 0  (rbx)
     *  stack_top - 32 : 0  (rbp)
     *  stack_top - 40 : 0  (r12)
     *  stack_top - 48 : 0  (r13)
     *  stack_top - 56 : 0  (r14)
     *  stack_top - 64 : 0  (r15)  ← initial rsp
     *
     * After the six pops and ret, RSP = stack_top - 8, which is
     * 8 (mod 16) with a page-aligned stack_top — matching the ABI
     * requirement at function entry.
     */
    sp = (unsigned long *)stack_top;
    sp--;                                       /* alignment pad */
    *(--sp) = (unsigned long)task_trampoline;   /* return addr   */
    *(--sp) = 0; /* rbx */
    *(--sp) = 0; /* rbp */
    *(--sp) = 0; /* r12 */
    *(--sp) = 0; /* r13 */
    *(--sp) = 0; /* r14 */
    *(--sp) = 0; /* r15 */

    t->rsp   = (unsigned long)sp;
    t->id    = next_id++;
    t->state = TASK_STATE_READY;
    t->entry = entry;
    t->arg   = arg;
    copy_name(t->name, name != (void *)0 ? name : "task", sizeof(t->name));

    /* Insert after task0 so the round-robin visits new tasks quickly. */
    t->next          = tasks[0].next;
    tasks[0].next    = t;

    return (int)t->id;
}

void task_yield(void)
{
    task_t *old = current_task;
    old->state = TASK_STATE_READY;
    do_switch_to_next(old);
    /* When we are switched back to, state is already RUNNING (set by
       the task that woke us).  Restore our own RUNNING mark. */
    current_task->state = TASK_STATE_RUNNING;
}

void task_exit(void)
{
    if (user_task_saved_rsp != 0) {
        /*
         * A ring-3 user program is exiting via SYS_EXIT.
         * Restore the kernel stack saved before iretq; this returns
         * inside task_exec_user() right after the call to
         * task_save_and_enter_user().
         */
        unsigned long saved = user_task_saved_rsp;
        user_task_saved_rsp = 0;
        task_restore_kernel(saved);
        /* never reached */
    }

    /* Kernel task exiting — mark zombie and yield. */
    if (current_task == &tasks[0]) {
        /* Keep task0 alive; it's the shell/idle anchor. */
        return;
    }
    current_task->state = TASK_STATE_ZOMBIE;
    do_switch_to_next(current_task);
    for (;;) arch_halt(); /* should never reach here */
}

task_t *task_current(void)
{
    return current_task;
}

void task_print_list(void)
{
    unsigned int i;
    terminal_write_line("  ID  State    Name");
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].id == 0) continue;
        terminal_write("  ");
        write_uint(tasks[i].id);
        terminal_write("   ");
        if (tasks[i].state == TASK_STATE_RUNNING)     terminal_write("RUNNING  ");
        else if (tasks[i].state == TASK_STATE_READY)  terminal_write("READY    ");
        else                                           terminal_write("ZOMBIE   ");
        terminal_write_line(tasks[i].name);
    }
}

int task_kill(unsigned int id)
{
    task_t *t = find_task_by_id(id);
    if (t == (void *)0) return -1;
    if (t == &tasks[0]) return -1;
    if (t == current_task) return -1;

    unlink_task(t);
    reclaim_task_slot(t);
    return 0;
}

int task_kill_all(void)
{
    unsigned int i;
    int killed = 0;
    for (i = 1; i < TASK_MAX; i++) {
        if (&tasks[i] == current_task) continue;
        if (tasks[i].id == 0) continue;
        unlink_task(&tasks[i]);
        reclaim_task_slot(&tasks[i]);
        killed++;
    }
    return killed;
}

/*
 * Called from task.h/idt.c when the user program causes a CPU exception.
 * Recovers back to the kernel by restoring the saved kernel RSP.
 */
void task_user_fault_exit(void)
{
    if (user_task_saved_rsp != 0) {
        unsigned long saved = user_task_saved_rsp;
        user_task_saved_rsp = 0;
        task_restore_kernel(saved);
        /* never reached */
    }
    /* No active user task — just halt. */
    for (;;) arch_halt();
}

void task_exec_user(unsigned long entry, unsigned long user_rsp)
{
    /*
     * Point the TSS RSP0 and the syscall kernel stack at the top of
     * our dedicated syscall/interrupt kernel stack.  Any ring-3 exception
     * or SYSCALL will use this stack in kernel mode.
     */
    syscall_kernel_rsp_slot = SYSCALL_KSTACK_TOP;
    gdt_tss_set_rsp0(SYSCALL_KSTACK_TOP);

    /*
     * Save the current kernel RSP into user_task_saved_rsp and
     * iretq into ring 3.  When the user program calls SYS_EXIT,
     * task_exit() → task_restore_kernel() brings RSP back here.
     */
    task_save_and_enter_user(entry, user_rsp, &user_task_saved_rsp);

    /* Execution resumes here after SYS_EXIT or a user-mode fault. */
    user_task_saved_rsp = 0;

    /* Clear the TSS RSP0 so stale use is obvious. */
    syscall_kernel_rsp_slot = 0;
    gdt_tss_set_rsp0(0);
}
