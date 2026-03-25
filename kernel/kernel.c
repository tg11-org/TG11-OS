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
    syscall_init();
    mouse_init();
    arch_enable_interrupts();

    for (;;)
    {
        terminal_poll();
    }
}