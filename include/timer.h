#ifndef TG11_TIMER_H
#define TG11_TIMER_H

void timer_init(void);
void timer_tick_handle_irq(void);
unsigned long timer_ticks(void);

#endif
