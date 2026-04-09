#ifndef TG11_ARCH_H
#define TG11_ARCH_H

void arch_enable_interrupts(void);
void arch_disable_interrupts(void);
void arch_halt(void);
unsigned char arch_inb(unsigned short port);
unsigned short arch_inw(unsigned short port);
unsigned int arch_inl(unsigned short port);
void arch_outb(unsigned short port, unsigned char value);
void arch_outw(unsigned short port, unsigned short value);
void arch_outl(unsigned short port, unsigned int value);
void arch_io_wait(void);
void arch_lidt(const void *idtr);
void arch_lgdt(const void *gdtr);
unsigned long arch_read_cr2(void);
unsigned long arch_read_cr3(void);
void arch_write_cr3(unsigned long value);
void arch_invlpg(const void *addr);
void gdt_init(void);

/* MSR access */
void          arch_wrmsr(unsigned long msr, unsigned long value);
unsigned long arch_rdmsr(unsigned long msr);
/* Load the task register with a GDT selector */
void          arch_ltr(unsigned short selector);
/* Update TSS RSP0 (defined in gdt.c) */
void          gdt_tss_set_rsp0(unsigned long rsp0);

#endif