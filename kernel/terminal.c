#include "terminal.h"

#include "arch.h"
#include "screen.h"
#include "serial.h"
#include "memmap.h"

#define INPUT_BUFFER_SIZE 128
#define SCANCODE_QUEUE_SIZE 256
#define QEMU_POWER_PORT     0x604
#define QEMU_POWER_OFF      0x2000

/* ------------------------------------------------------------------ */
/* Scancode ring buffer                                                */
/* ------------------------------------------------------------------ */
static volatile unsigned char scancode_queue[SCANCODE_QUEUE_SIZE];
static volatile unsigned long scancode_queue_head = 0;
static volatile unsigned long scancode_queue_tail = 0;

/* ------------------------------------------------------------------ */
/* Input line state                                                    */
/* ------------------------------------------------------------------ */
static char           input_buffer[INPUT_BUFFER_SIZE];
static unsigned long  input_length   = 0;
static unsigned long  cursor_pos     = 0;     /* insert point 0..input_length */
static unsigned short prompt_vga_start = 0;   /* VGA offset right after "> " */

/* ------------------------------------------------------------------ */
/* Keyboard modifier state                                             */
/* ------------------------------------------------------------------ */
static int shift_held   = 0;
static int caps_lock_on = 0;
static int extended_key = 0;

/* ------------------------------------------------------------------ */
/* History                                                             */
/* ------------------------------------------------------------------ */
#define HISTORY_SIZE 16
static char history[HISTORY_SIZE][INPUT_BUFFER_SIZE];
static int  history_count = 0;
static int  history_start = 0;
static int  history_pos   = -1;
static char history_draft[INPUT_BUFFER_SIZE];
static int  history_draft_len = 0;

/* ------------------------------------------------------------------ */
/* Misc                                                                */
/* ------------------------------------------------------------------ */
static int serial_ready = 0;

static void terminal_putc(char c)
{
	screen_putchar(c);

	if (serial_ready)
	{
		serial_putchar(c);
	}
}

void terminal_write(const char *str)
{
	while (*str != '\0')
	{
		terminal_putc(*str++);
	}
}

void terminal_write_line(const char *str)
{
	terminal_write(str);
	terminal_putc('\n');
}

/* ================================================================== */
/* Hex output helpers (also public — used by memmap, etc.)            */
/* ================================================================== */

void terminal_write_hex64(unsigned long v)
{
	static const char digits[] = "0123456789ABCDEF";
	char buf[19];
	int i;
	buf[0] = '0'; buf[1] = 'x';
	for (i = 0; i < 16; i++) { buf[17 - i] = digits[v & 0xF]; v >>= 4; }
	buf[18] = '\0';
	terminal_write(buf);
}

void terminal_write_hex8(unsigned char v)
{
	static const char digits[] = "0123456789ABCDEF";
	char buf[5];
	buf[0] = '0'; buf[1] = 'x';
	buf[2] = digits[(v >> 4) & 0xF];
	buf[3] = digits[v & 0xF];
	buf[4] = '\0';
	terminal_write(buf);
}

/* ================================================================== */
/* VGA cursor helpers                                                  */
/* ================================================================== */

/* Keep screen's software position (screen_row/col) at end-of-input.  */
static void sync_screen_pos(void)
{
	screen_set_pos((unsigned short)(prompt_vga_start + input_length));
}

static void terminal_prompt(void)
{
	screen_set_color(0x0B); /* bright cyan */
	terminal_write("> ");
	screen_set_color(0x0F); /* white */
	prompt_vga_start = screen_get_pos();
	cursor_pos = 0;
	screen_set_hw_cursor(prompt_vga_start);
}

/* ================================================================== */
/* String helpers                                                      */
/* ================================================================== */

static int string_equals(const char *a, const char *b)
{
	while (*a && *b) { if (*a != *b) return 0; a++; b++; }
	return *a == '\0' && *b == '\0';
}

static int string_starts_with(const char *text, const char *prefix)
{
	while (*prefix) { if (*text != *prefix) return 0; text++; prefix++; }
	return 1;
}

/* ================================================================== */
/* Hex parser                                                          */
/* ================================================================== */

static unsigned long parse_hex(const char *s, const char **end)
{
	unsigned long v = 0;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
	for (;;)
	{
		unsigned char n;
		char c = *s;
		if      (c >= '0' && c <= '9') n = (unsigned char)(c - '0');
		else if (c >= 'a' && c <= 'f') n = (unsigned char)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') n = (unsigned char)(c - 'A' + 10);
		else break;
		v = (v << 4) | n; s++;
	}
	if (end) *end = s;
	return v;
}

/* ================================================================== */
/* History                                                             */
/* ================================================================== */

static void history_push(void)
{
	int idx, i;
	if (input_length == 0) return;
	if (history_count > 0)
	{
		int last = (history_start + history_count - 1) % HISTORY_SIZE;
		if (string_equals(history[last], input_buffer)) return;
	}
	if (history_count < HISTORY_SIZE)
	{
		idx = (history_start + history_count) % HISTORY_SIZE;
		for (i = 0; i <= (int)input_length; i++) history[idx][i] = input_buffer[i];
		history_count++;
	}
	else
	{
		for (i = 0; i <= (int)input_length; i++) history[history_start][i] = input_buffer[i];
		history_start = (history_start + 1) % HISTORY_SIZE;
	}
}

static void clear_input_line(void)
{
	unsigned long i;
	for (i = 0; i < input_length; i++)
		screen_write_char_at((unsigned short)(prompt_vga_start + i), ' ');
	input_length = 0;
	cursor_pos   = 0;
	input_buffer[0] = '\0';
	screen_set_pos(prompt_vga_start);
	screen_set_hw_cursor(prompt_vga_start);
}

static void draw_from_buffer(void)
{
	unsigned long i;
	for (i = 0; i < input_length; i++)
		screen_write_char_at((unsigned short)(prompt_vga_start + i), input_buffer[i]);
	cursor_pos = input_length;
	sync_screen_pos();
	screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
}

static void handle_arrow_up(void)
{
	int idx;
	unsigned long i;
	const char *src;

	if (history_count == 0) return;

	if (history_pos == -1)
	{
		for (i = 0; i <= input_length; i++) history_draft[i] = input_buffer[i];
		history_draft_len = (int)input_length;
		history_pos = history_count - 1;
	}
	else if (history_pos > 0) { history_pos--; }
	else { return; }

	idx = (history_start + history_pos) % HISTORY_SIZE;
	src = history[idx];
	clear_input_line();
	for (i = 0; src[i]; i++) input_buffer[i] = src[i];
	input_length = i; input_buffer[i] = '\0';
	draw_from_buffer();
}

static void handle_arrow_down(void)
{
	unsigned long i;
	if (history_pos == -1) return;

	if (history_pos < history_count - 1)
	{
		const char *src; int idx;
		history_pos++;
		idx = (history_start + history_pos) % HISTORY_SIZE;
		src = history[idx];
		clear_input_line();
		for (i = 0; src[i]; i++) input_buffer[i] = src[i];
		input_length = i; input_buffer[i] = '\0';
	}
	else
	{
		history_pos = -1;
		clear_input_line();
		for (i = 0; i < (unsigned long)history_draft_len; i++) input_buffer[i] = history_draft[i];
		input_length = (unsigned long)history_draft_len;
		input_buffer[input_length] = '\0';
	}
	draw_from_buffer();
}

/* ================================================================== */
/* Tab completion                                                      */
/* ================================================================== */

static const char * const cmd_list[] = {
	"help", "echo", "clear", "reboot",
	"quit", "exit", "shutdown",
	"hexdump", "memmap", (void *)0
};

static void handle_tab(void)
{
	int i, match_count = 0;
	unsigned long j;
	const char *last = (void *)0;

	for (i = 0; cmd_list[i]; i++)
		if (string_starts_with(cmd_list[i], input_buffer)) { match_count++; last = cmd_list[i]; }

	if (match_count == 1)
	{
		clear_input_line();
		for (j = 0; last[j]; j++) { input_buffer[j] = last[j]; screen_write_char_at((unsigned short)(prompt_vga_start + j), last[j]); }
		input_length = j; cursor_pos = j; input_buffer[j] = '\0';
		sync_screen_pos();
		screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
	}
	else if (match_count > 1)
	{
		sync_screen_pos(); terminal_putc('\n');
		for (i = 0; cmd_list[i]; i++)
			if (string_starts_with(cmd_list[i], input_buffer)) { terminal_write(cmd_list[i]); terminal_putc(' '); }
		terminal_putc('\n');
		screen_set_color(0x0B); terminal_write("> "); screen_set_color(0x0F);
		prompt_vga_start = screen_get_pos();
		for (j = 0; j < input_length; j++) screen_write_char_at((unsigned short)(prompt_vga_start + j), input_buffer[j]);
		cursor_pos = input_length; sync_screen_pos();
		screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
	}
}

/* ================================================================== */
/* Scancode → character tables                                         */
/* ================================================================== */

static char translate_scancode_base(unsigned char sc)
{
	switch (sc)
	{
		case 0x02: return '1'; case 0x03: return '2'; case 0x04: return '3';
		case 0x05: return '4'; case 0x06: return '5'; case 0x07: return '6';
		case 0x08: return '7'; case 0x09: return '8'; case 0x0A: return '9';
		case 0x0B: return '0'; case 0x0C: return '-'; case 0x0D: return '=';
		case 0x10: return 'q'; case 0x11: return 'w'; case 0x12: return 'e';
		case 0x13: return 'r'; case 0x14: return 't'; case 0x15: return 'y';
		case 0x16: return 'u'; case 0x17: return 'i'; case 0x18: return 'o';
		case 0x19: return 'p'; case 0x1A: return '['; case 0x1B: return ']';
		case 0x1E: return 'a'; case 0x1F: return 's'; case 0x20: return 'd';
		case 0x21: return 'f'; case 0x22: return 'g'; case 0x23: return 'h';
		case 0x24: return 'j'; case 0x25: return 'k'; case 0x26: return 'l';
		case 0x27: return ';'; case 0x28: return '\''; case 0x29: return '`';
		case 0x2B: return '\\'; case 0x2C: return 'z'; case 0x2D: return 'x';
		case 0x2E: return 'c'; case 0x2F: return 'v'; case 0x30: return 'b';
		case 0x31: return 'n'; case 0x32: return 'm'; case 0x33: return ',';
		case 0x34: return '.'; case 0x35: return '/'; case 0x37: return '*';
		case 0x39: return ' ';
		case 0x47: return '7'; case 0x48: return '8'; case 0x49: return '9';
		case 0x4A: return '-'; case 0x4B: return '4'; case 0x4C: return '5';
		case 0x4D: return '6'; case 0x4E: return '+'; case 0x4F: return '1';
		case 0x50: return '2'; case 0x51: return '3'; case 0x52: return '0';
		case 0x53: return '.'; default: return '\0';
	}
}

static char translate_scancode_shifted(unsigned char sc)
{
	switch (sc)
	{
		case 0x02: return '!'; case 0x03: return '@'; case 0x04: return '#';
		case 0x05: return '$'; case 0x06: return '%'; case 0x07: return '^';
		case 0x08: return '&'; case 0x09: return '*'; case 0x0A: return '(';
		case 0x0B: return ')'; case 0x0C: return '_'; case 0x0D: return '+';
		case 0x10: return 'Q'; case 0x11: return 'W'; case 0x12: return 'E';
		case 0x13: return 'R'; case 0x14: return 'T'; case 0x15: return 'Y';
		case 0x16: return 'U'; case 0x17: return 'I'; case 0x18: return 'O';
		case 0x19: return 'P'; case 0x1A: return '{'; case 0x1B: return '}';
		case 0x1E: return 'A'; case 0x1F: return 'S'; case 0x20: return 'D';
		case 0x21: return 'F'; case 0x22: return 'G'; case 0x23: return 'H';
		case 0x24: return 'J'; case 0x25: return 'K'; case 0x26: return 'L';
		case 0x27: return ':'; case 0x28: return '"'; case 0x29: return '~';
		case 0x2B: return '|'; case 0x2C: return 'Z'; case 0x2D: return 'X';
		case 0x2E: return 'C'; case 0x2F: return 'V'; case 0x30: return 'B';
		case 0x31: return 'N'; case 0x32: return 'M'; case 0x33: return '<';
		case 0x34: return '>'; case 0x35: return '?'; case 0x37: return '*';
		case 0x39: return ' '; default: return '\0';
	}
}

static char translate_scancode(unsigned char sc)
{
	char base = translate_scancode_base(sc);
	int is_letter = (base >= 'a' && base <= 'z');
	int use_shift = shift_held;
	if (is_letter && caps_lock_on) use_shift = !use_shift;
	if (use_shift) { char s = translate_scancode_shifted(sc); if (s != '\0') return s; }
	return base;
}

/* ================================================================== */
/* Backspace (cursor-aware)                                            */
/* ================================================================== */

static void handle_backspace(void)
{
	unsigned long i;
	if (cursor_pos == 0) return;
	cursor_pos--;
	for (i = cursor_pos; i < input_length - 1; i++) input_buffer[i] = input_buffer[i + 1];
	input_length--;
	input_buffer[input_length] = '\0';
	for (i = cursor_pos; i < input_length; i++)
		screen_write_char_at((unsigned short)(prompt_vga_start + i), input_buffer[i]);
	screen_write_char_at((unsigned short)(prompt_vga_start + input_length), ' ');
	sync_screen_pos();
	screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
	if (serial_ready) serial_write("\b \b");
}

/* ================================================================== */
/* Commands                                                            */
/* ================================================================== */

static void terminal_shutdown(void)
{
	terminal_write_line("Shutting down...");
	arch_outw(QEMU_POWER_PORT, QEMU_POWER_OFF);
	arch_disable_interrupts();
	for (;;) arch_halt();
}

static void do_reboot(void)
{
	volatile unsigned long delay;

	terminal_write_line("[SYSTEM] Reboot requested...");
	terminal_write_line("[SYSTEM] Triggering reset now.");

	if (serial_ready)
	{
		serial_write("[SYSTEM] Reboot requested...\n");
		serial_write("[SYSTEM] Triggering reset now.\n");
	}

	for (delay = 0; delay < 500000; delay++)
	{
		arch_io_wait();
	}

	arch_disable_interrupts();
	arch_outb(0xCF9, 0x06); /* ACPI / PCH reset control register */
	arch_outb(0x64, 0xFE);  /* Fallback: 8042 reset line          */
	for (;;) arch_halt();
}

static void cmd_hexdump(const char *args)
{
	const char *p;
	unsigned long addr, len, i, j;

	while (*args == ' ') args++;
	if (*args == '\0') { terminal_write_line("Usage: hexdump <addr> [len]"); return; }

	addr = parse_hex(args, &p);
	while (*p == ' ') p++;
	len = (*p == '\0') ? 256 : parse_hex(p, (void *)0);
	if (len == 0 || len > 4096) len = 256;

	for (i = 0; i < len; i += 16)
	{
		unsigned long row = (len - i < 16) ? (len - i) : 16;
		terminal_write_hex64(addr + i);
		terminal_write("  ");
		for (j = 0; j < 16; j++)
		{
			if (j < row) { terminal_write_hex8(*((volatile unsigned char *)(addr + i + j))); terminal_putc(' '); }
			else         { terminal_write("   "); }
			if (j == 7) terminal_putc(' ');
		}
		terminal_write(" |");
		for (j = 0; j < row; j++)
		{ unsigned char b = *((volatile unsigned char *)(addr + i + j)); terminal_putc(b >= 0x20 && b < 0x7F ? (char)b : '.'); }
		terminal_write_line("|");
	}
}

static void run_command(void)
{
	if (input_length == 0) { terminal_prompt(); return; }

	if (string_equals(input_buffer, "help"))
	{
		terminal_write_line("Available commands:");
		terminal_write_line("  help              - Show this help");
		terminal_write_line("  echo <text>       - Print text");
		terminal_write_line("  clear             - Clear screen");
		terminal_write_line("  memmap            - Physical memory map");
		terminal_write_line("  hexdump <a> [n]   - Hex dump n bytes at address a");
		terminal_write_line("  reboot            - Reboot the system");
		terminal_write_line("  shutdown/exit/quit- Shut down");
	}
	else if (string_equals(input_buffer, "clear"))
	{
		input_length = 0; input_buffer[0] = '\0';
		screen_clear(); terminal_prompt(); return;
	}
	else if (string_equals(input_buffer, "memmap"))
	{
		memmap_print();
	}
	else if (string_starts_with(input_buffer, "hexdump"))
	{
		cmd_hexdump(input_buffer + 7);
	}
	else if (string_starts_with(input_buffer, "echo"))
	{
		if      (input_buffer[4] == '\0') terminal_putc('\n');
		else if (input_buffer[4] == ' ')  terminal_write_line(&input_buffer[5]);
		else    terminal_write_line("Unknown command. Type help for a list.");
	}
	else if (string_equals(input_buffer, "reboot"))
	{
		do_reboot();
	}
	else if (string_equals(input_buffer, "quit") ||
	         string_equals(input_buffer, "exit") ||
	         string_equals(input_buffer, "shutdown"))
	{
		terminal_shutdown();
	}
	else
	{
		terminal_write_line("Unknown command. Type help for a list.");
	}

	input_length = 0; input_buffer[0] = '\0';
	terminal_prompt();
}

static void submit_current_line(void)
{
	sync_screen_pos();
	terminal_putc('\n');
	input_buffer[input_length] = '\0';
	history_pos = -1;
	history_push();
	run_command();
}

/* ================================================================== */
/* Scancode dispatcher                                                 */
/* ================================================================== */

static void handle_scancode(unsigned char scancode)
{
	char c;
	unsigned long i;

	if (scancode == 0xE0) { extended_key = 1; return; }

	if (extended_key)
	{
		extended_key = 0;
		if (scancode == 0x1C) { submit_current_line(); return; } /* Keypad Enter */
		if (scancode == 0x35) { c = '/'; goto insert_character; } /* Keypad / */
		if (scancode == 0x48) { handle_arrow_up(); return; }
		if (scancode == 0x50) { handle_arrow_down(); return; }
		if (scancode == 0x4B && cursor_pos > 0)  /* Left */
		{ cursor_pos--; screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos)); return; }
		if (scancode == 0x4D && cursor_pos < input_length)  /* Right */
		{ cursor_pos++; screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos)); return; }
		if (scancode == 0x47) /* Home */
		{ cursor_pos = 0; screen_set_hw_cursor(prompt_vga_start); return; }
		if (scancode == 0x4F) /* End */
		{ cursor_pos = input_length; screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos)); return; }
		return;
	}

	if (scancode == 0x2A || scancode == 0x36) { shift_held = 1; return; }
	if (scancode == 0xAA || scancode == 0xB6) { shift_held = 0; return; }
	if (scancode == 0x3A) { caps_lock_on = !caps_lock_on; return; }
	if (scancode == 0x0F) { handle_tab(); return; }
	if (scancode & 0x80)  return;

	if (scancode == 0x0E) { handle_backspace(); return; }

	if (scancode == 0x1C)
	{
		submit_current_line();
		return;
	}

	c = translate_scancode(scancode);
	if (c == '\0' || input_length >= (INPUT_BUFFER_SIZE - 1)) return;

insert_character:
	if (input_length >= (INPUT_BUFFER_SIZE - 1)) return;

	if (cursor_pos == input_length)
	{
		input_buffer[input_length++] = c;
		input_buffer[input_length]   = '\0';
		cursor_pos++;
		terminal_putc(c);
		screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
	}
	else
	{
		/* Mid-line insert: shift right, redraw suffix */
		for (i = input_length; i > cursor_pos; i--) input_buffer[i] = input_buffer[i - 1];
		input_buffer[cursor_pos] = c;
		input_length++; cursor_pos++;
		input_buffer[input_length] = '\0';
		for (i = cursor_pos - 1; i < input_length; i++)
			screen_write_char_at((unsigned short)(prompt_vga_start + i), input_buffer[i]);
		sync_screen_pos();
		screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
	}
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

void terminal_enqueue_scancode(unsigned char scancode)
{
	unsigned long next = (scancode_queue_head + 1) % SCANCODE_QUEUE_SIZE;
	if (next == scancode_queue_tail) return;
	scancode_queue[scancode_queue_head] = scancode;
	scancode_queue_head = next;
}

void terminal_init(unsigned long mb2_info_addr)
{
	screen_clear();
	serial_ready = serial_init();
	memmap_init(mb2_info_addr);
	terminal_write_line("TG11 OS (64-bit)");
	terminal_write_line("v0.0.3");
	terminal_write_line("Type help to view available commands.");
	terminal_prompt();
}

void terminal_poll(void)
{
	while (scancode_queue_tail != scancode_queue_head)
	{
		unsigned char sc = scancode_queue[scancode_queue_tail];
		scancode_queue_tail = (scancode_queue_tail + 1) % SCANCODE_QUEUE_SIZE;
		handle_scancode(sc);
	}
}

