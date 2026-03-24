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
void arch_lgdt(const void *gdtr);
unsigned long arch_read_cr3(void);
void arch_write_cr3(unsigned long value);
void arch_invlpg(const void *addr);
void gdt_init(void);

#endif