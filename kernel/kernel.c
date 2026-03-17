#include "kernel.h"

#include "arch.h"
#include "idt.h"
#include "terminal.h"
#include "mouse.h"

void kernel_main(unsigned long mb2_info_addr)
{
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