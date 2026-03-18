#include "terminal.h"

#include "arch.h"
#include "screen.h"
#include "serial.h"
#include "kernel.h"
#include "memmap.h"
#include "fs.h"
#include "ata.h"
#include "blockdev.h"
#include "fat32.h"
#include "basic.h"

#define INPUT_BUFFER_SIZE 128
#define SCANCODE_QUEUE_SIZE 256
#define QEMU_POWER_PORT     0x604
#define QEMU_POWER_OFF      0x2000
#define EDITOR_BUFFER_SIZE  4096
#define VGA_TEXT_WIDTH      80
#define EDITOR_TEXT_GUTTER  6
#define EDITOR_HEX_BYTES_PER_ROW 16
#define EDITOR_HEX_DATA_COL 6
#define EDITOR_HEX_ASCII_COL 55
#define SYSTEM_THEME_DIR "/themes"
#define SYSTEM_THEME_CURRENT_PATH "/themes/current"
#define EDITOR_THEME_DIR "/edit/themes"
#define EDITOR_THEME_CURRENT_PATH "/edit/themes/current"

/* ------------------------------------------------------------------ */
/* Scancode ring buffer                                               */
/* ------------------------------------------------------------------ */
static volatile unsigned char scancode_queue[SCANCODE_QUEUE_SIZE];
static volatile unsigned long scancode_queue_head = 0;
static volatile unsigned long scancode_queue_tail = 0;

/* ------------------------------------------------------------------ */
/* Input line state                                                   */
/* ------------------------------------------------------------------ */
static char           input_buffer[INPUT_BUFFER_SIZE];
static unsigned long  input_length   = 0;
static unsigned long  cursor_pos     = 0;     /* insert point 0..input_length */
static unsigned short prompt_vga_start = 0;   /* VGA offset right after "> " */

/* ------------------------------------------------------------------ */
/* Keyboard modifier state                                            */
/* ------------------------------------------------------------------ */
static int shift_held   = 0;
static int caps_lock_on = 0;
static int ctrl_held    = 0;
static int extended_key = 0;

/* ------------------------------------------------------------------ */
/* History                                                            */
/* ------------------------------------------------------------------ */
#define HISTORY_SIZE 16
static char history[HISTORY_SIZE][INPUT_BUFFER_SIZE];
static int  history_count = 0;
static int  history_start = 0;
static int  history_pos   = -1;
static char history_draft[INPUT_BUFFER_SIZE];
static int  history_draft_len = 0;

/* ------------------------------------------------------------------ */
/* Misc                                                               */
/* ------------------------------------------------------------------ */
static int serial_ready = 0;
static int vfs_prefer_fat_root = 0;
static char fat_cwd[128] = "/";
static unsigned char terminal_text_color = 0x0F;
static unsigned char terminal_prompt_color = 0x0B;
static unsigned char editor_header_color = 0x0F;
static unsigned char editor_path_color = 0x0B;
static unsigned char editor_rule_color = 0x08;
static unsigned char editor_line_number_color = 0x08;
static unsigned char editor_text_color = 0x0F;
static unsigned char editor_sh_keyword_color = 0x0A;
static unsigned char editor_sh_comment_color = 0x08;
static unsigned char editor_sh_string_color = 0x0E;
static unsigned char editor_basic_keyword_color = 0x0D;
static unsigned char editor_basic_comment_color = 0x08;
static unsigned char editor_basic_string_color = 0x0B;
static int editor_active = 0;
static int editor_use_fat = 0;
static int editor_hex_mode = 0;
static int editor_hex_nibble = 0;
enum editor_lang
{
	EDITOR_LANG_PLAIN = 0,
	EDITOR_LANG_SH,
	EDITOR_LANG_BASIC
};
static enum editor_lang editor_language = EDITOR_LANG_PLAIN;
static char editor_target_path[128];
static char editor_buffer[EDITOR_BUFFER_SIZE];
static unsigned long editor_length = 0;
static unsigned long editor_cursor = 0;
static unsigned long editor_view_top = 0;
static unsigned short editor_vga_start = 0;
static unsigned short editor_prev_end = 0;
static int editor_dirty = 0;
static int script_mode_active = 0;
static int script_depth = 0;
#define SCRIPT_VAR_MAX 8
static int script_var_count = 0;
static char script_var_names[SCRIPT_VAR_MAX][16];
static char script_var_values[SCRIPT_VAR_MAX][96];

static int string_equals(const char *a, const char *b);
static const char *read_token(const char *s, char *out, unsigned long out_size);
static unsigned long string_length(const char *s);
static int fat_mode_active(void);
static int fat_resolve_path(const char *input, char *out, unsigned long out_size);
static void editor_handle_scancode(unsigned char scancode);
static void run_command(void);
static int eval_script_condition(const char *expr);
static int execute_substitution_command(const char *raw_cmd, char *out, unsigned long out_size);
static int expand_command_substitutions(const char *in, char *out, unsigned long out_size);
static int expand_cp437_aliases(const char *in, char *out, unsigned long out_size);
static void script_set_var(const char *name, const char *value);
static void editor_render(void);
static unsigned long editor_line_start(unsigned long index);
static unsigned long editor_line_end(unsigned long index);
static unsigned long editor_visible_rows(void);
static unsigned long editor_text_width(void);
static unsigned long editor_next_visual_row(unsigned long index);
static unsigned long editor_prev_visual_row(unsigned long index);
static unsigned long editor_cursor_row_from_top(unsigned long top, unsigned long cursor);
static unsigned long editor_visual_row_start(unsigned long index);
static unsigned long editor_visual_row_col(unsigned long index);
static unsigned long editor_visual_row_length(unsigned long row_start);
static unsigned long editor_move_visual_rows(unsigned long index, unsigned long rows, int down);
static unsigned long editor_line_number_for_index(unsigned long index);
static int editor_has_more_below(unsigned long top, unsigned long rows);
static void editor_draw_header(int can_scroll_up, int can_scroll_down);
static int editor_open_file(const char *path, int hex_mode);
static int parse_color_token(const char *s, unsigned char *out);
static int starts_with_word_ci(const char *s, const char *word, unsigned long *word_len_out);
static int token_is_keyword_ci(const char *s, unsigned long len, const char *const *keywords, unsigned long keyword_count);
static int editor_path_has_ext_ci(const char *path, const char *ext);
static enum editor_lang editor_detect_language_from_path(const char *path);
static int apply_system_theme_from_text(const char *name, const char *text);
static int apply_system_theme_by_name(const char *name, int persist_current);
static int apply_editor_theme_from_text(const char *name, const char *text);
static int apply_editor_theme_by_name(const char *name, int persist_current);
static void ensure_theme_files(void);
static void load_current_system_theme(void);
static void load_current_editor_theme(void);
static void cmd_themes(const char *args);
static void cmd_etheme(const char *args);
static void cmd_ethemes(const char *args);
static void cmd_ramfs(void);

static void print_help_basic(void)
{
	terminal_write_line("Basic commands:");
	terminal_write_line("  help [basic|fs|disk] - Show help pages");
	terminal_write_line("  version              - Show OS version");
	terminal_write_line("  echo <text>          - Print text");
	terminal_write_line("  glyph <0xNN>         - Print one CP437 glyph byte");
	terminal_write_line("  charmap              - Show CP437 alias escapes");
	terminal_write_line("  color show           - Show current colors");
	terminal_write_line("  color text <0xNN>    - Set text color attr byte");
	terminal_write_line("  color prompt <0xNN>  - Set prompt color attr byte");
	terminal_write_line("  theme <name>         - Apply system theme from /themes");
	terminal_write_line("  themes [list]        - List system themes");
	terminal_write_line("  etheme <name>        - Apply editor theme from /edit/themes");
	terminal_write_line("  etheme edit <name>   - Edit /edit/themes/<name>.theme");
	terminal_write_line("  ethemes [list]       - List editor themes");
	terminal_write_line("  clear                - Clear screen");
	terminal_write_line("  reboot               - Reboot the system");
	terminal_write_line("  shutdown/exit/quit   - Shut down");
	terminal_write_line("  memmap               - Physical memory map");
	terminal_write_line("  hexdump <a> [n]      - Hex dump memory");
}

static void print_help_fs(void)
{
	terminal_write_line("Filesystem commands:");
	terminal_write_line("  pwd                  - Print current directory");
	terminal_write_line("  ls [path]            - List entries");
	terminal_write_line("  cd <path>            - Change directory");
	terminal_write_line("  mkdir <path>         - Create directory");
	terminal_write_line("  touch <path>         - Create empty file");
	terminal_write_line("  write <p> <text>     - Write text");
	terminal_write_line("  cat <path>           - Read file");
	terminal_write_line("  rm <path>            - Remove path");
	terminal_write_line("  cp <src> <dst>       - Copy file");
	terminal_write_line("  mv <src> <dst>       - Move/rename");
	terminal_write_line("  edit <path>          - Edit text file");
	terminal_write_line("  hexedit <path>       - Edit raw bytes in hex");
	terminal_write_line("  run [-x] <path>      - Run script (-x echoes lines)");
	terminal_write_line("  basic <path>         - Run Tiny BASIC program");
	terminal_write_line("  script: foreach i in a,b do echo $(i)");
	terminal_write_line("  fatmount             - Mount FAT32 data disk");
	terminal_write_line("  ramfs                - Switch generic fs commands to RAM FS");
	terminal_write_line("  fatunmount           - Unmount FAT32 data disk");
	terminal_write_line("  fatls                - List FAT32 cwd");
	terminal_write_line("  fatcat <path>        - Read FAT32 file");
	terminal_write_line("  fattouch <path>      - Create FAT32 file");
	terminal_write_line("  fatwrite <p> <txt>   - Write FAT32 file");
	terminal_write_line("  fatrm <path>         - Remove FAT32 path");
}

static void print_help_disk(void)
{
	terminal_write_line("Disk/ATA commands:");
	terminal_write_line("  ataid                - Show ATA presence + sector count");
	terminal_write_line("  readsec <lba-hex>    - Dump one 512-byte sector");
	terminal_write_line("  writesec <lba> <txt> - Write marker text to one sector");
}

static void cmd_help(const char *args)
{
	char page[16];
	const char *p = read_token(args, page, sizeof(page));
	if (p == (void *)0 || page[0] == '\0' || string_equals(page, "basic"))
	{
		print_help_basic();
		terminal_write_line("Type 'help fs' or 'help disk' for more.");
		return;
	}
	if (string_equals(page, "fs"))
	{
		print_help_fs();
		return;
	}
	if (string_equals(page, "disk"))
	{
		print_help_disk();
		return;
	}
	terminal_write_line("Usage: help [basic|fs|disk]");
}

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
/* VGA cursor helpers                                                 */
/* ================================================================== */

/* Keep screen's software position (screen_row/col) at end-of-input.  */
static void sync_screen_pos(void)
{
	screen_set_pos((unsigned short)(prompt_vga_start + input_length));
}

static void terminal_prompt(void)
{
	screen_set_color(terminal_prompt_color);
	terminal_write("> ");
	screen_set_color(terminal_text_color);
	prompt_vga_start = screen_get_pos();
	cursor_pos = 0;
	screen_set_hw_cursor(prompt_vga_start);
}

/* ================================================================== */
/* String helpers                                                     */
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

static unsigned long string_length(const char *s)
{
	unsigned long n = 0;
	while (s[n] != '\0') n++;
	return n;
}

static int fat_mode_active(void)
{
	return vfs_prefer_fat_root && fat32_is_mounted();
}

static int fat_append_component(char parts[][16], int *depth, const char *component)
{
	unsigned long i = 0;
	if (*depth >= 24) return -1;
	while (component[i] != '\0')
	{
		if (i + 1 >= 16) return -1;
		parts[*depth][i] = component[i];
		i++;
	}
	parts[*depth][i] = '\0';
	(*depth)++;
	return 0;
}

static int fat_push_from_path(char parts[][16], int *depth, const char *path)
{
	char token[16];
	unsigned long i = 0;
	const char *p = path;

	while (*p != '\0')
	{
		while (*p == '/' || *p == '\\') p++;
		if (*p == '\0') break;

		i = 0;
		while (*p != '\0' && *p != '/' && *p != '\\')
		{
			if (i + 1 >= sizeof(token)) return -1;
			token[i++] = *p++;
		}
		token[i] = '\0';

		if (string_equals(token, "."))
		{
			continue;
		}
		if (string_equals(token, ".."))
		{
			if (*depth > 0) (*depth)--;
			continue;
		}

		if (fat_append_component(parts, depth, token) != 0) return -1;
	}

	return 0;
}

static int fat_build_path_from_parts(char parts[][16], int depth, char *out, unsigned long out_size)
{
	unsigned long n = 0;
	int i;
	if (out_size < 2) return -1;
	out[n++] = '/';
	if (depth == 0)
	{
		out[n] = '\0';
		return 0;
	}

	for (i = 0; i < depth; i++)
	{
		unsigned long j = 0;
		while (parts[i][j] != '\0')
		{
			if (n + 1 >= out_size) return -1;
			out[n++] = parts[i][j++];
		}
		if (i + 1 < depth)
		{
			if (n + 1 >= out_size) return -1;
			out[n++] = '/';
		}
	}
	out[n] = '\0';
	return 0;
}

static int fat_resolve_path(const char *input, char *out, unsigned long out_size)
{
	char parts[24][16];
	int depth = 0;
	const char *p;

	if (input == (void *)0 || out == (void *)0) return -1;
	if (input[0] == '\0')
	{
		if (string_length(fat_cwd) + 1 > out_size) return -1;
		{
			unsigned long i = 0;
			while (fat_cwd[i] != '\0') { out[i] = fat_cwd[i]; i++; }
			out[i] = '\0';
		}
		return 0;
	}

	p = input;
	if (p[0] != '/' && p[0] != '\\')
	{
		if (fat_push_from_path(parts, &depth, fat_cwd) != 0) return -1;
	}

	if (fat_push_from_path(parts, &depth, p) != 0) return -1;
	return fat_build_path_from_parts(parts, depth, out, out_size);
}

/* ================================================================== */
/* Hex parser                                                         */
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

static const char *skip_spaces(const char *s)
{
	while (*s == ' ') s++;
	return s;
}

static const char *read_token(const char *s, char *out, unsigned long out_size)
{
	unsigned long i = 0;
	s = skip_spaces(s);
	if (out_size == 0) return s;
	while (*s != '\0' && *s != ' ')
	{
		if (i + 1 >= out_size) return (void *)0;
		out[i++] = *s++;
	}
	out[i] = '\0';
	return s;
}

static int parse_color_token(const char *s, unsigned char *out)
{
	const char *end;
	unsigned long v;
	if (s == (void *)0 || out == (void *)0 || s[0] == '\0') return -1;
	v = parse_hex(s, &end);
	if (*end != '\0') return -1;
	if (v > 0xFFUL) return -1;
	*out = (unsigned char)v;
	return 0;
}

static char ascii_upper(char c)
{
	if (c >= 'a' && c <= 'z') return (char)(c - ('a' - 'A'));
	return c;
}

static int char_is_word(char c)
{
	if (c >= 'A' && c <= 'Z') return 1;
	if (c >= 'a' && c <= 'z') return 1;
	if (c >= '0' && c <= '9') return 1;
	return c == '_';
}

static int starts_with_word_ci(const char *s, const char *word, unsigned long *word_len_out)
{
	unsigned long i = 0;
	while (word[i] != '\0')
	{
		if (ascii_upper(s[i]) != ascii_upper(word[i])) return 0;
		i++;
	}
	if (word_len_out != (void *)0) *word_len_out = i;
	if (char_is_word(s[i])) return 0;
	return 1;
}

static int token_is_keyword_ci(const char *s, unsigned long len, const char *const *keywords, unsigned long keyword_count)
{
	unsigned long k;
	for (k = 0; k < keyword_count; k++)
	{
		const char *kw = keywords[k];
		unsigned long i = 0;
		while (kw[i] != '\0' && i < len)
		{
			if (ascii_upper(kw[i]) != ascii_upper(s[i])) break;
			i++;
		}
		if (i == len && kw[i] == '\0') return 1;
	}
	return 0;
}

static int editor_path_has_ext_ci(const char *path, const char *ext)
{
	unsigned long path_len = string_length(path);
	unsigned long ext_len = string_length(ext);
	unsigned long i;
	if (path_len < ext_len) return 0;
	for (i = 0; i < ext_len; i++)
	{
		if (ascii_upper(path[path_len - ext_len + i]) != ascii_upper(ext[i])) return 0;
	}
	return 1;
}

static enum editor_lang editor_detect_language_from_path(const char *path)
{
	if (editor_path_has_ext_ci(path, ".sh")) return EDITOR_LANG_SH;
	if (editor_path_has_ext_ci(path, ".bas") || editor_path_has_ext_ci(path, ".basic")) return EDITOR_LANG_BASIC;
	return EDITOR_LANG_PLAIN;
}

static int apply_system_theme_from_text(const char *name, const char *text)
{
	unsigned char t_text = terminal_text_color;
	unsigned char t_prompt = terminal_prompt_color;
	const char *p = text;

	while (p != (void *)0 && *p != '\0')
	{
		char line[96];
		char key[32];
		char val[32];
		unsigned long i = 0;
		unsigned long eq = 0;
		unsigned char c;

		while (*p == '\r' || *p == '\n') p++;
		if (*p == '\0') break;
		while (*p != '\0' && *p != '\n' && i + 1 < sizeof(line)) line[i++] = *p++;
		line[i] = '\0';
		while (*p == '\r' || *p == '\n') p++;
		if (line[0] == '\0' || line[0] == '#') continue;

		i = 0;
		while (line[i] != '\0') { if (line[i] == '=') { eq = i; break; } i++; }
		if (line[eq] != '=') continue;

		i = 0;
		while (i < eq && i + 1 < sizeof(key)) { key[i] = line[i]; i++; }
		key[i] = '\0';

		i = 0;
		while (line[eq + 1 + i] != '\0' && i + 1 < sizeof(val)) { val[i] = line[eq + 1 + i]; i++; }
		val[i] = '\0';

		if (parse_color_token(val, &c) != 0) continue;
		if (string_equals(key, "terminal_text")) t_text = c;
		else if (string_equals(key, "terminal_prompt")) t_prompt = c;
	}

	terminal_text_color = t_text;
	terminal_prompt_color = t_prompt;
	if (!editor_active) screen_set_color(terminal_text_color);

	if (name != (void *)0)
	{
		terminal_write("system theme: ");
		terminal_write_line(name);
	}

	return 0;
}

static int apply_system_theme_by_name(const char *name, int persist_current)
{
	char theme_path[64];
	const char *text;
	unsigned long i = 0;
	const char *prefix = SYSTEM_THEME_DIR "/";
	if (name == (void *)0 || name[0] == '\0') return -1;

	while (prefix[i] != '\0' && i + 1 < sizeof(theme_path)) { theme_path[i] = prefix[i]; i++; }
	{
		unsigned long j = 0;
		while (name[j] != '\0' && i + 1 < sizeof(theme_path)) theme_path[i++] = name[j++];
	}
	if (i + 7 >= sizeof(theme_path)) return -1;
	theme_path[i++] = '.';
	theme_path[i++] = 't';
	theme_path[i++] = 'h';
	theme_path[i++] = 'e';
	theme_path[i++] = 'm';
	theme_path[i++] = 'e';
	theme_path[i] = '\0';

	if (fs_read_text(theme_path, &text) != 0) return -1;
	if (apply_system_theme_from_text(name, text) != 0) return -1;
	if (persist_current) fs_write_text(SYSTEM_THEME_CURRENT_PATH, name);
	return 0;
}

static int apply_editor_theme_from_text(const char *name, const char *text)
{
	unsigned char e_header = editor_header_color;
	unsigned char e_path = editor_path_color;
	unsigned char e_rule = editor_rule_color;
	unsigned char e_line = editor_line_number_color;
	unsigned char e_text = editor_text_color;
	unsigned char sh_kw = editor_sh_keyword_color;
	unsigned char sh_comment = editor_sh_comment_color;
	unsigned char sh_string = editor_sh_string_color;
	unsigned char b_kw = editor_basic_keyword_color;
	unsigned char b_comment = editor_basic_comment_color;
	unsigned char b_string = editor_basic_string_color;
	const char *p = text;

	while (p != (void *)0 && *p != '\0')
	{
		char line[96];
		char key[32];
		char val[32];
		unsigned long i = 0;
		unsigned long eq = 0;
		unsigned char c;

		while (*p == '\r' || *p == '\n') p++;
		if (*p == '\0') break;
		while (*p != '\0' && *p != '\n' && i + 1 < sizeof(line)) line[i++] = *p++;
		line[i] = '\0';
		while (*p == '\r' || *p == '\n') p++;
		if (line[0] == '\0' || line[0] == '#') continue;

		i = 0;
		while (line[i] != '\0') { if (line[i] == '=') { eq = i; break; } i++; }
		if (line[eq] != '=') continue;

		i = 0;
		while (i < eq && i + 1 < sizeof(key)) { key[i] = line[i]; i++; }
		key[i] = '\0';

		i = 0;
		while (line[eq + 1 + i] != '\0' && i + 1 < sizeof(val)) { val[i] = line[eq + 1 + i]; i++; }
		val[i] = '\0';

		if (parse_color_token(val, &c) != 0) continue;
		if (string_equals(key, "editor_header")) e_header = c;
		else if (string_equals(key, "editor_path")) e_path = c;
		else if (string_equals(key, "editor_rule")) e_rule = c;
		else if (string_equals(key, "editor_line")) e_line = c;
		else if (string_equals(key, "editor_text")) e_text = c;
		else if (string_equals(key, "sh_keyword")) sh_kw = c;
		else if (string_equals(key, "sh_comment")) sh_comment = c;
		else if (string_equals(key, "sh_string")) sh_string = c;
		else if (string_equals(key, "basic_keyword")) b_kw = c;
		else if (string_equals(key, "basic_comment")) b_comment = c;
		else if (string_equals(key, "basic_string")) b_string = c;
	}

	editor_header_color = e_header;
	editor_path_color = e_path;
	editor_rule_color = e_rule;
	editor_line_number_color = e_line;
	editor_text_color = e_text;
	editor_sh_keyword_color = sh_kw;
	editor_sh_comment_color = sh_comment;
	editor_sh_string_color = sh_string;
	editor_basic_keyword_color = b_kw;
	editor_basic_comment_color = b_comment;
	editor_basic_string_color = b_string;
	if (editor_active) screen_set_color(editor_text_color);

	if (name != (void *)0)
	{
		terminal_write("editor theme: ");
		terminal_write_line(name);
	}

	return 0;
}

static int apply_editor_theme_by_name(const char *name, int persist_current)
{
	char theme_path[64];
	const char *text;
	unsigned long i = 0;
	const char *prefix = EDITOR_THEME_DIR "/";
	if (name == (void *)0 || name[0] == '\0') return -1;

	while (prefix[i] != '\0' && i + 1 < sizeof(theme_path)) { theme_path[i] = prefix[i]; i++; }
	{
		unsigned long j = 0;
		while (name[j] != '\0' && i + 1 < sizeof(theme_path)) theme_path[i++] = name[j++];
	}
	if (i + 7 >= sizeof(theme_path)) return -1;
	theme_path[i++] = '.';
	theme_path[i++] = 't';
	theme_path[i++] = 'h';
	theme_path[i++] = 'e';
	theme_path[i++] = 'm';
	theme_path[i++] = 'e';
	theme_path[i] = '\0';

	if (fs_read_text(theme_path, &text) != 0) return -1;
	if (apply_editor_theme_from_text(name, text) != 0) return -1;
	if (persist_current) fs_write_text(EDITOR_THEME_CURRENT_PATH, name);
	return 0;
}

static void ensure_theme_files(void)
{
	const char *existing;
	fs_mkdir(SYSTEM_THEME_DIR);
	fs_mkdir("/edit");
	fs_mkdir(EDITOR_THEME_DIR);

	if (fs_read_text(SYSTEM_THEME_DIR "/default.theme", &existing) != 0)
	{
		fs_write_text(SYSTEM_THEME_DIR "/default.theme",
			"# TG11 system theme file\n"
			"terminal_text=0x0F\n"
			"terminal_prompt=0x0B\n");
	}
	if (fs_read_text(SYSTEM_THEME_DIR "/c64.theme", &existing) != 0)
	{
		fs_write_text(SYSTEM_THEME_DIR "/c64.theme",
			"# Commodore-ish system palette\n"
			"terminal_text=0x19\n"
			"terminal_prompt=0x1E\n");
	}

	if (fs_read_text(EDITOR_THEME_DIR "/default.theme", &existing) != 0)
	{
		fs_write_text(EDITOR_THEME_DIR "/default.theme",
			"# TG11 editor theme file\n"
			"editor_header=0x0F\n"
			"editor_path=0x0B\n"
			"editor_rule=0x08\n"
			"editor_line=0x08\n"
			"editor_text=0x0F\n"
			"sh_keyword=0x0A\n"
			"sh_comment=0x08\n"
			"sh_string=0x0E\n"
			"basic_keyword=0x0D\n"
			"basic_comment=0x08\n"
			"basic_string=0x0B\n");
	}
	if (fs_read_text(EDITOR_THEME_DIR "/c64.theme", &existing) != 0)
	{
		fs_write_text(EDITOR_THEME_DIR "/c64.theme",
			"# Commodore-ish editor palette\n"
			"editor_header=0x1F\n"
			"editor_path=0x1E\n"
			"editor_rule=0x11\n"
			"editor_line=0x11\n"
			"editor_text=0x19\n"
			"sh_keyword=0x1A\n"
			"sh_comment=0x13\n"
			"sh_string=0x1E\n"
			"basic_keyword=0x1D\n"
			"basic_comment=0x13\n"
			"basic_string=0x1E\n");
	}

	{
		const char *current;
		if (fs_read_text(SYSTEM_THEME_CURRENT_PATH, &current) != 0)
		{
			fs_write_text(SYSTEM_THEME_CURRENT_PATH, "default");
		}
		if (fs_read_text(EDITOR_THEME_CURRENT_PATH, &current) != 0)
		{
			fs_write_text(EDITOR_THEME_CURRENT_PATH, "default");
		}
	}
}

static void load_current_system_theme(void)
{
	const char *current;
	if (fs_read_text(SYSTEM_THEME_CURRENT_PATH, &current) == 0)
	{
		if (apply_system_theme_by_name(current, 0) == 0) return;
	}
	apply_system_theme_by_name("default", 0);
}

static void load_current_editor_theme(void)
{
	const char *current;
	if (fs_read_text(EDITOR_THEME_CURRENT_PATH, &current) == 0)
	{
		if (apply_editor_theme_by_name(current, 0) == 0) return;
	}
	apply_editor_theme_by_name("default", 0);
}

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int expand_cp437_aliases(const char *in, char *out, unsigned long out_size)
{
	struct alias_item { const char *name; unsigned char v; };
	static const struct alias_item aliases[] = {
		{"boxh", 0xC4}, {"boxv", 0xB3}, {"boxul", 0xDA}, {"boxur", 0xBF},
		{"boxll", 0xC0}, {"boxlr", 0xD9}, {"boxt", 0xC2}, {"boxb", 0xC1},
		{"boxl", 0xC3}, {"boxr", 0xB4}, {"boxx", 0xC5},
		{"blkup", 0xDF}, {"blkdn", 0xDC}, {"blk", 0xDB},
		{"shade1", 0xB0}, {"shade2", 0xB1}, {"shade3", 0xB2},
		{"tri", 0x1E}, {"arru", 0x18}, {"arrd", 0x19}, {"arrl", 0x1B}, {"arrr", 0x1A}
	};
	unsigned long i = 0;
	unsigned long o = 0;

	if (in == (void *)0 || out == (void *)0 || out_size == 0) return -1;

	while (in[i] != '\0')
	{
		if (in[i] == '\\')
		{
			if (in[i + 1] == '\\')
			{
				if (o + 1 >= out_size) return -1;
				out[o++] = '\\';
				i += 2;
				continue;
			}
			if (in[i + 1] == 'x')
			{
				int h1 = hex_nibble(in[i + 2]);
				int h2 = hex_nibble(in[i + 3]);
				if (h1 >= 0 && h2 >= 0)
				{
					if (o + 1 >= out_size) return -1;
					out[o++] = (char)((h1 << 4) | h2);
					i += 4;
					continue;
				}
			}
			{
				unsigned long a;
				for (a = 0; a < sizeof(aliases) / sizeof(aliases[0]); a++)
				{
					unsigned long n = string_length(aliases[a].name);
					unsigned long j;
					int match = 1;
					for (j = 0; j < n; j++)
					{
						if (in[i + 1 + j] != aliases[a].name[j]) { match = 0; break; }
					}
					if (match)
					{
						if (o + 1 >= out_size) return -1;
						out[o++] = (char)aliases[a].v;
						i += 1 + n;
						goto alias_done;
					}
				}
			}
		}

		if (o + 1 >= out_size) return -1;
		out[o++] = in[i++];
		continue;

	alias_done:
		continue;
	}

	out[o] = '\0';
	return 0;
}

static unsigned long editor_visible_rows(void)
{
	unsigned long start_row = (unsigned long)(editor_vga_start / VGA_TEXT_WIDTH);
	if (start_row >= 25) return 1;
	return 25 - start_row;
}

static unsigned long editor_text_width(void)
{
	if (VGA_TEXT_WIDTH <= EDITOR_TEXT_GUTTER) return 1;
	return VGA_TEXT_WIDTH - EDITOR_TEXT_GUTTER;
}

static char editor_hex_digit(unsigned char v)
{
	static const char digits[] = "0123456789ABCDEF";
	return digits[v & 0x0F];
}

static int editor_hex_value(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static void editor_write_uint_right(unsigned short offset, unsigned long value, unsigned long width)
{
	unsigned long i;
	for (i = 0; i < width; i++) screen_write_char_at((unsigned short)(offset + i), ' ');
	if (width == 0) return;
	i = width;
	do
	{
		i--;
		screen_write_char_at((unsigned short)(offset + i), (char)('0' + (value % 10UL)));
		value /= 10UL;
	} while (i > 0 && value > 0);
}

static unsigned long editor_next_visual_row(unsigned long index)
{
	unsigned long col = 0;
	unsigned long width = editor_text_width();
	if (index >= editor_length) return editor_length;
	while (index < editor_length)
	{
		char c = editor_buffer[index++];
		if (c == '\n') return index;
		col++;
		if (col >= width) return index;
	}
	return editor_length;
}

static unsigned long editor_prev_visual_row(unsigned long index)
{
	unsigned long prev = 0;
	unsigned long next = 0;
	if (index == 0) return 0;
	if (index > editor_length) index = editor_length;
	while (next < index)
	{
		prev = next;
		next = editor_next_visual_row(next);
		if (next >= index) return prev;
	}
	return prev;
}

static unsigned long editor_visual_row_start(unsigned long index)
{
	unsigned long row_start = 0;
	unsigned long next;
	if (index > editor_length) index = editor_length;
	while (row_start < index)
	{
		next = editor_next_visual_row(row_start);
		if (next > index) break;
		row_start = next;
	}
	return row_start;
}

static unsigned long editor_visual_row_col(unsigned long index)
{
	unsigned long row_start;
	if (index > editor_length) index = editor_length;
	row_start = editor_visual_row_start(index);
	return index - row_start;
}

static unsigned long editor_visual_row_length(unsigned long row_start)
{
	unsigned long row_end;
	if (row_start >= editor_length) return 0;
	row_end = editor_next_visual_row(row_start);
	if (row_end > row_start && editor_buffer[row_end - 1] == '\n') return row_end - row_start - 1;
	return row_end - row_start;
}

static unsigned long editor_move_visual_rows(unsigned long index, unsigned long rows, int down)
{
	unsigned long col;
	unsigned long row_start;
	unsigned long row_len;
	unsigned long i;

	if (index > editor_length) index = editor_length;
	col = editor_visual_row_col(index);
	row_start = editor_visual_row_start(index);

	for (i = 0; i < rows; i++)
	{
		unsigned long next_start = down ? editor_next_visual_row(row_start) : editor_prev_visual_row(row_start);
		if (next_start == row_start) break;
		row_start = next_start;
	}

	row_len = editor_visual_row_length(row_start);
	if (col > row_len) col = row_len;
	return row_start + col;
}

static unsigned long editor_cursor_row_from_top(unsigned long top, unsigned long cursor)
{
	unsigned long row = 0;
	unsigned long col = 0;
	unsigned long i = top;
	unsigned long width = editor_text_width();
	if (cursor > editor_length) cursor = editor_length;
	if (top > editor_length) top = editor_length;
	while (i < cursor)
	{
		char c = editor_buffer[i++];
		if (c == '\n')
		{
			row++;
			col = 0;
			continue;
		}
		col++;
		if (col >= width)
		{
			row++;
			col = 0;
		}
	}
	return row;
}

static unsigned long editor_line_number_for_index(unsigned long index)
{
	unsigned long line = 1;
	unsigned long i;
	if (index > editor_length) index = editor_length;
	for (i = 0; i < index; i++)
	{
		if (editor_buffer[i] == '\n') line++;
	}
	return line;
}

static int editor_has_more_below(unsigned long top, unsigned long rows)
{
	unsigned long idx = top;
	unsigned long r = 0;
	if (top >= editor_length) return 0;
	while (r < rows && idx < editor_length)
	{
		idx = editor_next_visual_row(idx);
		r++;
	}
	return idx < editor_length;
}

static unsigned long editor_line_start(unsigned long index)
{
	if (index > editor_length) index = editor_length;
	while (index > 0 && editor_buffer[index - 1] != '\n') index--;
	return index;
}

static unsigned long editor_line_end(unsigned long index)
{
	if (index > editor_length) index = editor_length;
	while (index < editor_length && editor_buffer[index] != '\n') index++;
	return index;
}

static void editor_render(void)
{
	unsigned short clear_off;
	unsigned long rows;
	int can_scroll_up;
	int can_scroll_down;

	if (editor_cursor > editor_length) editor_cursor = editor_length;
	rows = editor_visible_rows();
	if (rows == 0) return;

	if (editor_hex_mode)
	{
		unsigned long row;
		unsigned long view_limit;
		if (editor_view_top > editor_length) editor_view_top = (editor_length / EDITOR_HEX_BYTES_PER_ROW) * EDITOR_HEX_BYTES_PER_ROW;
		rows = editor_visible_rows();
		if (editor_cursor < editor_view_top) editor_view_top = (editor_cursor / EDITOR_HEX_BYTES_PER_ROW) * EDITOR_HEX_BYTES_PER_ROW;
		view_limit = editor_view_top + rows * EDITOR_HEX_BYTES_PER_ROW;
		while (editor_cursor >= view_limit && rows > 0)
		{
			editor_view_top += EDITOR_HEX_BYTES_PER_ROW;
			view_limit = editor_view_top + rows * EDITOR_HEX_BYTES_PER_ROW;
		}
		can_scroll_up = (editor_view_top > 0);
		can_scroll_down = (editor_view_top + rows * EDITOR_HEX_BYTES_PER_ROW < editor_length);
		editor_draw_header(can_scroll_up, can_scroll_down);
		rows = editor_visible_rows();
		for (clear_off = editor_vga_start; clear_off < (VGA_TEXT_WIDTH * 25); clear_off++) screen_write_char_at(clear_off, ' ');
		for (row = 0; row < rows; row++)
		{
			unsigned long row_start = editor_view_top + row * EDITOR_HEX_BYTES_PER_ROW;
			unsigned short row_off = (unsigned short)(editor_vga_start + row * VGA_TEXT_WIDTH);
			unsigned long b;
			screen_write_char_at((unsigned short)(row_off + 0), editor_hex_digit((unsigned char)((row_start >> 12) & 0xF)));
			screen_write_char_at((unsigned short)(row_off + 1), editor_hex_digit((unsigned char)((row_start >> 8) & 0xF)));
			screen_write_char_at((unsigned short)(row_off + 2), editor_hex_digit((unsigned char)((row_start >> 4) & 0xF)));
			screen_write_char_at((unsigned short)(row_off + 3), editor_hex_digit((unsigned char)(row_start & 0xF)));
			screen_write_char_at((unsigned short)(row_off + 4), ':');
			screen_write_char_at((unsigned short)(row_off + 5), ' ');
			screen_write_char_at((unsigned short)(row_off + 54), '|');
			screen_write_char_at((unsigned short)(row_off + 71), '|');
			for (b = 0; b < EDITOR_HEX_BYTES_PER_ROW; b++)
			{
				unsigned short hex_off = (unsigned short)(row_off + EDITOR_HEX_DATA_COL + b * 3);
				unsigned short ascii_off = (unsigned short)(row_off + EDITOR_HEX_ASCII_COL + b);
				if (row_start + b < editor_length)
				{
					unsigned char byte = (unsigned char)editor_buffer[row_start + b];
					screen_write_char_at(hex_off, editor_hex_digit((unsigned char)(byte >> 4)));
					screen_write_char_at((unsigned short)(hex_off + 1), editor_hex_digit(byte));
					screen_write_char_at((unsigned short)(hex_off + 2), ' ');
					screen_write_char_at(ascii_off, (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.');
				}
				else
				{
					screen_write_char_at(hex_off, ' ');
					screen_write_char_at((unsigned short)(hex_off + 1), ' ');
					screen_write_char_at((unsigned short)(hex_off + 2), ' ');
					screen_write_char_at(ascii_off, ' ');
				}
			}
		}
		editor_prev_end = (unsigned short)(editor_vga_start + (rows - 1) * VGA_TEXT_WIDTH);
		{
			unsigned long r = (editor_cursor - editor_view_top) / EDITOR_HEX_BYTES_PER_ROW;
			unsigned long c = (editor_cursor - editor_view_top) % EDITOR_HEX_BYTES_PER_ROW;
			unsigned short cursor_off;
			if (r >= rows) r = rows - 1;
			if (c >= EDITOR_HEX_BYTES_PER_ROW) c = EDITOR_HEX_BYTES_PER_ROW - 1;
			cursor_off = (unsigned short)(editor_vga_start + r * VGA_TEXT_WIDTH + EDITOR_HEX_DATA_COL + c * 3 + editor_hex_nibble);
			if (cursor_off >= VGA_TEXT_WIDTH * 25) cursor_off = (unsigned short)(VGA_TEXT_WIDTH * 25 - 1);
			screen_set_hw_cursor(cursor_off);
			screen_set_pos(cursor_off);
		}
		return;
	}

	if (editor_view_top > editor_length) editor_view_top = editor_length;
	rows = editor_visible_rows();
	if (editor_cursor < editor_view_top) editor_view_top = editor_visual_row_start(editor_cursor);
	while (editor_cursor_row_from_top(editor_view_top, editor_cursor) >= rows)
	{
		unsigned long next_top = editor_next_visual_row(editor_view_top);
		if (next_top == editor_view_top) break;
		editor_view_top = next_top;
	}

	can_scroll_up = (editor_view_top > 0);
	can_scroll_down = editor_has_more_below(editor_view_top, rows);
	editor_draw_header(can_scroll_up, can_scroll_down);
	rows = editor_visible_rows();
	for (clear_off = editor_vga_start; clear_off < (VGA_TEXT_WIDTH * 25); clear_off++) screen_write_char_at(clear_off, ' ');
	{
		unsigned long row = 0;
		unsigned long row_start = editor_view_top;
		unsigned long width = editor_text_width();
		static const char *const sh_keywords[] = {
			"if", "then", "else", "fi", "for", "do", "done", "while", "in", "case", "esac", "function", "echo", "exit", "cd", "ls"
		};
		static const char *const basic_keywords[] = {
			"PRINT", "LET", "IF", "THEN", "GOTO", "GOSUB", "RETURN", "FOR", "TO", "STEP", "NEXT", "INPUT", "REM", "END", "STOP", "LIST", "RUN"
		};
		while (row < rows)
		{
			unsigned short row_off = (unsigned short)(editor_vga_start + row * VGA_TEXT_WIDTH);
			unsigned long row_end = row_start;
			unsigned long col = 0;
			int in_comment = 0;
			int in_string = 0;
			unsigned long token_remaining = 0;
			unsigned char token_color = editor_text_color;
			if (row_start == 0 || (row_start <= editor_length && editor_buffer[row_start - 1] == '\n'))
			{
				screen_set_color(editor_line_number_color);
				editor_write_uint_right(row_off, editor_line_number_for_index(row_start), EDITOR_TEXT_GUTTER - 2);
			}
			screen_set_color(editor_rule_color);
			screen_write_char_at((unsigned short)(row_off + EDITOR_TEXT_GUTTER - 2), '|');
			screen_write_char_at((unsigned short)(row_off + EDITOR_TEXT_GUTTER - 1), ' ');
			while (row_end < editor_length && col < width)
			{
				char ch = editor_buffer[row_end];
				unsigned char ch_color = editor_text_color;
				if (ch == '\n')
				{
					row_end++;
					break;
				}

				if (editor_language == EDITOR_LANG_SH)
				{
					if (in_comment)
					{
						ch_color = editor_sh_comment_color;
					}
					else if (in_string)
					{
						ch_color = editor_sh_string_color;
						if (ch == '"') in_string = 0;
					}
					else if (token_remaining > 0)
					{
						ch_color = token_color;
						token_remaining--;
					}
					else if (ch == '#')
					{
						in_comment = 1;
						ch_color = editor_sh_comment_color;
					}
					else if (ch == '"')
					{
						in_string = 1;
						ch_color = editor_sh_string_color;
					}
					else if ((row_end == row_start || !char_is_word(editor_buffer[row_end - 1])) && char_is_word(ch))
					{
						unsigned long tlen = 0;
						while (row_end + tlen < editor_length && char_is_word(editor_buffer[row_end + tlen])) tlen++;
						if (token_is_keyword_ci(&editor_buffer[row_end], tlen, sh_keywords, sizeof(sh_keywords) / sizeof(sh_keywords[0])))
						{
							ch_color = editor_sh_keyword_color;
							token_color = editor_sh_keyword_color;
							if (tlen > 0) token_remaining = tlen - 1;
						}
					}
				}
				else if (editor_language == EDITOR_LANG_BASIC)
				{
					if (in_comment)
					{
						ch_color = editor_basic_comment_color;
					}
					else if (in_string)
					{
						ch_color = editor_basic_string_color;
						if (ch == '"') in_string = 0;
					}
					else if (token_remaining > 0)
					{
						ch_color = token_color;
						token_remaining--;
					}
					else if (ch == '\'')
					{
						in_comment = 1;
						ch_color = editor_basic_comment_color;
					}
					else if (ch == '"')
					{
						in_string = 1;
						ch_color = editor_basic_string_color;
					}
					else if ((row_end == row_start || !char_is_word(editor_buffer[row_end - 1])) && char_is_word(ch))
					{
						unsigned long tlen = 0;
						unsigned long rem_len = 0;
						while (row_end + tlen < editor_length && char_is_word(editor_buffer[row_end + tlen])) tlen++;
						if (starts_with_word_ci(&editor_buffer[row_end], "REM", &rem_len))
						{
							in_comment = 1;
							ch_color = editor_basic_comment_color;
							token_color = editor_basic_comment_color;
							if (rem_len > 0) token_remaining = rem_len - 1;
						}
						else if (token_is_keyword_ci(&editor_buffer[row_end], tlen, basic_keywords, sizeof(basic_keywords) / sizeof(basic_keywords[0])))
						{
							ch_color = editor_basic_keyword_color;
							token_color = editor_basic_keyword_color;
							if (tlen > 0) token_remaining = tlen - 1;
						}
					}
				}

				screen_set_color(ch_color);
				screen_write_char_at((unsigned short)(row_off + EDITOR_TEXT_GUTTER + col), ch);
				row_end++;
				col++;
			}
			if (row_end >= editor_length && row_start >= editor_length && row > 0) break;
			if (row_end == row_start && row_start >= editor_length)
			{
				row++;
				continue;
			}
			row_start = row_end;
			row++;
		}
	}
	editor_prev_end = (unsigned short)(editor_vga_start + (rows - 1) * VGA_TEXT_WIDTH);
	{
		unsigned long r = editor_cursor_row_from_top(editor_view_top, editor_cursor);
		unsigned long c = editor_visual_row_col(editor_cursor);
		unsigned short cursor_off;
		if (r >= rows) r = rows - 1;
		if (c >= editor_text_width()) c = editor_text_width() - 1;
		cursor_off = (unsigned short)(editor_vga_start + r * VGA_TEXT_WIDTH + EDITOR_TEXT_GUTTER + c);
		if (cursor_off >= VGA_TEXT_WIDTH * 25) cursor_off = (unsigned short)(VGA_TEXT_WIDTH * 25 - 1);
		screen_set_hw_cursor(cursor_off);
		screen_set_pos(cursor_off);
	}
}

static int execute_substitution_command(const char *raw_cmd, char *out, unsigned long out_size)
{
	char cmdline[128];
	char op[16];
	char arg[96];
	const char *p;
	unsigned long i = 0;
	unsigned long n;

	if (raw_cmd == (void *)0 || out == (void *)0 || out_size == 0) return -1;

	p = skip_spaces(raw_cmd);
	while (p[i] != '\0' && i + 1 < sizeof(cmdline))
	{
		cmdline[i] = p[i];
		i++;
	}
	cmdline[i] = '\0';

	n = string_length(cmdline);
	while (n > 0 && (cmdline[n - 1] == ' ' || cmdline[n - 1] == '\t'))
	{
		cmdline[n - 1] = '\0';
		n--;
	}

	p = read_token(cmdline, op, sizeof(op));
	if (p == (void *)0 || op[0] == '\0') return -1;

	for (i = 0; i < (unsigned long)script_var_count; i++)
	{
		if (string_equals(script_var_names[i], op))
		{
			unsigned long j = 0;
			while (script_var_values[i][j] != '\0' && j + 1 < out_size)
			{
				out[j] = script_var_values[i][j];
				j++;
			}
			out[j] = '\0';
			return 0;
		}
	}

	if (string_equals(op, "version"))
	{
		i = 0;
		while (TG11_OS_VERSION[i] != '\0' && i + 1 < out_size)
		{
			out[i] = TG11_OS_VERSION[i];
			i++;
		}
		out[i] = '\0';
		return 0;
	}

	if (string_equals(op, "pwd"))
	{
		if (fat_mode_active())
		{
			i = 0;
			while (fat_cwd[i] != '\0' && i + 1 < out_size)
			{
				out[i] = fat_cwd[i];
				i++;
			}
			out[i] = '\0';
			return 0;
		}
		else
		{
			char cwd[256];
			fs_get_pwd(cwd, sizeof(cwd));
			i = 0;
			while (cwd[i] != '\0' && i + 1 < out_size)
			{
				out[i] = cwd[i];
				i++;
			}
			out[i] = '\0';
			return 0;
		}
	}

	if (string_equals(op, "cat"))
	{
		p = read_token(p, arg, sizeof(arg));
		if (p == (void *)0 || arg[0] == '\0') return -1;

		if (fat_mode_active())
		{
			char full_path[128];
			unsigned char data[1024];
			unsigned long size;
			if (fat_resolve_path(arg, full_path, sizeof(full_path)) != 0) return -1;
			if (fat32_read_file_path(full_path, data, sizeof(data), &size) != 0) return -1;
			for (i = 0; i < size && i + 1 < out_size; i++) out[i] = (char)data[i];
			out[i] = '\0';
			return 0;
		}
		else
		{
			const char *text;
			if (fs_read_text(arg, &text) != 0) return -1;
			i = 0;
			while (text[i] != '\0' && i + 1 < out_size)
			{
				out[i] = text[i];
				i++;
			}
			out[i] = '\0';
			return 0;
		}
	}

	return -1;
}

static void script_set_var(const char *name, const char *value)
{
	int slot = -1;
	int i;
	unsigned long j;

	if (name == (void *)0 || value == (void *)0 || name[0] == '\0') return;

	for (i = 0; i < script_var_count; i++)
	{
		if (string_equals(script_var_names[i], name))
		{
			slot = i;
			break;
		}
	}

	if (slot < 0)
	{
		if (script_var_count >= SCRIPT_VAR_MAX) return;
		slot = script_var_count++;
		j = 0;
		while (name[j] != '\0' && j + 1 < sizeof(script_var_names[slot]))
		{
			script_var_names[slot][j] = name[j];
			j++;
		}
		script_var_names[slot][j] = '\0';
	}

	j = 0;
	while (value[j] != '\0' && j + 1 < sizeof(script_var_values[slot]))
	{
		script_var_values[slot][j] = value[j];
		j++;
	}
	script_var_values[slot][j] = '\0';
}

static int expand_command_substitutions(const char *in, char *out, unsigned long out_size)
{
	unsigned long i = 0;
	unsigned long o = 0;

	if (in == (void *)0 || out == (void *)0 || out_size == 0) return -1;

	while (in[i] != '\0')
	{
		if (in[i] == '$' && in[i + 1] == '(')
		{
			char inner[128];
			char value[256];
			unsigned long j = i + 2;
			unsigned long k = 0;

			while (in[j] != '\0' && in[j] != ')')
			{
				if (k + 1 >= sizeof(inner)) return -1;
				inner[k++] = in[j++];
			}
			if (in[j] != ')') return -1;
			inner[k] = '\0';

			if (execute_substitution_command(inner, value, sizeof(value)) != 0) return -1;

			k = 0;
			while (value[k] != '\0')
			{
				if (o + 1 >= out_size) return -1;
				out[o++] = value[k++];
			}
			i = j + 1;
			continue;
		}

		if (o + 1 >= out_size) return -1;
		out[o++] = in[i++];
	}

	out[o] = '\0';
	return 0;
}

/* ================================================================== */
/* History                                                            */
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
/* Tab completion                                                     */
/* ================================================================== */

static const char * const cmd_list[] = {
	"help", "version", "echo", "glyph", "charmap", "color", "themes", "theme", "ethemes", "etheme", "clear", "reboot",
	"quit", "exit", "shutdown",
	"pwd", "ls", "cd", "mkdir", "touch", "write", "cat", "rm", "cp", "mv", "edit", "hexedit", "run", "basic",
	"hexdump", "memmap", "ataid", "readsec", "writesec", "fatmount", "ramfs", "fatunmount", "fatls", "fatcat", "fattouch", "fatwrite", "fatrm", (void *)0
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
		screen_set_color(terminal_prompt_color); terminal_write("> "); screen_set_color(terminal_text_color);
		prompt_vga_start = screen_get_pos();
		for (j = 0; j < input_length; j++) screen_write_char_at((unsigned short)(prompt_vga_start + j), input_buffer[j]);
		cursor_pos = input_length; sync_screen_pos();
		screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
	}
}

/* ================================================================== */
/* Scancode → character tables                                        */
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
/* Backspace (cursor-aware)                                           */
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
/* Commands                                                           */
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
	arch_outb(0x64, 0xFE);  /* Fallback: 8042 reset line         */
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

static void cmd_pwd(void)
{
	if (fat_mode_active())
	{
		terminal_write_line(fat_cwd);
		return;
	}
	else
	{
		char path[256];
		fs_get_pwd(path, sizeof(path));
		terminal_write_line(path);
	}
}

static void cmd_ls(const char *args)
{
	char target[128];
	int count;
	int i;
	const char *p = skip_spaces(args);

	target[0] = '\0';
	if (*p != '\0')
	{
		p = read_token(p, target, sizeof(target));
		if (p == (void *)0)
		{
			terminal_write_line("ls: path too long");
			return;
		}
	}

	if (fat_mode_active())
	{
		char fat_names[64][40];
		char full_path[128];
		int fat_count;
		if (fat_resolve_path(target[0] == '\0' ? "." : target, full_path, sizeof(full_path)) != 0)
		{
			terminal_write_line("ls: invalid FAT path");
			return;
		}

		if (fat32_ls_path(full_path, fat_names, 64, &fat_count) != 0)
		{
			terminal_write_line("ls: invalid path");
			return;
		}
		for (i = 0; i < fat_count; i++) terminal_write_line(fat_names[i]);
		return;
	}

	{
		char names[FS_MAX_LIST][FS_NAME_MAX + 2];
		int types[FS_MAX_LIST];
	if (fs_ls(target[0] == '\0' ? (void *)0 : target, names, types, FS_MAX_LIST, &count) != 0)
	{
		terminal_write_line("ls: invalid path");
		return;
	}

	for (i = 0; i < count; i++)
	{
		(void)types[i];
		terminal_write_line(names[i]);
	}
	}
}

static void cmd_cd(const char *args)
{
	char path[128];
	char full_path[128];
	char names[1][40];
	int count = 0;
	const char *p = read_token(args, path, sizeof(path));
	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: cd <path>");
		return;
	}
	if (fat_mode_active())
	{
		if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0 || fat32_ls_path(full_path, names, 1, &count) != 0)
		{
			terminal_write_line("cd: invalid directory");
			return;
		}
		{
			unsigned long i = 0;
			while (full_path[i] != '\0' && i + 1 < sizeof(fat_cwd)) { fat_cwd[i] = full_path[i]; i++; }
			fat_cwd[i] = '\0';
		}
		return;
	}
	if (fs_cd(path) != 0) terminal_write_line("cd: invalid directory");
}

static void cmd_mkdir(const char *args)
{
	char path[128];
	char full_path[128];
	const char *p = read_token(args, path, sizeof(path));
	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: mkdir <path>");
		return;
	}
	if (fat_mode_active())
	{
		if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0 || fat32_mkdir_path(full_path) != 0)
		{
			terminal_write_line("mkdir: failed");
		}
		return;
	}
	if (fs_mkdir(path) != 0) terminal_write_line("mkdir: failed");
}

static void cmd_touch(const char *args)
{
	char path[128];
	char full_path[128];
	const char *p = read_token(args, path, sizeof(path));
	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: touch <path>");
		return;
	}
	if (fat_mode_active())
	{
		if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0 || fat32_touch_file_path(full_path) != 0)
		{
			terminal_write_line("touch: failed");
		}
		return;
	}
	if (fs_touch(path) != 0) terminal_write_line("touch: failed");
}

static void cmd_cat(const char *args)
{
	char path[128];
	char full_path[128];
	const char *text;
	const char *p = read_token(args, path, sizeof(path));
	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: cat <path>");
		return;
	}

	if (fat_mode_active())
	{
		unsigned long i;
		unsigned char data[2048];
		unsigned long size;
		if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0)
		{
			terminal_write_line("cat: failed");
			return;
		}
		if (fat32_read_file_path(full_path, data, sizeof(data) - 1, &size) == 0)
		{
			for (i = 0; i < size; i++)
			{
				unsigned char b = data[i];
				terminal_putc((b >= 0x20 && b < 0x7F) || b == '\n' || b == '\r' || b == '\t' ? (char)b : '.');
			}
			terminal_putc('\n');
			return;
		}
		terminal_write_line("cat: failed");
		return;
	}

	if (fs_read_text(path, &text) != 0)
	{
		terminal_write_line("cat: failed");
		return;
	}
	terminal_write_line(text);
}

static void cmd_write(const char *args)
{
	char path[128];
	char full_path[128];
	const char *p = read_token(args, path, sizeof(path));
	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: write <path> <text>");
		return;
	}
	p = skip_spaces(p);
	if (*p == '\0')
	{
		terminal_write_line("Usage: write <path> <text>");
		return;
	}
	if (fat_mode_active())
	{
		if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0 || fat32_write_file_path(full_path, (const unsigned char *)p, string_length(p)) != 0)
		{
			terminal_write_line("write: failed");
		}
		return;
	}
	if (fs_write_text(path, p) != 0) terminal_write_line("write: failed");
}

static void cmd_rm(const char *args)
{
	char path[128];
	char full_path[128];
	const char *p = read_token(args, path, sizeof(path));
	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: rm <path>");
		return;
	}
	if (fat_mode_active())
	{
		if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0 || fat32_remove_path(full_path) != 0)
		{
			terminal_write_line("rm: failed (non-empty dir?)");
		}
		return;
	}
	if (fs_rm(path) != 0) terminal_write_line("rm: failed (non-empty dir?)");
}

static void cmd_cp(const char *args)
{
	char src[128];
	char dst[128];
	char src_full[128];
	char dst_full[128];
	const char *p = read_token(args, src, sizeof(src));
	if (p == (void *)0 || src[0] == '\0')
	{
		terminal_write_line("Usage: cp <src> <dst>");
		return;
	}
	p = read_token(p, dst, sizeof(dst));
	if (p == (void *)0 || dst[0] == '\0')
	{
		terminal_write_line("Usage: cp <src> <dst>");
		return;
	}
	if (fat_mode_active())
	{
		unsigned char data[4096];
		unsigned long size;
		if (fat_resolve_path(src, src_full, sizeof(src_full)) != 0 || fat_resolve_path(dst, dst_full, sizeof(dst_full)) != 0)
		{
			terminal_write_line("cp: failed");
			return;
		}
		if (fat32_read_file_path(src_full, data, sizeof(data), &size) != 0 || fat32_write_file_path(dst_full, data, size) != 0)
		{
			terminal_write_line("cp: failed");
		}
		return;
	}
	if (fs_cp(src, dst) != 0) terminal_write_line("cp: failed");
}

static void cmd_mv(const char *args)
{
	char src[128];
	char dst[128];
	char src_full[128];
	char dst_full[128];
	const char *p = read_token(args, src, sizeof(src));
	if (p == (void *)0 || src[0] == '\0')
	{
		terminal_write_line("Usage: mv <src> <dst>");
		return;
	}
	p = read_token(p, dst, sizeof(dst));
	if (p == (void *)0 || dst[0] == '\0')
	{
		terminal_write_line("Usage: mv <src> <dst>");
		return;
	}
	if (fat_mode_active())
	{
		unsigned char data[4096];
		unsigned long size;
		if (fat_resolve_path(src, src_full, sizeof(src_full)) != 0 || fat_resolve_path(dst, dst_full, sizeof(dst_full)) != 0)
		{
			terminal_write_line("mv: failed");
			return;
		}
		if (fat32_read_file_path(src_full, data, sizeof(data), &size) != 0 || fat32_write_file_path(dst_full, data, size) != 0 || fat32_remove_path(src_full) != 0)
		{
			terminal_write_line("mv: failed");
		}
		return;
	}
	if (fs_mv(src, dst) != 0) terminal_write_line("mv: failed");
}

static int editor_save(void)
{
	int rc;
	if (editor_use_fat)
	{
		rc = fat32_write_file_path(editor_target_path, (const unsigned char *)editor_buffer, editor_length);
		if (rc == 0) editor_dirty = 0;
		return rc;
	}

	rc = fs_write_file(editor_target_path, (const unsigned char *)editor_buffer, editor_length);
	if (rc == 0) editor_dirty = 0;
	return rc;
}

static void editor_draw_header(int can_scroll_up, int can_scroll_down)
{
	screen_set_pos(0);
	screen_set_hw_cursor(0);
	screen_set_color(editor_header_color);
	terminal_write("TG11 Editor ");
	terminal_write(editor_hex_mode ? "[hex]" : "[text]");
	terminal_write(" [");
	terminal_putc(can_scroll_up ? '^' : ' ');
	terminal_putc('/');
	terminal_putc(can_scroll_down ? 'v' : ' ');
	terminal_write("]");
	if (editor_dirty) terminal_write(" [modified]");
	terminal_putc('\n');
	screen_set_color(editor_path_color);
	terminal_write("Path: ");
	terminal_write_line(editor_target_path);
	screen_set_color(editor_header_color);
	if (editor_hex_mode) terminal_write_line("Hex: 0-9/A-F edit | arrows PgUp PgDn Home End | Ctrl+Home/End");
	else terminal_write_line("Ctrl+S save | F10 save+exit | Esc cancel | PgUp PgDn Home End");
	screen_set_color(editor_rule_color);
	terminal_write_line("--------------------------------");
	screen_set_color(editor_text_color);
	editor_vga_start = screen_get_pos();
	editor_prev_end = editor_vga_start;
}

static int editor_open_file(const char *path, int hex_mode)
{
	unsigned long size = 0;
	unsigned long i;
	editor_use_fat = fat_mode_active();
	editor_hex_mode = hex_mode;
	editor_hex_nibble = 0;
	editor_language = editor_detect_language_from_path(path);

	if (editor_use_fat)
	{
		if (fat_resolve_path(path, editor_target_path, sizeof(editor_target_path)) != 0) return -1;
	}
	else
	{
		for (i = 0; path[i] != '\0' && i + 1 < sizeof(editor_target_path); i++) editor_target_path[i] = path[i];
		editor_target_path[i] = '\0';
	}

	editor_length = 0;
	editor_dirty = 0;
	if (hex_mode)
	{
		if (editor_use_fat)
		{
			if (fat32_read_file_path(editor_target_path, (unsigned char *)editor_buffer, EDITOR_BUFFER_SIZE, &size) == 0) editor_length = size;
		}
		else
		{
			if (fs_read_file(editor_target_path, (unsigned char *)editor_buffer, EDITOR_BUFFER_SIZE, &size) == 0) editor_length = size;
		}
	}
	else
	{
		if (editor_use_fat)
		{
			if (fat32_read_file_path(editor_target_path, (unsigned char *)editor_buffer, EDITOR_BUFFER_SIZE - 1, &size) == 0) editor_length = size;
		}
		else
		{
			if (fs_read_file(editor_target_path, (unsigned char *)editor_buffer, EDITOR_BUFFER_SIZE - 1, &size) == 0) editor_length = size;
		}
		editor_buffer[editor_length] = '\0';
	}

	screen_clear();
	editor_cursor = editor_length;
	editor_view_top = 0;
	editor_render();
	editor_active = 1;
	return 0;
}

static void cmd_edit(const char *args)
{
	char path[128];
	const char *p = read_token(args, path, sizeof(path));
	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: edit <path>");
		return;
	}
	if (editor_open_file(path, 0) != 0)
	{
		terminal_write_line(fat_mode_active() ? "edit: invalid FAT path" : "edit: failed");
	}
}

static void cmd_hexedit(const char *args)
{
	char path[128];
	const char *p = read_token(args, path, sizeof(path));
	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: hexedit <path>");
		return;
	}
	if (editor_open_file(path, 1) != 0)
	{
		terminal_write_line(fat_mode_active() ? "hexedit: invalid FAT path" : "hexedit: failed");
	}
}

static void editor_status_line(const char *msg)
{
	screen_set_pos(editor_prev_end);
	screen_set_hw_cursor(editor_prev_end);
	terminal_putc('\n');
	terminal_write_line(msg);
	editor_render();
}

static void run_inline_command(const char *line)
{
	unsigned long n = 0;
	unsigned long i;

	while (line[n] != '\0' && n + 1 < INPUT_BUFFER_SIZE)
	{
		input_buffer[n] = line[n];
		n++;
	}
	input_buffer[n] = '\0';
	input_length = n;
	cursor_pos = n;

	for (i = 0; i < n; i++)
	{
		if (input_buffer[i] != ' ' && input_buffer[i] != '\t')
		{
			run_command();
			return;
		}
	}
}

static void trim_spaces_inplace(char *s)
{
	unsigned long start = 0;
	unsigned long end;
	unsigned long i;

	while (s[start] == ' ' || s[start] == '\t') start++;
	end = string_length(s);
	while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;

	if (start > 0)
	{
		for (i = 0; start + i < end; i++) s[i] = s[start + i];
		s[i] = '\0';
	}
	else
	{
		s[end] = '\0';
	}
}

static void unquote_token(char *s)
{
	unsigned long n = string_length(s);
	unsigned long i;
	if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\'')))
	{
		for (i = 1; i + 1 < n; i++) s[i - 1] = s[i];
		s[n - 2] = '\0';
	}
}

static int resolve_script_value(const char *token, char *out, unsigned long out_size)
{
	char tmp[96];
	unsigned long i = 0;
	unsigned long n;

	if (token == (void *)0 || out == (void *)0 || out_size == 0) return -1;
	while (token[i] != '\0' && i + 1 < sizeof(tmp))
	{
		tmp[i] = token[i];
		i++;
	}
	tmp[i] = '\0';
	unquote_token(tmp);

	n = string_length(tmp);
	if (n >= 4 && tmp[0] == '$' && tmp[1] == '(' && tmp[n - 1] == ')')
	{
		tmp[n - 1] = '\0';
		return execute_substitution_command(&tmp[2], out, out_size);
	}

	i = 0;
	while (tmp[i] != '\0' && i + 1 < out_size)
	{
		out[i] = tmp[i];
		i++;
	}
	out[i] = '\0';
	return 0;
}

static int eval_script_condition(const char *expr)
{
	char work[128];
	char lhs_raw[64];
	char lhs[64];
	char op[8];
	char rhs_raw[64];
	char rhs[64];
	const char *p;
	unsigned long i = 0;

	while (expr[i] != '\0' && i + 1 < sizeof(work))
	{
		work[i] = expr[i];
		i++;
	}
	work[i] = '\0';
	trim_spaces_inplace(work);

	if (work[0] == '(')
	{
		unsigned long n = string_length(work);
		if (n >= 2 && work[n - 1] == ')')
		{
			unsigned long j;
			for (j = 1; j + 1 < n; j++) work[j - 1] = work[j];
			work[n - 2] = '\0';
			trim_spaces_inplace(work);
		}
	}

	p = read_token(work, lhs_raw, sizeof(lhs_raw));
	if (p == (void *)0 || lhs_raw[0] == '\0') return 0;
	p = read_token(p, op, sizeof(op));
	if (p == (void *)0 || op[0] == '\0') return 0;
	p = read_token(p, rhs_raw, sizeof(rhs_raw));
	if (p == (void *)0 || rhs_raw[0] == '\0') return 0;

	if (resolve_script_value(lhs_raw, lhs, sizeof(lhs)) != 0) return 0;
	if (resolve_script_value(rhs_raw, rhs, sizeof(rhs)) != 0) return 0;

	if (string_equals(op, "==")) return string_equals(lhs, rhs);
	if (string_equals(op, "!=")) return !string_equals(lhs, rhs);
	return 0;
}

static void cmd_run(const char *args)
{
	struct run_if_state
	{
		int parent_exec;
		int branch_taken;
		int executing;
	};
	struct run_if_state if_stack[8];
	int if_depth = 0;
	int current_exec = 1;
	int trace = 0;
	int old_script_var_count;
	char old_script_var_names[SCRIPT_VAR_MAX][16];
	char old_script_var_values[SCRIPT_VAR_MAX][96];
	char path[128];
	char resolved[128];
	char script[EDITOR_BUFFER_SIZE];
	unsigned long script_len = 0;
	unsigned long i;
	unsigned long line_len = 0;
	char line[INPUT_BUFFER_SIZE];
	char first[32];
	const char *p = read_token(args, first, sizeof(first));

	if (p != (void *)0 && string_equals(first, "-x"))
	{
		trace = 1;
		p = read_token(p, path, sizeof(path));
	}
	else
	{
		unsigned long c = 0;
		while (first[c] != '\0' && c + 1 < sizeof(path)) { path[c] = first[c]; c++; }
		path[c] = '\0';
	}

	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: run [-x] <path>");
		return;
	}

	if (script_depth >= 3)
	{
		terminal_write_line("run: max script depth reached");
		return;
	}

	old_script_var_count = script_var_count;
	for (i = 0; i < (unsigned long)SCRIPT_VAR_MAX; i++)
	{
		unsigned long j = 0;
		while (j < sizeof(old_script_var_names[0]))
		{
			old_script_var_names[i][j] = script_var_names[i][j];
			j++;
		}
		j = 0;
		while (j < sizeof(old_script_var_values[0]))
		{
			old_script_var_values[i][j] = script_var_values[i][j];
			j++;
		}
	}

	if (fat_mode_active())
	{
		if (fat_resolve_path(path, resolved, sizeof(resolved)) != 0)
		{
			terminal_write_line("run: invalid FAT path");
			return;
		}
		if (fat32_read_file_path(resolved, (unsigned char *)script, sizeof(script) - 1, &script_len) != 0)
		{
			terminal_write_line("run: read failed");
			return;
		}
		script[script_len] = '\0';
	}
	else
	{
		const char *text;
		if (fs_read_text(path, &text) != 0)
		{
			terminal_write_line("run: read failed");
			return;
		}
		while (text[script_len] != '\0' && script_len + 1 < sizeof(script))
		{
			script[script_len] = text[script_len];
			script_len++;
		}
		script[script_len] = '\0';
	}

	script_mode_active = 1;
	script_depth++;

	for (i = 0; i <= script_len; i++)
	{
		char c = script[i];
		if (c == '\r') continue;
		if (c != '\n' && c != '\0')
		{
			if (line_len + 1 < sizeof(line)) line[line_len++] = c;
			continue;
		}

		line[line_len] = '\0';
		if (line_len > 0)
		{
			unsigned long start = 0;
			while (line[start] == ' ' || line[start] == '\t') start++;
			if (line[start] != '\0' && line[start] != '#')
			{
				char *cmd = &line[start];
				trim_spaces_inplace(cmd);
				if (string_starts_with(cmd, "if "))
				{
					int cond;
					if (if_depth >= 8)
					{
						terminal_write_line("run: if nesting too deep");
						line_len = 0;
						break;
					}
					cond = eval_script_condition(cmd + 3);
					if_stack[if_depth].parent_exec = current_exec;
					if_stack[if_depth].branch_taken = current_exec && cond;
					if_stack[if_depth].executing = current_exec && cond;
					current_exec = if_stack[if_depth].executing;
					if_depth++;
				}
				else if (string_starts_with(cmd, "elif "))
				{
					if (if_depth == 0)
					{
						terminal_write_line("run: elif without if");
					}
					else
					{
						struct run_if_state *st = &if_stack[if_depth - 1];
						if (!st->parent_exec || st->branch_taken)
						{
							st->executing = 0;
							current_exec = 0;
						}
						else
						{
							int cond = eval_script_condition(cmd + 5);
							st->executing = cond;
							st->branch_taken = cond;
							current_exec = st->executing;
						}
					}
				}
				else if (string_equals(cmd, "else"))
				{
					if (if_depth == 0)
					{
						terminal_write_line("run: else without if");
					}
					else
					{
						struct run_if_state *st = &if_stack[if_depth - 1];
						if (!st->parent_exec || st->branch_taken)
						{
							st->executing = 0;
							current_exec = 0;
						}
						else
						{
							st->executing = 1;
							st->branch_taken = 1;
							current_exec = 1;
						}
					}
				}
				else if (string_starts_with(cmd, "foreach ") || string_starts_with(cmd, "for "))
				{
					char var_name[16];
					char items[128];
					char body[INPUT_BUFFER_SIZE];
					char *cur;
					char *sep;
					const char *rest;
					if (!current_exec) { line_len = 0; continue; }

					rest = string_starts_with(cmd, "foreach ") ? (cmd + 8) : (cmd + 4);
					rest = read_token(rest, var_name, sizeof(var_name));
					if (rest == (void *)0 || var_name[0] == '\0')
					{
						terminal_write_line("run: bad foreach syntax");
						line_len = 0;
						continue;
					}
					rest = read_token(rest, items, sizeof(items));
					if (rest == (void *)0 || !string_equals(items, "in"))
					{
						terminal_write_line("run: use 'foreach <var> in <a,b,..> do <cmd>'");
						line_len = 0;
						continue;
					}
					rest = read_token(rest, items, sizeof(items));
					if (rest == (void *)0 || items[0] == '\0')
					{
						terminal_write_line("run: missing foreach items");
						line_len = 0;
						continue;
					}
					rest = read_token(rest, body, sizeof(body));
					if (rest == (void *)0 || !string_equals(body, "do"))
					{
						terminal_write_line("run: missing 'do' in foreach");
						line_len = 0;
						continue;
					}
					rest = skip_spaces(rest);
					if (*rest == '\0')
					{
						terminal_write_line("run: missing foreach body command");
						line_len = 0;
						continue;
					}
					{
						unsigned long bi = 0;
						while (rest[bi] != '\0' && bi + 1 < sizeof(body))
						{
							body[bi] = rest[bi];
							bi++;
						}
						body[bi] = '\0';
					}

					cur = items;
					for (;;)
					{
						char item_val[96];
						char exec_line[INPUT_BUFFER_SIZE];
						unsigned long vi = 0;
						sep = cur;
						while (*sep != '\0' && *sep != ',') sep++;
						while (*cur == ' ' || *cur == '\t') cur++;
						while (cur + vi < sep && vi + 1 < sizeof(item_val))
						{
							item_val[vi] = cur[vi];
							vi++;
						}
						item_val[vi] = '\0';
						trim_spaces_inplace(item_val);
						if (item_val[0] != '\0')
						{
							script_set_var(var_name, item_val);
							{
								unsigned long ei = 0;
								while (body[ei] != '\0' && ei + 1 < sizeof(exec_line))
								{
									exec_line[ei] = body[ei];
									ei++;
								}
								exec_line[ei] = '\0';
							}
							if (trace)
							{
								terminal_write("+ ");
								terminal_write_line(exec_line);
							}
							run_inline_command(exec_line);
						}
						if (*sep == '\0') break;
						cur = sep + 1;
					}
				}
				else if (string_equals(cmd, "fi"))
				{
					if (if_depth == 0)
					{
						terminal_write_line("run: fi without if");
					}
					else
					{
						if_depth--;
						if (if_depth == 0) current_exec = 1;
						else current_exec = if_stack[if_depth - 1].executing;
					}
				}
				else if (current_exec)
				{
					if (trace)
					{
						terminal_write("+ ");
						terminal_write_line(cmd);
					}
					run_inline_command(cmd);
				}
			}
		}
		line_len = 0;
	}

	if (if_depth != 0)
	{
		terminal_write_line("run: missing fi");
	}

	script_depth--;
	if (script_depth == 0) script_mode_active = 0;

	for (i = 0; i < (unsigned long)SCRIPT_VAR_MAX; i++)
	{
		unsigned long j = 0;
		while (j < sizeof(script_var_names[0]))
		{
			script_var_names[i][j] = old_script_var_names[i][j];
			j++;
		}
		j = 0;
		while (j < sizeof(script_var_values[0]))
		{
			script_var_values[i][j] = old_script_var_values[i][j];
			j++;
		}
	}
	script_var_count = old_script_var_count;
}

static void cmd_basic(const char *args)
{
	char path[128];
	char resolved[128];
	char program[EDITOR_BUFFER_SIZE];
	unsigned long size = 0;
	const char *p = read_token(args, path, sizeof(path));

	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: basic <path>");
		return;
	}

	if (fat_mode_active())
	{
		if (fat_resolve_path(path, resolved, sizeof(resolved)) != 0)
		{
			terminal_write_line("basic: invalid FAT path");
			return;
		}
		if (fat32_read_file_path(resolved, (unsigned char *)program, sizeof(program) - 1, &size) != 0)
		{
			terminal_write_line("basic: read failed");
			return;
		}
		program[size] = '\0';
	}
	else
	{
		const char *text;
		if (fs_read_text(path, &text) != 0)
		{
			terminal_write_line("basic: read failed");
			return;
		}
		while (text[size] != '\0' && size + 1 < sizeof(program))
		{
			program[size] = text[size];
			size++;
		}
		program[size] = '\0';
	}

	terminal_write_line("[basic] running...");
	if (basic_run(program) != 0)
	{
		terminal_write_line("[basic] program ended with errors");
		return;
	}
	terminal_write_line("[basic] done");
}

static void cmd_ataid(void)
{
	if (!ata_is_present())
	{
		terminal_write_line("ATA: no device");
		return;
	}

	terminal_write("ATA: present, sectors=");
	terminal_write_hex64((unsigned long)ata_get_sector_count_low());
	terminal_putc('\n');
}

static void cmd_readsec(const char *args)
{
	unsigned char buf[512];
	unsigned long lba;
	const char *p;
	unsigned long i;
	unsigned long j;

	p = skip_spaces(args);
	if (*p == '\0')
	{
		terminal_write_line("Usage: readsec <lba-hex>");
		return;
	}

	lba = parse_hex(p, (void *)0);
	if (ata_read_sector28((unsigned int)lba, buf) != 0)
	{
		terminal_write_line("readsec: ATA read failed");
		return;
	}

	for (i = 0; i < 512; i += 16)
	{
		terminal_write_hex64(i);
		terminal_write("  ");
		for (j = 0; j < 16; j++)
		{
			terminal_write_hex8(buf[i + j]);
			terminal_putc(' ');
		}
		terminal_write(" |");
		for (j = 0; j < 16; j++)
		{
			unsigned char b = buf[i + j];
			terminal_putc((b >= 0x20 && b < 0x7F) ? (char)b : '.');
		}
		terminal_write_line("|");
	}
}

static void cmd_writesec(const char *args)
{
	unsigned char buf[512];
	unsigned long lba;
	const char *p;
	unsigned long i;
	unsigned long n;

	p = skip_spaces(args);
	if (*p == '\0')
	{
		terminal_write_line("Usage: writesec <lba-hex> <text>");
		return;
	}

	lba = parse_hex(p, &p);
	p = skip_spaces(p);
	if (*p == '\0')
	{
		terminal_write_line("Usage: writesec <lba-hex> <text>");
		return;
	}

	for (i = 0; i < 512; i++) buf[i] = 0;
	buf[0] = 'T'; buf[1] = 'G'; buf[2] = '1'; buf[3] = '1';
	buf[4] = '-'; buf[5] = 'O'; buf[6] = 'S'; buf[7] = ':';

	n = 8;
	while (*p != '\0' && n < 511)
	{
		buf[n++] = (unsigned char)*p;
		p++;
	}

	if (ata_write_sector28((unsigned int)lba, buf) != 0)
	{
		terminal_write_line("writesec: ATA write failed");
		return;
	}

	terminal_write_line("writesec: done");
}

static void cmd_fatmount(void)
{
	struct block_device *dev = blockdev_get_primary();
	if (dev == (void *)0 || !dev->present)
	{
		terminal_write_line("fatmount: no block device");
		return;
	}

	if (fat32_mount(dev) != 0)
	{
		terminal_write_line("fatmount: mount failed");
		return;
	}

	vfs_prefer_fat_root = 1;
	fat_cwd[0] = '/';
	fat_cwd[1] = '\0';

	terminal_write_line("fatmount: mounted (generic fs commands now use FAT)");
}

static void cmd_fatunmount(void)
{
	if (!fat32_is_mounted())
	{
		terminal_write_line("fatunmount: not mounted");
		return;
	}

	fat32_unmount();
	vfs_prefer_fat_root = 0;
	fat_cwd[0] = '/';
	fat_cwd[1] = '\0';
	terminal_write_line("fatunmount: done");
}

static void cmd_color(const char *args)
{
	char what[16];
	char value[16];
	unsigned char c;
	const char *p = read_token(args, what, sizeof(what));

	if (p == (void *)0 || what[0] == '\0' || string_equals(what, "show"))
	{
		terminal_write("color: text=");
		terminal_write_hex8(terminal_text_color);
		terminal_write(" prompt=");
		terminal_write_hex8(terminal_prompt_color);
		terminal_putc('\n');
		return;
	}

	p = read_token(p, value, sizeof(value));
	if (p == (void *)0 || value[0] == '\0' || parse_color_token(value, &c) != 0)
	{
		terminal_write_line("Usage: color [show|text <0xNN>|prompt <0xNN>]");
		return;
	}

	if (string_equals(what, "text"))
	{
		terminal_text_color = c;
		screen_set_color(terminal_text_color);
		terminal_write_line("color: text updated");
		return;
	}
	if (string_equals(what, "prompt"))
	{
		terminal_prompt_color = c;
		terminal_write_line("color: prompt updated");
		return;
	}

	terminal_write_line("Usage: color [show|text <0xNN>|prompt <0xNN>]");
}

static void cmd_theme(const char *args)
{
	char name[24];
	const char *p = read_token(args, name, sizeof(name));
	if (p == (void *)0 || name[0] == '\0')
	{
		terminal_write_line("Usage: theme <name>");
		terminal_write_line("Hint: run 'themes' to list available themes");
		return;
	}
	if (apply_system_theme_by_name(name, 1) != 0)
	{
		terminal_write("system theme not found: ");
		terminal_write_line(name);
	}
}

static void cmd_themes(const char *args)
{
	char op[16];
	char names[FS_MAX_LIST][FS_NAME_MAX + 2];
	int types[FS_MAX_LIST];
	int count = 0;
	int i;
	const char *current = (void *)0;
	const char *p = read_token(args, op, sizeof(op));

	if (p != (void *)0 && op[0] != '\0' && !string_equals(op, "list"))
	{
		terminal_write_line("Usage: themes [list]");
		return;
	}

	if (fs_read_text(SYSTEM_THEME_CURRENT_PATH, &current) == 0)
	{
		terminal_write("system themes: current=");
		terminal_write_line(current);
	}
	else
	{
		terminal_write_line("system themes: current=(unknown)");
	}

	if (fs_ls(SYSTEM_THEME_DIR, names, types, FS_MAX_LIST, &count) != 0)
	{
		terminal_write_line("themes: failed to list /themes");
		return;
	}

	for (i = 0; i < count; i++)
	{
		if (types[i] == 1) continue;
		if (string_equals(names[i], "current")) continue;
		terminal_write("  ");
		terminal_write_line(names[i]);
	}
}

static void cmd_etheme(const char *args)
{
	char first[24];
	char name[24];
	char theme_path[96];
	const char *p = read_token(args, first, sizeof(first));
	if (p == (void *)0 || first[0] == '\0')
	{
		terminal_write_line("Usage: etheme <name>|edit <name>");
		terminal_write_line("Hint: run 'ethemes' to list available editor themes");
		return;
	}

	if (string_equals(first, "edit"))
	{
		unsigned long i = 0;
		unsigned long j = 0;
		int prev_vfs_fat = vfs_prefer_fat_root;
		p = read_token(p, name, sizeof(name));
		if (p == (void *)0 || name[0] == '\0')
		{
			terminal_write_line("Usage: etheme edit <name>");
			return;
		}

		while ("/edit/themes/"[i] != '\0' && i + 1 < sizeof(theme_path)) { theme_path[i] = "/edit/themes/"[i]; i++; }
		while (name[j] != '\0' && i + 1 < sizeof(theme_path)) theme_path[i++] = name[j++];
		if (i + 7 >= sizeof(theme_path))
		{
			terminal_write_line("etheme: name too long");
			return;
		}
		theme_path[i++] = '.';
		theme_path[i++] = 't';
		theme_path[i++] = 'h';
		theme_path[i++] = 'e';
		theme_path[i++] = 'm';
		theme_path[i++] = 'e';
		theme_path[i] = '\0';

		vfs_prefer_fat_root = 0;
		if (editor_open_file(theme_path, 0) != 0)
		{
			terminal_write_line("etheme edit: failed");
		}
		vfs_prefer_fat_root = prev_vfs_fat;
		return;
	}

	if (apply_editor_theme_by_name(first, 1) != 0)
	{
		terminal_write("editor theme not found: ");
		terminal_write_line(first);
	}
}

static void cmd_ethemes(const char *args)
{
	char op[16];
	char names[FS_MAX_LIST][FS_NAME_MAX + 2];
	int types[FS_MAX_LIST];
	int count = 0;
	int i;
	const char *current = (void *)0;
	const char *p = read_token(args, op, sizeof(op));

	if (p != (void *)0 && op[0] != '\0' && !string_equals(op, "list"))
	{
		terminal_write_line("Usage: ethemes [list]");
		return;
	}

	if (fs_read_text(EDITOR_THEME_CURRENT_PATH, &current) == 0)
	{
		terminal_write("editor themes: current=");
		terminal_write_line(current);
	}
	else
	{
		terminal_write_line("editor themes: current=(unknown)");
	}

	if (fs_ls(EDITOR_THEME_DIR, names, types, FS_MAX_LIST, &count) != 0)
	{
		terminal_write_line("ethemes: failed to list /edit/themes");
		return;
	}

	for (i = 0; i < count; i++)
	{
		if (types[i] == 1) continue;
		if (string_equals(names[i], "current")) continue;
		terminal_write("  ");
		terminal_write_line(names[i]);
	}
}

static void cmd_ramfs(void)
{
	vfs_prefer_fat_root = 0;
	terminal_write_line("ramfs: active (generic fs commands now use RAM FS)");
}

static void cmd_glyph(const char *args)
{
	char tok[16];
	const char *end;
	unsigned long v;
	char out[2];
	const char *p = read_token(args, tok, sizeof(tok));
	if (p == (void *)0 || tok[0] == '\0')
	{
		terminal_write_line("Usage: glyph <0xNN>");
		return;
	}
	v = parse_hex(tok, &end);
	if (*end != '\0' || v > 0xFFUL)
	{
		terminal_write_line("Usage: glyph <0xNN>");
		return;
	}
	out[0] = (char)v;
	out[1] = '\0';
	terminal_write(out);
	terminal_putc('\n');
}

static void cmd_charmap(void)
{
	terminal_write_line("CP437 aliases (use in commands/scripts):");
	terminal_write_line("  \\boxh \\boxv \\boxul \\boxur \\boxll \\boxlr");
	terminal_write_line("  \\boxt \\boxb \\boxl \\boxr \\boxx");
	terminal_write_line("  \\blk \\blkup \\blkdn \\shade1 \\shade2 \\shade3");
	terminal_write_line("  \\arru \\arrd \\arrl \\arrr \\tri");
	terminal_write_line("  \\xNN for raw byte (hex), example: echo \\xDB\\xDB\\xDB");
}

static void cmd_fatls(void)
{
	char names[64][40];
	char full_path[128];
	int count;
	int i;

	if (!fat32_is_mounted())
	{
		terminal_write_line("fatls: not mounted (run fatmount)");
		return;
	}

	if (fat_resolve_path(".", full_path, sizeof(full_path)) != 0 || fat32_ls_path(full_path, names, 64, &count) != 0)
	{
		terminal_write_line("fatls: failed");
		return;
	}

	for (i = 0; i < count; i++)
	{
		terminal_write_line(names[i]);
	}
}

static void cmd_fatcat(const char *args)
{
	char path[128];
	char full_path[128];
	unsigned char data[1024];
	unsigned long size;
	unsigned long i;
	const char *p = read_token(args, path, sizeof(path));

	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: fatcat <path>");
		return;
	}

	if (!fat32_is_mounted())
	{
		terminal_write_line("fatcat: not mounted (run fatmount)");
		return;
	}

	if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0 || fat32_read_file_path(full_path, data, sizeof(data) - 1, &size) != 0)
	{
		terminal_write_line("fatcat: read failed");
		return;
	}

	for (i = 0; i < size; i++)
	{
		unsigned char b = data[i];
		terminal_putc((b >= 0x20 && b < 0x7F) || b == '\n' || b == '\r' || b == '\t' ? (char)b : '.');
	}
	terminal_putc('\n');
}

static void cmd_fattouch(const char *args)
{
	char path[128];
	char full_path[128];
	const char *p = read_token(args, path, sizeof(path));
	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: fattouch <path>");
		return;
	}
	if (!fat32_is_mounted())
	{
		terminal_write_line("fattouch: not mounted (run fatmount)");
		return;
	}
	if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0 || fat32_touch_file_path(full_path) != 0)
	{
		terminal_write_line("fattouch: failed");
		return;
	}
	terminal_write_line("fattouch: done");
}

static void cmd_fatwrite(const char *args)
{
	char path[128];
	char full_path[128];
	const char *p = read_token(args, path, sizeof(path));
	unsigned long len = 0;
	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: fatwrite <path> <text>");
		return;
	}
	p = skip_spaces(p);
	if (*p == '\0')
	{
		terminal_write_line("Usage: fatwrite <path> <text>");
		return;
	}
	if (!fat32_is_mounted())
	{
		terminal_write_line("fatwrite: not mounted (run fatmount)");
		return;
	}
	while (p[len] != '\0') len++;
	if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0 || fat32_write_file_path(full_path, (const unsigned char *)p, len) != 0)
	{
		terminal_write_line("fatwrite: failed");
		return;
	}
	terminal_write_line("fatwrite: done");
}

static void cmd_fatrm(const char *args)
{
	char path[128];
	char full_path[128];
	const char *p = read_token(args, path, sizeof(path));
	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: fatrm <path>");
		return;
	}
	if (!fat32_is_mounted())
	{
		terminal_write_line("fatrm: not mounted (run fatmount)");
		return;
	}
	if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0 || fat32_remove_path(full_path) != 0)
	{
		terminal_write_line("fatrm: failed");
		return;
	}
	terminal_write_line("fatrm: done");
}

static void run_command(void)
{
	char expanded[INPUT_BUFFER_SIZE];
	char expanded_glyphs[INPUT_BUFFER_SIZE];
	unsigned long i;

	if (input_length == 0) { terminal_prompt(); return; }

	if (expand_command_substitutions(input_buffer, expanded, sizeof(expanded)) != 0)
	{
		terminal_write_line("substitution: bad $(...) expression");
		input_length = 0;
		input_buffer[0] = '\0';
		if (!editor_active && !script_mode_active) terminal_prompt();
		return;
	}
	if (expand_cp437_aliases(expanded, expanded_glyphs, sizeof(expanded_glyphs)) != 0)
	{
		terminal_write_line("glyph escape: bad alias or overflow");
		input_length = 0;
		input_buffer[0] = '\0';
		if (!editor_active && !script_mode_active) terminal_prompt();
		return;
	}

	i = 0;
	while (expanded_glyphs[i] != '\0' && i + 1 < sizeof(input_buffer))
	{
		input_buffer[i] = expanded_glyphs[i];
		i++;
	}
	input_buffer[i] = '\0';
	input_length = i;
	cursor_pos = i;

	if (input_length == 0)
	{
		if (!editor_active && !script_mode_active) terminal_prompt();
		return;
	}

	if (string_starts_with(input_buffer, "help"))
	{
		if (input_buffer[4] == '\0') cmd_help("");
		else if (input_buffer[4] == ' ') cmd_help(input_buffer + 5);
		else terminal_write_line("Usage: help [basic|fs|disk]");
	}
	else if (string_equals(input_buffer, "version"))
	{
		terminal_write_line(TG11_OS_VERSION);
	}
	else if (string_equals(input_buffer, "pwd"))
	{
		cmd_pwd();
	}
	else if (string_starts_with(input_buffer, "ls"))
	{
		if (input_buffer[2] == '\0' || input_buffer[2] == ' ') cmd_ls(input_buffer + 2);
		else terminal_write_line("Usage: ls [path]");
	}
	else if (string_starts_with(input_buffer, "cd"))
	{
		if (input_buffer[2] == ' ') cmd_cd(input_buffer + 3);
		else terminal_write_line("Usage: cd <path>");
	}
	else if (string_starts_with(input_buffer, "mkdir"))
	{
		if (input_buffer[5] == ' ') cmd_mkdir(input_buffer + 6);
		else terminal_write_line("Usage: mkdir <path>");
	}
	else if (string_starts_with(input_buffer, "touch"))
	{
		if (input_buffer[5] == ' ') cmd_touch(input_buffer + 6);
		else terminal_write_line("Usage: touch <path>");
	}
	else if (string_starts_with(input_buffer, "write"))
	{
		if (input_buffer[5] == ' ') cmd_write(input_buffer + 6);
		else terminal_write_line("Usage: write <path> <text>");
	}
	else if (string_starts_with(input_buffer, "cat"))
	{
		if (input_buffer[3] == ' ') cmd_cat(input_buffer + 4);
		else terminal_write_line("Usage: cat <path>");
	}
	else if (string_starts_with(input_buffer, "rm"))
	{
		if (input_buffer[2] == ' ') cmd_rm(input_buffer + 3);
		else terminal_write_line("Usage: rm <path>");
	}
	else if (string_starts_with(input_buffer, "cp"))
	{
		if (input_buffer[2] == ' ') cmd_cp(input_buffer + 3);
		else terminal_write_line("Usage: cp <src> <dst>");
	}
	else if (string_starts_with(input_buffer, "mv"))
	{
		if (input_buffer[2] == ' ') cmd_mv(input_buffer + 3);
		else terminal_write_line("Usage: mv <src> <dst>");
	}
	else if (string_starts_with(input_buffer, "edit"))
	{
		if (input_buffer[4] == ' ') cmd_edit(input_buffer + 5);
		else terminal_write_line("Usage: edit <path>");
	}
	else if (string_starts_with(input_buffer, "hexedit"))
	{
		if (input_buffer[7] == ' ') cmd_hexedit(input_buffer + 8);
		else terminal_write_line("Usage: hexedit <path>");
	}
	else if (string_starts_with(input_buffer, "run"))
	{
		if (input_buffer[3] == ' ') cmd_run(input_buffer + 4);
		else terminal_write_line("Usage: run [-x] <path>");
	}
	else if (string_starts_with(input_buffer, "basic"))
	{
		if (input_buffer[5] == ' ') cmd_basic(input_buffer + 6);
		else terminal_write_line("Usage: basic <path>");
	}
	else if (string_equals(input_buffer, "clear"))
	{
		input_length = 0; input_buffer[0] = '\0';
		screen_clear();
		if (!script_mode_active) terminal_prompt();
		return;
	}
	else if (string_equals(input_buffer, "memmap"))
	{
		memmap_print();
	}
	else if (string_starts_with(input_buffer, "hexdump"))
	{
		cmd_hexdump(input_buffer + 7);
	}
	else if (string_equals(input_buffer, "ataid"))
	{
		cmd_ataid();
	}
	else if (string_starts_with(input_buffer, "readsec"))
	{
		if (input_buffer[7] == ' ') cmd_readsec(input_buffer + 8);
		else terminal_write_line("Usage: readsec <lba-hex>");
	}
	else if (string_starts_with(input_buffer, "writesec"))
	{
		if (input_buffer[8] == ' ') cmd_writesec(input_buffer + 9);
		else terminal_write_line("Usage: writesec <lba-hex> <text>");
	}
	else if (string_equals(input_buffer, "fatmount"))
	{
		cmd_fatmount();
	}
	else if (string_equals(input_buffer, "fatunmount"))
	{
		cmd_fatunmount();
	}
	else if (string_equals(input_buffer, "fatls"))
	{
		cmd_fatls();
	}
	else if (string_starts_with(input_buffer, "fatcat"))
	{
		if (input_buffer[6] == ' ') cmd_fatcat(input_buffer + 7);
		else terminal_write_line("Usage: fatcat <path>");
	}
	else if (string_starts_with(input_buffer, "fattouch"))
	{
		if (input_buffer[8] == ' ') cmd_fattouch(input_buffer + 9);
		else terminal_write_line("Usage: fattouch <path>");
	}
	else if (string_starts_with(input_buffer, "fatwrite"))
	{
		if (input_buffer[8] == ' ') cmd_fatwrite(input_buffer + 9);
		else terminal_write_line("Usage: fatwrite <path> <text>");
	}
	else if (string_starts_with(input_buffer, "fatrm"))
	{
		if (input_buffer[5] == ' ') cmd_fatrm(input_buffer + 6);
		else terminal_write_line("Usage: fatrm <path>");
	}
	else if (string_starts_with(input_buffer, "echo"))
	{
		if      (input_buffer[4] == '\0') terminal_putc('\n');
		else if (input_buffer[4] == ' ')  terminal_write_line(&input_buffer[5]);
		else    terminal_write_line("Unknown command. Type help for a list.");
	}
	else if (string_starts_with(input_buffer, "glyph"))
	{
		if (input_buffer[5] == ' ') cmd_glyph(input_buffer + 6);
		else terminal_write_line("Usage: glyph <0xNN>");
	}
	else if (string_equals(input_buffer, "charmap"))
	{
		cmd_charmap();
	}
	else if (string_starts_with(input_buffer, "color"))
	{
		if (input_buffer[5] == '\0') cmd_color("show");
		else if (input_buffer[5] == ' ') cmd_color(input_buffer + 6);
		else terminal_write_line("Usage: color [show|text <0xNN>|prompt <0xNN>]");
	}
	else if (string_starts_with(input_buffer, "themes"))
	{
		if (input_buffer[6] == '\0') cmd_themes("list");
		else if (input_buffer[6] == ' ') cmd_themes(input_buffer + 7);
		else terminal_write_line("Usage: themes [list]");
	}
	else if (string_starts_with(input_buffer, "theme"))
	{
		if (input_buffer[5] == ' ') cmd_theme(input_buffer + 6);
		else terminal_write_line("Usage: theme <name>");
	}
	else if (string_starts_with(input_buffer, "ethemes"))
	{
		if (input_buffer[7] == '\0') cmd_ethemes("list");
		else if (input_buffer[7] == ' ') cmd_ethemes(input_buffer + 8);
		else terminal_write_line("Usage: ethemes [list]");
	}
	else if (string_starts_with(input_buffer, "etheme"))
	{
		if (input_buffer[6] == ' ') cmd_etheme(input_buffer + 7);
		else terminal_write_line("Usage: etheme <name>|edit <name>");
	}
	else if (string_equals(input_buffer, "ramfs"))
	{
		cmd_ramfs();
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
	if (!editor_active && !script_mode_active)
	{
		terminal_prompt();
	}
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

static void editor_handle_scancode(unsigned char scancode)
{
	char c;
	unsigned long i;

	if (scancode == 0xE0) { extended_key = 1; return; }
	if (extended_key)
	{
		unsigned long col;
		unsigned long line_start;
		unsigned long line_end;
		unsigned long rows;
		extended_key = 0;
		if (scancode == 0x1D) { ctrl_held = 1; return; }
		if (scancode == 0x9D) { ctrl_held = 0; return; }
		if (editor_hex_mode)
		{
			unsigned long row_start = (editor_cursor / EDITOR_HEX_BYTES_PER_ROW) * EDITOR_HEX_BYTES_PER_ROW;
			if (scancode == 0x53)
			{
				if (editor_cursor < editor_length)
				{
					for (i = editor_cursor; i + 1 < editor_length; i++) editor_buffer[i] = editor_buffer[i + 1];
					editor_length--;
					editor_dirty = 1;
					editor_hex_nibble = 0;
					editor_render();
				}
				return;
			}
			if (scancode == 0x4B)
			{
				if (editor_cursor > 0) editor_cursor--;
				editor_hex_nibble = 0;
				editor_render();
				return;
			}
			if (scancode == 0x4D)
			{
				if (editor_cursor < editor_length) editor_cursor++;
				editor_hex_nibble = 0;
				editor_render();
				return;
			}
			if (scancode == 0x47)
			{
				editor_cursor = ctrl_held ? 0 : row_start;
				editor_hex_nibble = 0;
				editor_render();
				return;
			}
			if (scancode == 0x4F)
			{
				unsigned long row_end = row_start + (EDITOR_HEX_BYTES_PER_ROW - 1);
				if (row_end > editor_length) row_end = editor_length;
				editor_cursor = ctrl_held ? editor_length : row_end;
				editor_hex_nibble = 0;
				editor_render();
				return;
			}
			if (scancode == 0x49)
			{
				rows = editor_visible_rows();
				if (rows > 1) rows--;
				if (editor_cursor >= rows * EDITOR_HEX_BYTES_PER_ROW) editor_cursor -= rows * EDITOR_HEX_BYTES_PER_ROW;
				else editor_cursor = 0;
				editor_hex_nibble = 0;
				editor_render();
				return;
			}
			if (scancode == 0x51)
			{
				rows = editor_visible_rows();
				if (rows > 1) rows--;
				editor_cursor += rows * EDITOR_HEX_BYTES_PER_ROW;
				if (editor_cursor > editor_length) editor_cursor = editor_length;
				editor_hex_nibble = 0;
				editor_render();
				return;
			}
			if (scancode == 0x48)
			{
				if (editor_cursor >= EDITOR_HEX_BYTES_PER_ROW) editor_cursor -= EDITOR_HEX_BYTES_PER_ROW;
				editor_hex_nibble = 0;
				editor_render();
				return;
			}
			if (scancode == 0x50)
			{
				editor_cursor += EDITOR_HEX_BYTES_PER_ROW;
				if (editor_cursor > editor_length) editor_cursor = editor_length;
				editor_hex_nibble = 0;
				editor_render();
				return;
			}
			return;
		}
		if (scancode == 0x4B)
		{
			if (editor_cursor > 0) editor_cursor--;
			editor_render();
			return;
		}
		if (scancode == 0x4D)
		{
			if (editor_cursor < editor_length) editor_cursor++;
			editor_render();
			return;
		}
		if (scancode == 0x47)
		{
			if (ctrl_held) editor_cursor = 0;
			else editor_cursor = editor_line_start(editor_cursor);
			editor_render();
			return;
		}
		if (scancode == 0x4F)
		{
			if (ctrl_held) editor_cursor = editor_length;
			else editor_cursor = editor_line_end(editor_cursor);
			editor_render();
			return;
		}
		if (scancode == 0x49)
		{
			rows = editor_visible_rows();
			if (rows > 1) rows--;
			editor_cursor = editor_move_visual_rows(editor_cursor, rows, 0);
			editor_render();
			return;
		}
		if (scancode == 0x51)
		{
			rows = editor_visible_rows();
			if (rows > 1) rows--;
			editor_cursor = editor_move_visual_rows(editor_cursor, rows, 1);
			editor_render();
			return;
		}
		if (scancode == 0x48)
		{
			line_start = editor_line_start(editor_cursor);
			if (line_start == 0)
			{
				editor_render();
				return;
			}
			col = editor_cursor - line_start;
			{
				unsigned long prev_end = line_start - 1;
				unsigned long prev_start = editor_line_start(prev_end);
				unsigned long prev_len = prev_end - prev_start;
				editor_cursor = prev_start + (col < prev_len ? col : prev_len);
			}
			editor_render();
			return;
		}
		if (scancode == 0x50)
		{
			line_start = editor_line_start(editor_cursor);
			line_end = editor_line_end(editor_cursor);
			if (line_end >= editor_length)
			{
				editor_render();
				return;
			}
			col = editor_cursor - line_start;
			{
				unsigned long next_start = line_end + 1;
				unsigned long next_end = editor_line_end(next_start);
				unsigned long next_len = next_end - next_start;
				editor_cursor = next_start + (col < next_len ? col : next_len);
			}
			editor_render();
			return;
		}
		return;
	}

	if (scancode == 0x2A || scancode == 0x36) { shift_held = 1; return; }
	if (scancode == 0xAA || scancode == 0xB6) { shift_held = 0; return; }
	if (scancode == 0x1D) { ctrl_held = 1; return; }
	if (scancode == 0x9D) { ctrl_held = 0; return; }
	if (scancode == 0x3A) { caps_lock_on = !caps_lock_on; return; }
	if (scancode & 0x80) return;

	if (scancode == 0x44)
	{
		screen_set_pos(editor_prev_end);
		screen_set_hw_cursor(editor_prev_end);
		terminal_putc('\n');
		if (editor_save() == 0) terminal_write_line("[editor] saved");
		else terminal_write_line("[editor] save failed");
		editor_active = 0;
		input_length = 0;
		cursor_pos = 0;
		input_buffer[0] = '\0';
		terminal_prompt();
		return;
	}

	if (scancode == 0x1F && ctrl_held)
	{
		if (editor_save() == 0) editor_status_line("[editor] saved");
		else editor_status_line("[editor] save failed");
		return;
	}

	if (scancode == 0x01)
	{
		screen_set_pos(editor_prev_end);
		screen_set_hw_cursor(editor_prev_end);
		terminal_putc('\n');
		terminal_write_line("[editor] canceled");
		editor_active = 0;
		input_length = 0;
		cursor_pos = 0;
		input_buffer[0] = '\0';
		terminal_prompt();
		return;
	}

	if (editor_hex_mode)
	{
		int hv;
		c = translate_scancode(scancode);
		hv = editor_hex_value(c);
		if (scancode == 0x0E)
		{
			if (editor_cursor > 0)
			{
				editor_cursor--;
				for (i = editor_cursor; i + 1 < editor_length; i++) editor_buffer[i] = editor_buffer[i + 1];
				editor_length--;
				editor_dirty = 1;
				editor_hex_nibble = 0;
				editor_render();
			}
			return;
		}
		if (hv < 0) return;
		if (editor_cursor >= EDITOR_BUFFER_SIZE)
		{
			editor_status_line("[editor] buffer full");
			return;
		}
		if (editor_cursor == editor_length)
		{
			editor_buffer[editor_length] = 0;
			editor_length++;
		}
		{
			unsigned char byte = (unsigned char)editor_buffer[editor_cursor];
			if (editor_hex_nibble == 0)
			{
				byte = (unsigned char)((byte & 0x0F) | ((unsigned char)hv << 4));
				editor_buffer[editor_cursor] = (char)byte;
				editor_hex_nibble = 1;
			}
			else
			{
				byte = (unsigned char)((byte & 0xF0) | (unsigned char)hv);
				editor_buffer[editor_cursor] = (char)byte;
				editor_hex_nibble = 0;
				if (editor_cursor < editor_length) editor_cursor++;
			}
		}
		editor_dirty = 1;
		editor_render();
		return;
	}

	if (scancode == 0x0E)
	{
		if (editor_cursor > 0)
		{
			for (i = editor_cursor - 1; i < editor_length - 1; i++) editor_buffer[i] = editor_buffer[i + 1];
			editor_cursor--;
			editor_length--;
			editor_buffer[editor_length] = '\0';
			editor_dirty = 1;
			editor_render();
		}
		return;
	}

	if (scancode == 0x1C)
	{
		if (editor_length + 1 < EDITOR_BUFFER_SIZE)
		{
			for (i = editor_length; i > editor_cursor; i--) editor_buffer[i] = editor_buffer[i - 1];
			editor_buffer[editor_cursor++] = '\n';
			editor_length++;
			editor_buffer[editor_length] = '\0';
			editor_dirty = 1;
			editor_render();
		}
		return;
	}

	if (scancode == 0x0F) c = '\t';
	else c = translate_scancode(scancode);
	if (c == '\0') return;
	if (editor_length + 1 >= EDITOR_BUFFER_SIZE)
	{
		editor_status_line("[editor] buffer full");
		return;
	}
	for (i = editor_length; i > editor_cursor; i--) editor_buffer[i] = editor_buffer[i - 1];
	editor_buffer[editor_cursor++] = c;
	editor_length++;
	editor_buffer[editor_length] = '\0';
	editor_dirty = 1;
	editor_render();
}

/* ================================================================== */
/* Scancode dispatcher                                                */
/* ================================================================== */

static void handle_scancode(unsigned char scancode)
{
	char c;
	unsigned long i;

	if (editor_active)
	{
		editor_handle_scancode(scancode);
		return;
	}

	if (scancode == 0xE0) { extended_key = 1; return; }

	if (extended_key)
	{
		extended_key = 0;
		if (scancode == 0x1D) { ctrl_held = 1; return; }
		if (scancode == 0x9D) { ctrl_held = 0; return; }
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
	if (scancode == 0x1D) { ctrl_held = 1; return; }
	if (scancode == 0x9D) { ctrl_held = 0; return; }
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
/* Public API                                                         */
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
	ensure_theme_files();
	load_current_system_theme();
	load_current_editor_theme();
	screen_clear();
	screen_set_color(terminal_text_color);
	serial_ready = serial_init();
	memmap_init(mb2_info_addr);
	terminal_write_line("TG11 OS (64-bit)");
	terminal_write_line(TG11_OS_VERSION);
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

