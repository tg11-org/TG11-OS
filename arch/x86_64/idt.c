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
#include "serial.h"
#include "mouse.h"
#include "elf.h"
#include "ksym.h"
#include "memory.h"
#include "task.h"
#include "timer.h"

#define IDT_ENTRIES 256
#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI 0x20
#define QEMU_POWER_PORT 0x604
#define QEMU_POWER_OFF  0x2000
#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01
#define IRQ_BASE 0x20
#define IRQ_KEYBOARD_VECTOR (IRQ_BASE + 1)
#define IRQ_TIMER_VECTOR    (IRQ_BASE + 0)
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

/* Exception statistics tracking */
static unsigned long exception_stats[32] = {0};

extern void exception_hang_stub(void);
extern void isr_ignore_stub(void);
extern void irq1_stub(void);
extern void irq12_stub(void);
extern void irq0_stub(void);

extern void exception_stub_0(void);
extern void exception_stub_1(void);
extern void exception_stub_2(void);
extern void exception_stub_3(void);
extern void exception_stub_4(void);
extern void exception_stub_5(void);
extern void exception_stub_6(void);
extern void exception_stub_7(void);
extern void double_fault_stub(void);
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

static void pic_unmask_irqs(void)
{
	/* Master: enable IRQ0 (timer), IRQ1 (keyboard), IRQ2 (cascade to slave) */
	arch_outb(PIC1_DATA, 0xF8);
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

static inline void panic_write(const char *str)
{
	terminal_write(str);
	serial_write(str);
}

static inline void panic_writeln(const char *str)
{
	terminal_write_line(str);
	serial_write(str);
	serial_putchar('\n');
}

static int is_canonical_addr(unsigned long addr)
{
	unsigned long hi = addr >> 47;
	return hi == 0 || hi == 0x1FFFFUL;
}

static void write_symbolized_addr(const char *label, unsigned long addr)
{
	elf_symbol_t sym;
	unsigned long offset = 0;
	unsigned long image_base = 0;
	unsigned long image_end = 0;

	if (elf_symbolize_active_addr(addr, &sym, &offset, &image_base, &image_end) == ELF_OK)
	{
		terminal_write(label);
		terminal_write(sym.name);
		terminal_write("+");
		write_hex_64(offset);
		terminal_write("\n");
		terminal_write("  Image:      ");
		write_hex_64(image_base);
		terminal_write(" - ");
		write_hex_64(image_end);
		terminal_write("\n");
		return;
	}

	/* Try the built-in kernel symbol table (generated at build time) */
	{
		const char *name = ksym_lookup(addr, &offset);
		if (name != (void *)0)
		{
			terminal_write(label);
			terminal_write(name);
			terminal_write("+");
			write_hex_64(offset);
			terminal_write("\n");
			return;
		}
	}

	/* Last fallback: print raw address if it looks like a kernel address */
	{
		extern char __kernel_start[];
		extern char __kernel_end[];
		int in_kernel = (addr >= (unsigned long)__kernel_start &&
		                 addr <  (unsigned long)__kernel_end);
		int in_high   = (is_canonical_addr(addr) &&
		                 addr >= 0xFFFF800000000000UL);
		if (in_kernel || in_high)
		{
			terminal_write(label);
			terminal_write("[kernel] ");
			write_hex_64(addr);
			terminal_write("\n");
		}
	}
}

static void write_page_fault_detail(unsigned long error_code)
{
	unsigned long cr2 = arch_read_cr2();
	int present = (error_code & 0x01UL) != 0;
	int is_write = (error_code & 0x02UL) != 0;
	int is_user = (error_code & 0x04UL) != 0;
	int is_exec = (error_code & 0x10UL) != 0;

	terminal_write("Page Fault:  ");
	write_hex_64(cr2);
	terminal_write("\n");

	/* Context line: describe what operation caused the fault */
	terminal_write("  Accessed:   ");
	if (is_exec)
		terminal_write("code fetch");
	else if (is_write)
		terminal_write("memory write");
	else
		terminal_write("memory read");
	terminal_write(" to ");
	if (!present)
		terminal_write("[unmapped]");
	else if (is_user)
		terminal_write("[kernel-only page]");
	else
		terminal_write("[protection fault]");
	terminal_write("\n");

	terminal_write("  Present:    ");
	terminal_write(present ? "yes" : "no");
	terminal_write("  Write: ");
	terminal_write(is_write ? "yes" : "no");
	terminal_write("  User: ");
	terminal_write(is_user ? "yes" : "no");
	terminal_write("\n");
	terminal_write("  Reserved:   ");
	terminal_write((error_code & 0x08UL) ? "yes" : "no");
	terminal_write("  Exec:  ");
	terminal_write(is_exec ? "yes" : "no");
	terminal_write("  PK:   ");
	terminal_write((error_code & 0x20UL) ? "yes" : "no");
	terminal_write("\n");
	terminal_write("  ShadowStk:  ");
	terminal_write((error_code & 0x40UL) ? "yes" : "no");
	terminal_write("  SGX:   ");
	terminal_write((error_code & 0x8000UL) ? "yes" : "no");
	terminal_write("\n");
}

static int is_range_mapped(unsigned long addr, unsigned long size)
{
	unsigned long page;
	unsigned long start;
	unsigned long end;

	if (size == 0) return 1;
	if (!is_canonical_addr(addr)) return 0;
	if (addr + (size - 1) < addr) return 0;

	start = addr & ~(MEMORY_PAGE_SIZE - 1UL);
	end = (addr + (size - 1)) & ~(MEMORY_PAGE_SIZE - 1UL);
	for (page = start; page <= end; page += MEMORY_PAGE_SIZE)
	{
		if (paging_get_phys(page) == 0) return 0;
	}
	return 1;
}

static int safe_read_u64(unsigned long addr, unsigned long *out)
{
	if (out == (void *)0) return 0;
	if (!is_range_mapped(addr, sizeof(unsigned long))) return 0;
	*out = *((volatile unsigned long *)addr);
	return 1;
}

static void write_stack_hints(unsigned long rsp)
{
	unsigned long slot0;
	unsigned long slot1;

	terminal_write("Stack Hints:\n");
	if (!safe_read_u64(rsp, &slot0))
	{
		terminal_write("  RSP unreadable at ");
		write_hex_64(rsp);
		terminal_write("\n");
		return;
	}

	terminal_write("  [RSP+0]: ");
	write_hex_64(slot0);
	terminal_write("\n");
	write_symbolized_addr("    Symbol: ", slot0);

	if (!safe_read_u64(rsp + 8UL, &slot1))
	{
		terminal_write("  [RSP+8]: <unreadable>\n");
		return;
	}

	terminal_write("  [RSP+8]: ");
	write_hex_64(slot1);
	terminal_write("\n");
	write_symbolized_addr("    Symbol: ", slot1);
}

static void write_stack_scan(unsigned long rsp)
{
	unsigned int i;
	unsigned int hits = 0;

	terminal_write("Stack Scan:\n");
	for (i = 0; i < 8; i++)
	{
		unsigned long slot_addr = rsp + ((unsigned long)i * 8UL);
		unsigned long value;
		elf_symbol_t sym;
		unsigned long offset = 0;

		if (!safe_read_u64(slot_addr, &value)) break;
		if (elf_symbolize_active_addr(value, &sym, &offset, (void *)0, (void *)0) != ELF_OK) continue;

		hits++;
		terminal_write("  [RSP+");
		if (i == 0) terminal_write("0");
		else
		{
			write_hex_64((unsigned long)i * 8UL);
		}
		terminal_write("] -> ");
		write_hex_64(value);
		terminal_write("\n");
		write_symbolized_addr("     => ", value);
	}

	if (hits == 0) terminal_write("  <no symbolized stack entries>\n");
}

static void write_backtrace(unsigned long rbp)
{
	unsigned int depth;
	int printed_any = 0;

	terminal_write("Backtrace (RBP chain):\n");
	for (depth = 0; depth < 8; depth++)
	{
		unsigned long next_rbp;
		unsigned long ret_addr;
		char depth_ch[2];

		if (rbp == 0)
		{
			if (!printed_any) terminal_write("  <no rbp>\n");
			return;
		}
		if ((rbp & 0x7UL) != 0 || !is_range_mapped(rbp, 16UL))
		{
			if (!printed_any)
			{
				terminal_write("  <unreadable rbp chain at ");
				write_hex_64(rbp);
				terminal_write(">\n");
			}
			return;
		}

		next_rbp = *((volatile unsigned long *)rbp);
		ret_addr = *((volatile unsigned long *)(rbp + 8UL));

		/* Stop if the return address is outside the kernel binary */
		{
			extern char __kernel_start[];
			extern char __kernel_end[];
			if (ret_addr < (unsigned long)__kernel_start ||
			    ret_addr >= (unsigned long)__kernel_end)
			{
				if (!printed_any) terminal_write("  <no frames>\n");
				return;
			}
		}

		printed_any = 1;
		depth_ch[0] = (char)('0' + (depth % 10));
		depth_ch[1] = '\0';

		terminal_write("  #");
		terminal_write(depth_ch);
		terminal_write(" rbp=");
		write_hex_64(rbp);
		terminal_write(" ret=");
		write_hex_64(ret_addr);
		terminal_write("\n");
		write_symbolized_addr("     -> ", ret_addr);

		if (next_rbp <= rbp) return;
		rbp = next_rbp;
	}
}

static void panic_reboot_now(void)
{
	unsigned long i;
	for (i = 0; i < 500000UL; i++) arch_io_wait();
	arch_outb(0xCF9, 0x06); /* ACPI/PCH reset */
	arch_outb(0x64, 0xFE);  /* 8042 fallback  */
}

static void panic_poweroff_now(void)
{
	arch_outw(QEMU_POWER_PORT, QEMU_POWER_OFF);
	for (;;) arch_halt();
}

void double_fault_handler(struct exception_frame *frame)
{
	/* Track exception statistics (vector 8 = double fault) */
	exception_stats[8]++;

	/* Log double fault to serial */
	serial_write("[PANIC] Double Fault - CPU exception handler recovery\n");

	screen_set_color(0x4F); /* bright white text on red background */
	screen_clear();

	terminal_write("====================================\n");
	terminal_write("      *** DOUBLE FAULT ***         \n");
	terminal_write("====================================\n");
	terminal_write("Exception Recovery: The CPU attempted\n");
	terminal_write("to handle an exception while another\n");
	terminal_write("exception was already in progress.\n");
	terminal_write("====================================\n");
	terminal_write("State at Time of Double Fault:\n");
	write_reg("RIP:         ", frame->rip);
	write_symbolized_addr("RIP Symbol:  ", frame->rip);
	write_reg("RSP:         ", frame->rsp);
	write_reg("RFLAGS:      ", frame->rflags);
	write_reg("Error Code:  ", frame->error_code);
	terminal_write("------------------------------------\n");
	terminal_write("Registers:\n");
	write_reg("  RAX: ", frame->rax);
	write_reg("  RBX: ", frame->rbx);
	write_reg("  RCX: ", frame->rcx);
	write_reg("  RDX: ", frame->rdx);
	write_reg("  RBP: ", frame->rbp);
	terminal_write("====================================\n");
	terminal_write("System halted.\n");
	terminal_write("Press F11 to reboot or F12 to power down.\n");

	arch_disable_interrupts();
	for (;;)
	{
		/* Poll keyboard directly because IRQ delivery is disabled in panic state. */
		if ((arch_inb(0x64) & 0x01) != 0)
		{
			unsigned char sc = arch_inb(0x60);
			if (sc == 0x57)
			{
				terminal_write("Rebooting...\n");
				panic_reboot_now();
			}
			if (sc == 0x58)
			{
				terminal_write("Powering down...\n");
				panic_poweroff_now();
			}
		}
		arch_io_wait();
	}
}

/*
 * Page Fault Recovery Skeleton
 * 
 * This is a placeholder for future page fault recovery mechanisms.
 * Currently detects fault type but always defers to panic handler.
 * Future enhancements could implement:
 * - Copy-on-write page allocation
 * - Lazy page loading from disk/filesystem
 * - Demand paging from swap
 * - Guard page expansion for stack growth
 */
typedef struct
{
	unsigned long faulting_address;
	int is_present : 1;
	int is_write : 1;
	int is_user : 1;
	int is_reserved : 1;
	int is_exec : 1;
	int recoverable : 1;
	int recovery_type;   /* 0=none, 1=alloc, 2=load, 3=stack_grow */
} pf_recovery_t;

static int pf_recovery_analyze(unsigned long cr2, unsigned long error_code, pf_recovery_t *rec)
{
	unsigned long canonical_high = 0x0000800000000000UL;
	unsigned long kernel_low = 0xFFFF800000000000UL;

	rec->faulting_address = cr2;
	rec->is_present = (error_code & 0x01UL) != 0;
	rec->is_write = (error_code & 0x02UL) != 0;
	rec->is_user = (error_code & 0x04UL) != 0;
	rec->is_reserved = (error_code & 0x08UL) != 0;
	rec->is_exec = (error_code & 0x10UL) != 0;
	rec->recoverable = 0;
	rec->recovery_type = 0;

	/* Only attempt recovery for unmapped pages in user-addressable space */
	if (rec->is_present || rec->is_reserved)
		return 0;  /* Present bit or reserved fault - unrecoverable */

	if (cr2 < canonical_high || (cr2 >= kernel_low))
	{
		/* Address is canonically valid - could potentially allocate */
		rec->recoverable = 1;
		rec->recovery_type = 1;  /* type 1: allocate on demand */
		return 1;
	}

	return 0;  /* Non-canonical address - unrecoverable */
}

static int pf_recovery_attempt(struct exception_frame *frame __attribute__((unused)), pf_recovery_t *rec)
{
	/* Currently no actual recovery is implemented */
	/* This is a placeholder for future recovery attempts */
	
	/* Log to serial what would be needed for recovery */
	if (rec->recoverable)
	{
		serial_write("[INFO] #PF at 0x");
		{
			static const char hex[] = "0123456789ABCDEF";
			int i;
			for (i = 60; i >= 0; i -= 4)
				serial_putchar(hex[(rec->faulting_address >> i) & 0xF]);
		}
		serial_write(" - would be recoverable (type ");
		serial_putchar('0' + rec->recovery_type);
		serial_write(")\n");
	}

	/* Always return 0 (no recovery) for now */
	return 0;
}

/* Defined in kernel/task.c */
extern void task_user_fault_exit(void);

void exception_handler(struct exception_frame *frame)
{
	unsigned char vec = (unsigned char)(frame->vector & 0xFF);
	int is_user = (frame->cs & 3) == 3; /* 1 if fault came from ring 3 */

	/* Track exception statistics */
	if (vec < 32)
		exception_stats[vec]++;

	/* Log exception to serial for debugger capture */
	serial_write("[PANIC] Exception ");
	{
		char buf[4];
		if (vec < 10)
		{
			buf[0] = '0' + vec;
			buf[1] = '\0';
		}
		else
		{
			buf[0] = '0' + (vec / 10);
			buf[1] = '0' + (vec % 10);
			buf[2] = '\0';
		}
		serial_write(buf);
	}
	serial_write(": ");
	serial_write(exception_names[vec < 32 ? vec : 0]);
	serial_write("\n");

	screen_set_color(0x4F); /* bright white text on red background */
	screen_clear();

	terminal_write("====================================\n");
	if (is_user)
		terminal_write("     *** USER PROGRAM FAULT ***     \n");
	else
		terminal_write("        *** KERNEL PANIC ***        \n");
	terminal_write("====================================\n");
	terminal_write("Exception ");
	write_hex_8(vec);
	terminal_write(": ");
	terminal_write(exception_names[vec < 32 ? vec : 0]);
	terminal_write("\n");
	write_reg("Error Code:  ", frame->error_code);
	if (vec == 14)
	{
		write_page_fault_detail(frame->error_code);
		
		/* Page fault recovery analysis */
		pf_recovery_t pf_rec;
		unsigned long cr2 = arch_read_cr2();
		
		if (pf_recovery_analyze(cr2, frame->error_code, &pf_rec))
		{
			terminal_write("Recovery:    Potentially recoverable\n");
			pf_recovery_attempt(frame, &pf_rec);
		}
		else
		{
			terminal_write("Recovery:    Not recoverable - halting\n");
		}
	}
	write_reg("RIP:         ", frame->rip);
	write_symbolized_addr("RIP Symbol:  ", frame->rip);
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
	terminal_write("------------------------------------\n");
	write_stack_hints(frame->rsp);
	write_backtrace(frame->rbp);
	write_stack_scan(frame->rsp);
	terminal_write("====================================\n");
	if (is_user) {
		terminal_write("User program terminated by exception.\n");
		terminal_write("====================================\n");
		task_user_fault_exit();
		/* task_user_fault_exit() does not return */
	}

	terminal_write("System halted.\n");
	terminal_write("Press F11 to reboot or F12 to power down.\n");

	arch_disable_interrupts();
	for (;;)
	{
		/* Poll keyboard directly because IRQ delivery is disabled in panic state. */
		if ((arch_inb(0x64) & 0x01) != 0)
		{
			unsigned char sc = arch_inb(0x60);
			if (sc == 0x57)
			{
				terminal_write("Rebooting...\n");
				panic_reboot_now();
			}
			if (sc == 0x58)
			{
				terminal_write("Powering down...\n");
				panic_poweroff_now();
			}
		}
		arch_io_wait();
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

void timer_interrupt_handler(void)
{
	timer_tick_handle_irq();
	pic_send_eoi(0);
	task_preempt_tick();
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
	idt_set_gate( 8, double_fault_stub);
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

	idt_set_gate(IRQ_TIMER_VECTOR, irq0_stub);
	idt_set_gate(IRQ_KEYBOARD_VECTOR, irq1_stub);
	idt_set_gate(IRQ_MOUSE_VECTOR, irq12_stub);

	idtr.limit = (unsigned short)(sizeof(idt) - 1);
	idtr.base = (unsigned long)idt;

	arch_lidt(&idtr);

	pic_remap();
	pic_unmask_irqs();
}

unsigned long idt_get_exception_count(unsigned char vector)
{
	if (vector >= 32)
		return 0;
	return exception_stats[vector];
}

void idt_display_backtrace(unsigned long rbp)
{
	write_backtrace(rbp);
}


