/**
 * Copyright (C) 2026 TG11
 *
 * Cooperative task scheduler and user-mode execution support.
 */
#include "task.h"
#include "terminal.h"
#include "memory.h"
#include "arch.h"
#include "serial.h"

/* ── Storage ─────────────────────────────────────────────────────── */

static task_t tasks[TASK_MAX];
static task_t *current_task;

/* Forward declaration — defined further down after helpers. */
static void do_switch_to_next(task_t *old);
/* 0=default policy, 1=force protected, 2=force unprotected */
static unsigned char task_protect_mode[TASK_MAX];
static int task_event_log_on = 1;

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
static unsigned long user_heap_base = 0;
static unsigned long user_heap_limit = 0;
static unsigned long user_heap_brk = 0;

/*
 * Preemption control.  The timer tick handler calls task_preempt_tick()
 * which yields the CPU to the next ready task when safe to do so.
 *
 *  preempt_enabled       – master switch; toggled via task_set_preemption()
 *  preempt_disable_count – per-CPU nesting counter; >0 inhibits preemption
 */
static int preempt_enabled = 1;
static volatile int preempt_disable_count = 0;

static void brk_trace_fail(const char *reason,
    unsigned long req,
    unsigned long old_brk,
    unsigned long page)
{
    terminal_write("[brk] fail ");
    terminal_write(reason);
    terminal_write(" req=");
    terminal_write_hex64(req);
    terminal_write(" old=");
    terminal_write_hex64(old_brk);
    terminal_write(" page=");
    terminal_write_hex64(page);
    terminal_write(" base=");
    terminal_write_hex64(user_heap_base);
    terminal_write(" lim=");
    terminal_write_hex64(user_heap_limit);
    terminal_write("\n");
}

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

static int str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static unsigned int task_slot_index(const task_t *t)
{
    return (unsigned int)(t - tasks);
}

static void serial_write_uint(unsigned int v)
{
    char buf[12];
    unsigned int i = 0;
    if (v == 0) { serial_write("0"); return; }
    while (v > 0) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    {
        unsigned int lo = 0, hi = i - 1;
        while (lo < hi) { char t = buf[lo]; buf[lo] = buf[hi]; buf[hi] = t; lo++; hi--; }
    }
    buf[i] = '\0';
    serial_write(buf);
}

static void task_log_event(const char *op, const task_t *t)
{
    if (!task_event_log_on) return;
    if (op == (void *)0 || t == (void *)0) return;
    serial_write("[task] ");
    serial_write(op);
    serial_write(" id=");
    serial_write_uint(t->id);
    serial_write(" name=");
    serial_write(t->name[0] != '\0' ? t->name : "<none>");
    serial_write("\r\n");
}

static const char *task_class_name(const task_t *t)
{
    if (t == (void *)0) return "USER";
    /* Core infrastructure tasks are system-owned. */
    if (str_eq(t->name, "kernel") || str_eq(t->name, "shell") ||
        str_eq(t->name, "terminal") || str_eq(t->name, "serial"))
        return "SYS";
    return "USER";
}

static int is_protected_task(const task_t *t)
{
    unsigned int idx;
    if (t == (void *)0) return 0;
    idx = task_slot_index(t);
    if (idx < TASK_MAX)
    {
        if (task_protect_mode[idx] == 1) return 1;
        if (task_protect_mode[idx] == 2) return 0;
    }
    /* task0 is the kernel idle task and must never be removed/stopped. */
    if (t == &tasks[0]) return 1;
    /* Shell and serial rescue console must stay available for recovery actions. */
    if (str_eq(t->name, "shell")) return 1;
    if (str_eq(t->name, "serial")) return 1;
    return 0;
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
    unsigned int idx;
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
    idx = task_slot_index(t);
    if (idx < TASK_MAX) task_protect_mode[idx] = 0;
}

void task_reap_zombies(void)
{
    unsigned int i;
    arch_disable_interrupts();
    for (i = 1; i < TASK_MAX; i++) {
        if (&tasks[i] == current_task) continue;
        if (tasks[i].id == 0) continue;
        if (tasks[i].state != TASK_STATE_ZOMBIE) continue;
        unlink_task(&tasks[i]);
        reclaim_task_slot(&tasks[i]);
    }
    arch_enable_interrupts();
}

void task_preempt_tick(void)
{
    task_t *next;

    /* Don't preempt if the master switch is off or a critical section
     * has incremented the nesting counter. */
    if (!preempt_enabled || preempt_disable_count > 0)
        return;

    /* Don't preempt while a user-mode program is executing — the shared
     * syscall kernel stack and user_task_saved_rsp are not yet per-task. */
    if (user_task_saved_rsp != 0)
        return;

    /* Quick scan: is there another READY task? */
    next = current_task->next;
    while (next != current_task) {
        if (next->state == TASK_STATE_READY)
            break;
        next = next->next;
    }
    if (next == current_task)
        return;  /* sole runnable task — nothing to switch to */

    /* Prevent re-entrant preemption during the switch. */
    preempt_disable_count++;

    /* Yield via the normal cooperative path.  task_switch now saves
     * RFLAGS, so the resumed task restores its original interrupt flag. */
    {
        task_t *old = current_task;
        old->state = TASK_STATE_READY;
        do_switch_to_next(old);
        current_task->state = TASK_STATE_RUNNING;
    }

    preempt_disable_count--;
}

void task_set_preemption(int enabled)
{
    preempt_enabled = enabled ? 1 : 0;
}

int task_get_preemption(void)
{
    return preempt_enabled;
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
        task_protect_mode[i] = 0;
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
     * Build the initial context so task_switch() can pop RFLAGS, six
     * callee-saved registers, and ret into task_trampoline.
     *
     *  stack_top - 8  : alignment pad (skipped; not popped)
     *  stack_top - 16 : task_trampoline  ← return address for ret
     *  stack_top - 24 : 0  (rbx)
     *  stack_top - 32 : 0  (rbp)
     *  stack_top - 40 : 0  (r12)
     *  stack_top - 48 : 0  (r13)
     *  stack_top - 56 : 0  (r14)
     *  stack_top - 64 : 0  (r15)
     *  stack_top - 72 : 0x202  (RFLAGS: IF=1)  ← initial rsp
     *
     * After popfq, six pops and ret, RSP = stack_top - 8, which is
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
    *(--sp) = 0x202UL; /* RFLAGS: IF=1, reserved bit 1 set */

    t->rsp   = (unsigned long)sp;
    t->id    = next_id++;
    t->state = TASK_STATE_READY;
    t->entry = entry;
    t->arg   = arg;
    copy_name(t->name, name != (void *)0 ? name : "task", sizeof(t->name));
    task_protect_mode[task_slot_index(t)] = 0;

    /* Insert after task0 so the round-robin visits new tasks quickly. */
    t->next          = tasks[0].next;
    tasks[0].next    = t;

    task_log_event("create", t);

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
    task_log_event("exit", current_task);
    current_task->state = TASK_STATE_ZOMBIE;
    do_switch_to_next(current_task);
    for (;;) arch_halt(); /* should never reach here */
}

task_t *task_current(void)
{
    return current_task;
}

int task_find_id_by_name(const char *name)
{
    unsigned int i;
    int found = -1;
    if (name == (void *)0 || name[0] == '\0') return -1;

    arch_disable_interrupts();
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].id == 0) continue;
        if (tasks[i].state == TASK_STATE_ZOMBIE) continue;
        if (!str_eq(tasks[i].name, name)) continue;
        found = (int)tasks[i].id;
        break;
    }
    arch_enable_interrupts();
    return found;
}

int task_set_protection_by_id(unsigned int id, int enabled)
{
    task_t *t;
    unsigned int idx;
    if (id == 0) return -1;
    arch_disable_interrupts();
    t = find_task_by_id(id);
    if (t == (void *)0 || t == &tasks[0])
    {
        arch_enable_interrupts();
        return -1;
    }
    idx = task_slot_index(t);
    if (idx >= TASK_MAX)
    {
        arch_enable_interrupts();
        return -1;
    }
    task_protect_mode[idx] = enabled ? 1 : 2;
    arch_enable_interrupts();
    task_log_event(enabled ? "protect-on" : "protect-off", t);
    return 0;
}

int task_set_protection_by_name(const char *name, int enabled)
{
    unsigned int i;
    int rc = -1;
    if (name == (void *)0 || name[0] == '\0') return -1;
    arch_disable_interrupts();
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].id == 0) continue;
        if (&tasks[i] == &tasks[0]) continue;
        if (!str_eq(tasks[i].name, name)) continue;
        task_protect_mode[i] = enabled ? 1 : 2;
        rc = 0;
        break;
    }
    arch_enable_interrupts();
    if (rc == 0) task_log_event(enabled ? "protect-on" : "protect-off", &tasks[i]);
    return rc;
}

int task_is_protected_id(unsigned int id)
{
    task_t *t;
    int protected = 0;
    arch_disable_interrupts();
    t = find_task_by_id(id);
    if (t != (void *)0) protected = is_protected_task(t);
    arch_enable_interrupts();
    return protected;
}

void task_set_event_log(int enabled)
{
    task_event_log_on = enabled ? 1 : 0;
}

int task_event_log_enabled(void)
{
    return task_event_log_on;
}

void task_print_list(void)
{
    unsigned int i;
    terminal_write_line("  ID  State    Class Prot Name");
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].id == 0) continue;
        terminal_write("  ");
        write_uint(tasks[i].id);
        terminal_write("   ");
        if (tasks[i].state == TASK_STATE_RUNNING)     terminal_write("RUNNING  ");
        else if (tasks[i].state == TASK_STATE_READY)  terminal_write("READY    ");
        else if (tasks[i].state == TASK_STATE_STOPPED) terminal_write("STOPPED  ");
        else                                           terminal_write("ZOMBIE   ");
        terminal_write(task_class_name(&tasks[i]));
        if (str_eq(task_class_name(&tasks[i]), "SYS")) terminal_write("   ");
        else terminal_write("  ");
        terminal_write(is_protected_task(&tasks[i]) ? "Y    " : "N    ");
        terminal_write_line(tasks[i].name);
    }
}

void task_print_list_serial(void)
{
    unsigned int i;
    serial_write("  ID  State    Class Prot Name\r\n");
    for (i = 0; i < TASK_MAX; i++) {
        char buf[12];
        unsigned int v, j;
        if (tasks[i].id == 0) continue;
        serial_write("  ");
        /* write ID */
        v = tasks[i].id; j = 0;
        if (v == 0) { buf[j++] = '0'; }
        else { while (v > 0) { buf[j++] = (char)('0' + v % 10); v /= 10; }
               { unsigned int lo = 0, hi = j - 1;
                 while (lo < hi) { char t = buf[lo]; buf[lo] = buf[hi]; buf[hi] = t; lo++; hi--; } } }
        buf[j] = '\0';
        serial_write(buf);
        serial_write("   ");
        if (tasks[i].state == TASK_STATE_RUNNING)      serial_write("RUNNING  ");
        else if (tasks[i].state == TASK_STATE_READY)   serial_write("READY    ");
        else if (tasks[i].state == TASK_STATE_STOPPED) serial_write("STOPPED  ");
        else                                            serial_write("ZOMBIE   ");
        serial_write(task_class_name(&tasks[i]));
        if (str_eq(task_class_name(&tasks[i]), "SYS")) serial_write("   ");
        else serial_write("  ");
        serial_write(is_protected_task(&tasks[i]) ? "Y    " : "N    ");
        serial_write(tasks[i].name);
        serial_write("\r\n");
    }
}

int task_kill(unsigned int id)
{
    task_t *t;
    arch_disable_interrupts();
    t = find_task_by_id(id);
    if (t == (void *)0 || is_protected_task(t) || t == current_task)
    {
        arch_enable_interrupts();
        return -1;
    }
    task_log_event("kill", t);
    unlink_task(t);
    reclaim_task_slot(t);
    arch_enable_interrupts();
    return 0;
}

int task_kill_all(void)
{
    unsigned int i;
    int killed = 0;
    arch_disable_interrupts();
    for (i = 1; i < TASK_MAX; i++) {
        if (&tasks[i] == current_task) continue;
        if (tasks[i].id == 0) continue;
        if (is_protected_task(&tasks[i])) continue;
        task_log_event("kill", &tasks[i]);
        unlink_task(&tasks[i]);
        reclaim_task_slot(&tasks[i]);
        killed++;
    }
    arch_enable_interrupts();
    return killed;
}

int task_stop(unsigned int id)
{
    task_t *t;
    arch_disable_interrupts();
    t = find_task_by_id(id);
    if (t == (void *)0 || is_protected_task(t) || t == current_task ||
        (t->state != TASK_STATE_READY && t->state != TASK_STATE_RUNNING))
    {
        arch_enable_interrupts();
        return -1;
    }
    t->state = TASK_STATE_STOPPED;
    arch_enable_interrupts();
    task_log_event("stop", t);
    return 0;
}

int task_continue(unsigned int id)
{
    task_t *t;
    arch_disable_interrupts();
    t = find_task_by_id(id);
    if (t == (void *)0 || t->state != TASK_STATE_STOPPED)
    {
        arch_enable_interrupts();
        return -1;
    }
    t->state = TASK_STATE_READY;
    arch_enable_interrupts();
    task_log_event("cont", t);
    return 0;
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

static unsigned long align_up_page(unsigned long addr)
{
    return (addr + 4095UL) & ~4095UL;
}

static void user_heap_unmap_range(unsigned long start, unsigned long end)
{
    unsigned long page;
    for (page = start; page < end; page += 4096UL)
    {
        unsigned long phys = paging_get_phys(page);
        if (phys != 0)
        {
            paging_unmap_page(page);
            phys_free_page(phys);
        }
    }
}

void task_user_heap_reset(void)
{
    if (user_heap_base != 0 && user_heap_limit > user_heap_base)
    {
        unsigned long mapped_start = align_up_page(user_heap_base);
        unsigned long mapped_end = align_up_page(user_heap_brk);
        if (mapped_end > mapped_start)
        {
            user_heap_unmap_range(mapped_start, mapped_end);
        }
    }
    user_heap_base = 0;
    user_heap_limit = 0;
    user_heap_brk = 0;
}

int task_user_heap_config(unsigned long base, unsigned long limit)
{
    if (base == 0 || limit <= base) return -1;
    task_user_heap_reset();
    user_heap_base = base;
    user_heap_limit = limit;
    user_heap_brk = base;
    return 0;
}

unsigned long task_user_heap_brk(unsigned long new_break)
{
    unsigned long old_break;
    unsigned long map_from;
    unsigned long map_to;
    unsigned long page;

    if (user_heap_base == 0 || user_heap_limit <= user_heap_base)
    {
        brk_trace_fail("unconfigured", new_break, user_heap_brk, 0);
        return (unsigned long)-1;
    }

    if (new_break == 0)
    {
        return user_heap_brk;
    }

    if (new_break < user_heap_base || new_break > user_heap_limit)
    {
        brk_trace_fail("bounds", new_break, user_heap_brk, 0);
        return (unsigned long)-1;
    }

    old_break = user_heap_brk;
    if (new_break == old_break) return old_break;

    map_from = align_up_page(old_break);
    map_to = align_up_page(new_break);

    if (new_break > old_break)
    {
        for (page = map_from; page < map_to; page += 4096UL)
        {
            unsigned long phys;
            if (paging_get_phys(page) != 0)
            {
                brk_trace_fail("present", new_break, old_break, page);
                user_heap_unmap_range(map_from, page);
                return (unsigned long)-1;
            }
            phys = phys_alloc_page();
            if (phys == 0)
            {
                brk_trace_fail("oom", new_break, old_break, page);
                user_heap_unmap_range(map_from, page);
                return (unsigned long)-1;
            }
            if (paging_map_page(page, phys,
                                PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE |
                                PAGE_FLAG_USER | PAGE_FLAG_NO_EXECUTE) != 0)
            {
                brk_trace_fail("map", new_break, old_break, page);
                phys_free_page(phys);
                user_heap_unmap_range(map_from, page);
                return (unsigned long)-1;
            }

            /* Zero freshly-mapped heap pages before exposing them to user mode. */
            memory_zero_phys_page(phys);
        }
    }
    else
    {
        user_heap_unmap_range(map_to, map_from);
    }

    user_heap_brk = new_break;
    return user_heap_brk;
}

void task_exec_user(unsigned long entry, unsigned long user_rsp, unsigned long user_arg0, unsigned long argv1_ptr)
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
    task_save_and_enter_user(entry, user_rsp, user_arg0, &user_task_saved_rsp, argv1_ptr);

    /* Execution resumes here after SYS_EXIT or a user-mode fault. */
    user_task_saved_rsp = 0;
    task_user_heap_reset();

    /* Clear the TSS RSP0 so stale use is obvious. */
    syscall_kernel_rsp_slot = 0;
    gdt_tss_set_rsp0(0);
}
