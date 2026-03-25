#ifndef TG11_IDT_H
#define TG11_IDT_H

struct exception_frame
{
	unsigned long r15;
	unsigned long r14;
	unsigned long r13;
	unsigned long r12;
	unsigned long r11;
	unsigned long r10;
	unsigned long r9;
	unsigned long r8;
	unsigned long rdi;
	unsigned long rsi;
	unsigned long rbp;
	unsigned long rdx;
	unsigned long rcx;
	unsigned long rbx;
	unsigned long rax;
	unsigned long vector;
	unsigned long error_code;
	/* CPU-pushed iret frame */
	unsigned long rip;
	unsigned long cs;
	unsigned long rflags;
	unsigned long rsp;
	unsigned long ss;
};

void idt_init(void);
void double_fault_handler(struct exception_frame *frame);
unsigned long idt_get_exception_count(unsigned char vector);
void idt_display_backtrace(unsigned long rbp);

#endif