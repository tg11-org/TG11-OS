#include "kernel.h"

#include "arch.h"
#include "idt.h"
#include "terminal.h"
#include "mouse.h"
#include "fs.h"
#include "blockdev.h"
#include "framebuffer.h"
#include "memory.h"
#include "task.h"
#include "syscall.h"
#include "timer.h"
#include "serial_console.h"

static int shell_watchdog_on = 0;

static void shell_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        terminal_poll();
        task_yield();
    }
}

int kernel_ensure_shell_task(void)
{
    int id = task_find_id_by_name("shell");
    if (id > 0) return id;
    return task_create("shell", shell_task, (void *)0);
}

void kernel_set_shell_watchdog(int enabled)
{
    shell_watchdog_on = enabled ? 1 : 0;
}

int kernel_shell_watchdog_enabled(void)
{
    return shell_watchdog_on;
}

void kernel_main(unsigned long mb2_info_addr)
{
    gdt_init();
    task_init();
	memory_init(mb2_info_addr);
	fs_init();
    blockdev_init();
	framebuffer_init(mb2_info_addr);
    framebuffer_try_auto_hires();
    terminal_init(mb2_info_addr);
    idt_init();
    timer_init();
    syscall_init();
    mouse_init();
    arch_enable_interrupts();

    if (kernel_ensure_shell_task() < 0)
    {
        terminal_write_line("[kernel] failed to create shell task");
    }
    if (task_create("serial", serial_console_task, (void *)0) < 0)
    {
        terminal_write_line("[kernel] failed to create serial task");
    }

    for (;;)
    {
        if (shell_watchdog_on) kernel_ensure_shell_task();
        task_yield();
        arch_halt();
    }
}