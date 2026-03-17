/**
 * Copyright (C) 2026 TG11
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 * 
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "idt.h"

#include "arch.h"
#include "screen.h"
#include "terminal.h"
#include "mouse.h"

#define IDT_ENTRIES 256
#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI 0x20
#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01
#define IRQ_BASE 0x20
#define IRQ_KEYBOARD_VECTOR (IRQ_BASE + 1)
#define IRQ_MOUSE_VECTOR    (IRQ_BASE + 12)

#pragma pack(push, 1)

struct idt_entry
{
	unsigned short offset_low;
	unsigned short selector;
	unsigned char ist;
	unsigned char type_attr;
	unsigned short offset_mid;
	unsigned int offset_high;
	unsigned int zero;
};

struct idt_pointer
{
	unsigned short limit;
	unsigned long base;
};

#pragma pack(pop)

static struct idt_entry idt[IDT_ENTRIES];

extern void exception_hang_stub(void);
extern void isr_ignore_stub(void);
extern void irq1_stub(void);
extern void irq12_stub(void);

extern void exception_stub_0(void);
extern void exception_stub_1(void);
extern void exception_stub_2(void);
extern void exception_stub_3(void);
extern void exception_stub_4(void);
extern void exception_stub_5(void);
extern void exception_stub_6(void);
extern void exception_stub_7(void);
extern void exception_stub_8(void);
extern void exception_stub_9(void);
extern void exception_stub_10(void);
extern void exception_stub_11(void);
extern void exception_stub_12(void);
extern void exception_stub_13(void);
extern void exception_stub_14(void);
extern void exception_stub_15(void);
extern void exception_stub_16(void);
extern void exception_stub_17(void);
extern void exception_stub_18(void);
extern void exception_stub_19(void);
extern void exception_stub_20(void);
extern void exception_stub_21(void);
extern void exception_stub_22(void);
extern void exception_stub_23(void);
extern void exception_stub_24(void);
extern void exception_stub_25(void);
extern void exception_stub_26(void);
extern void exception_stub_27(void);
extern void exception_stub_28(void);
extern void exception_stub_29(void);
extern void exception_stub_30(void);
extern void exception_stub_31(void);

static void idt_set_gate(unsigned int vector, void (*handler)(void))
{
	unsigned long address = (unsigned long)handler;

	idt[vector].offset_low = (unsigned short)(address & 0xFFFF);
	idt[vector].selector = 0x08;
	idt[vector].ist = 0;
	idt[vector].type_attr = 0x8E;
	idt[vector].offset_mid = (unsigned short)((address >> 16) & 0xFFFF);
	idt[vector].offset_high = (unsigned int)((address >> 32) & 0xFFFFFFFFUL);
	idt[vector].zero = 0;
}

static void pic_remap(void)
{
	unsigned char master_mask = arch_inb(PIC1_DATA);
	unsigned char slave_mask = arch_inb(PIC2_DATA);

	arch_outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
	arch_io_wait();
	arch_outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
	arch_io_wait();

	arch_outb(PIC1_DATA, IRQ_BASE);
	arch_io_wait();
	arch_outb(PIC2_DATA, IRQ_BASE + 8);
	arch_io_wait();

	arch_outb(PIC1_DATA, 0x04);
	arch_io_wait();
	arch_outb(PIC2_DATA, 0x02);
	arch_io_wait();

	arch_outb(PIC1_DATA, ICW4_8086);
	arch_io_wait();
	arch_outb(PIC2_DATA, ICW4_8086);
	arch_io_wait();

	arch_outb(PIC1_DATA, master_mask);
	arch_outb(PIC2_DATA, slave_mask);
}

static void pic_unmask_keyboard(void)
{
	/* Master: enable IRQ1 (keyboard) + IRQ2 (cascade to slave) */
	arch_outb(PIC1_DATA, 0xF9);
	/* Slave: enable IRQ12 (mouse) */
	arch_outb(PIC2_DATA, 0xEF);
}

static void pic_send_eoi(unsigned char irq)
{
	if (irq >= 8)
	{
		arch_outb(PIC2_COMMAND, PIC_EOI);
	}

	arch_outb(PIC1_COMMAND, PIC_EOI);
}

static const char *exception_names[32] = {
	"Divide Error (#DE)",
	"Debug (#DB)",
	"NMI Interrupt",
	"Breakpoint (#BP)",
	"Overflow (#OF)",
	"BOUND Range Exceeded (#BR)",
	"Invalid Opcode (#UD)",
	"Device Not Available (#NM)",
	"Double Fault (#DF)",
	"Coprocessor Segment Overrun",
	"Invalid TSS (#TS)",
	"Segment Not Present (#NP)",
	"Stack Fault (#SS)",
	"General Protection (#GP)",
	"Page Fault (#PF)",
	"Reserved (0x0F)",
	"x87 FP Exception (#MF)",
	"Alignment Check (#AC)",
	"Machine Check (#MC)",
	"SIMD FP Exception (#XF)",
	"Virtualization Exception (#VE)",
	"Control Protection Exception (#CP)",
	"Reserved (0x16)",
	"Reserved (0x17)",
	"Reserved (0x18)",
	"Reserved (0x19)",
	"Reserved (0x1A)",
	"Reserved (0x1B)",
	"Reserved (0x1C)",
	"Reserved (0x1D)",
	"Security Exception (#SX)",
	"Reserved (0x1F)"
};

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

static void write_hex_64(unsigned long value)
{
	static const char digits[] = "0123456789ABCDEF";
	char buf[19];
	int i;

	buf[0] = '0';
	buf[1] = 'x';

	for (i = 0; i < 16; i++)
	{
		buf[17 - i] = digits[value & 0xF];
		value >>= 4;
	}

	buf[18] = '\0';
	terminal_write(buf);
}

static void write_hex_8(unsigned char value)
{
	static const char digits[] = "0123456789ABCDEF";
	char buf[5];

	buf[0] = '0';
	buf[1] = 'x';
	buf[2] = digits[(value >> 4) & 0xF];
	buf[3] = digits[value & 0xF];
	buf[4] = '\0';
	terminal_write(buf);
}

static void write_reg(const char *label, unsigned long value)
{
	terminal_write(label);
	write_hex_64(value);
	terminal_write("\n");
}

void exception_handler(struct exception_frame *frame)
{
	unsigned char vec = (unsigned char)(frame->vector & 0xFF);

	screen_set_color(0x4F); /* bright white text on red background */
	screen_clear();

	terminal_write("====================================\n");
	terminal_write("        *** KERNEL PANIC ***        \n");
	terminal_write("====================================\n");
	terminal_write("Exception ");
	write_hex_8(vec);
	terminal_write(": ");
	terminal_write(exception_names[vec < 32 ? vec : 0]);
	terminal_write("\n");
	write_reg("Error Code:  ", frame->error_code);
	write_reg("RIP:         ", frame->rip);
	write_reg("RSP:         ", frame->rsp);
	write_reg("RFLAGS:      ", frame->rflags);
	write_reg("CS:          ", frame->cs);
	write_reg("SS:          ", frame->ss);
	terminal_write("------------------------------------\n");
	terminal_write("Registers:\n");
	write_reg("  RAX: ", frame->rax);
	write_reg("  RBX: ", frame->rbx);
	write_reg("  RCX: ", frame->rcx);
	write_reg("  RDX: ", frame->rdx);
	write_reg("  RSI: ", frame->rsi);
	write_reg("  RDI: ", frame->rdi);
	write_reg("  RBP: ", frame->rbp);
	write_reg("   R8: ", frame->r8);
	write_reg("   R9: ", frame->r9);
	write_reg("  R10: ", frame->r10);
	write_reg("  R11: ", frame->r11);
	write_reg("  R12: ", frame->r12);
	write_reg("  R13: ", frame->r13);
	write_reg("  R14: ", frame->r14);
	write_reg("  R15: ", frame->r15);
	terminal_write("====================================\n");
	terminal_write("System halted.\n");

	arch_disable_interrupts();
	for (;;)
	{
		arch_halt();
	}
}

void isr_default_handler(void)
{
}

void keyboard_interrupt_handler(void)
{
	terminal_enqueue_scancode(arch_inb(0x60));
	pic_send_eoi(1);
}

void mouse_interrupt_handler(void)
{
	mouse_handle_byte(arch_inb(0x60));
	pic_send_eoi(12);
}

void idt_init(void)
{
	struct idt_pointer idtr;
	unsigned int index;

	/* Register per-exception handlers for vectors 0–31 */
	idt_set_gate( 0, exception_stub_0);
	idt_set_gate( 1, exception_stub_1);
	idt_set_gate( 2, exception_stub_2);
	idt_set_gate( 3, exception_stub_3);
	idt_set_gate( 4, exception_stub_4);
	idt_set_gate( 5, exception_stub_5);
	idt_set_gate( 6, exception_stub_6);
	idt_set_gate( 7, exception_stub_7);
	idt_set_gate( 8, exception_stub_8);
	idt_set_gate( 9, exception_stub_9);
	idt_set_gate(10, exception_stub_10);
	idt_set_gate(11, exception_stub_11);
	idt_set_gate(12, exception_stub_12);
	idt_set_gate(13, exception_stub_13);
	idt_set_gate(14, exception_stub_14);
	idt_set_gate(15, exception_stub_15);
	idt_set_gate(16, exception_stub_16);
	idt_set_gate(17, exception_stub_17);
	idt_set_gate(18, exception_stub_18);
	idt_set_gate(19, exception_stub_19);
	idt_set_gate(20, exception_stub_20);
	idt_set_gate(21, exception_stub_21);
	idt_set_gate(22, exception_stub_22);
	idt_set_gate(23, exception_stub_23);
	idt_set_gate(24, exception_stub_24);
	idt_set_gate(25, exception_stub_25);
	idt_set_gate(26, exception_stub_26);
	idt_set_gate(27, exception_stub_27);
	idt_set_gate(28, exception_stub_28);
	idt_set_gate(29, exception_stub_29);
	idt_set_gate(30, exception_stub_30);
	idt_set_gate(31, exception_stub_31);

	for (index = 32; index < IDT_ENTRIES; index++)
	{
		idt_set_gate(index, isr_ignore_stub);
	}

	idt_set_gate(IRQ_KEYBOARD_VECTOR, irq1_stub);
	idt_set_gate(IRQ_MOUSE_VECTOR, irq12_stub);

	idtr.limit = (unsigned short)(sizeof(idt) - 1);
	idtr.base = (unsigned long)idt;

	arch_lidt(&idtr);

	pic_remap();
	pic_unmask_keyboard();
}

