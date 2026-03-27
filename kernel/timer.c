#include "timer.h"

#include "arch.h"

#define PIT_COMMAND_PORT 0x43
#define PIT_CHANNEL0_PORT 0x40
#define PIT_BASE_HZ 1193182UL
#define PIT_TICK_HZ 100UL

static volatile unsigned long system_ticks = 0;

void timer_init(void)
{
    unsigned long divisor = PIT_BASE_HZ / PIT_TICK_HZ;

    arch_outb(PIT_COMMAND_PORT, 0x36);
    arch_outb(PIT_CHANNEL0_PORT, (unsigned char)(divisor & 0xFF));
    arch_outb(PIT_CHANNEL0_PORT, (unsigned char)((divisor >> 8) & 0xFF));
}

void timer_tick_handle_irq(void)
{
    system_ticks++;
}

unsigned long timer_ticks(void)
{
    return system_ticks;
}
