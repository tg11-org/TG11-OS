#include "kernel.h"

#include "arch.h"
#include "idt.h"
#include "terminal.h"
#include "mouse.h"
#include "fs.h"
#include "blockdev.h"

void kernel_main(unsigned long mb2_info_addr)
{
    gdt_init();
	fs_init();
    blockdev_init();
    terminal_init(mb2_info_addr);
    idt_init();
    mouse_init();
    arch_enable_interrupts();

    for (;;)
    {
		arch_halt();
        terminal_poll();
    }
}