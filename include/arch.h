#ifndef TG11_ARCH_H
#define TG11_ARCH_H

void arch_enable_interrupts(void);
void arch_disable_interrupts(void);
void arch_halt(void);
unsigned char arch_inb(unsigned short port);
unsigned short arch_inw(unsigned short port);
void arch_outb(unsigned short port, unsigned char value);
void arch_outw(unsigned short port, unsigned short value);
void arch_io_wait(void);
void arch_lidt(const void *idtr);

#endif