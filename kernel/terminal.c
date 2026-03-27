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
#include "framebuffer.h"
#include "memory.h"
#include "elf.h"
#include "idt.h"
#include "task.h"
#include "timer.h"

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
#define EDITOR_THEME_KEYWORD_MAX 40
#define EDITOR_THEME_KEYWORD_LEN 16
#define EDITOR_THEME_CUSTOM_GROUP_MAX 4
#define COMMAND_ALIAS_MAX 16
#define COMMAND_ALIAS_NAME_LEN 16
#define COMMAND_ALIAS_EXPANSION_LEN 96
#define SYSTEM_THEME_DIR "/themes"
#define SYSTEM_THEME_CURRENT_PATH "/themes/current"
#define EDITOR_THEME_DIR "/edit/themes"
#define EDITOR_THEME_CURRENT_PATH "/edit/themes/current"
#define FBFONT_DIR "/fonts"
#define ETC_DIR "/etc"
#define MOTD_PATH "/etc/motd.txt"
#define AUTORUN_PATH "/etc/autorun.sh"
#define AUTORUN_MODE_PATH "/etc/autorun.mode"
#define AUTORUN_ONCE_STATE_PATH "/etc/autorun.once_state"
#define AUTORUN_DELAY_PATH "/etc/autorun.delay"
#define FAT_AUTORUN_PATH "/autorun.sh"
#define FAT_AUTORUN_MODE_PATH "/autorun.mod"
#define FAT_AUTORUN_ONCE_STATE_PATH "/autorun.onc"
#define FAT_AUTORUN_DELAY_PATH "/autorun.dly"
#define AUTORUN_DEFAULT_DELAY_SECONDS 8UL
#define MB2_TAG_END 0
#define MB2_TAG_CMDLINE 1
#define EDITOR_SCREEN_SNAPSHOT_MAX_CELLS (256UL * 144UL)

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
static int terminal_selection_active = 0;
static unsigned long terminal_selection_anchor = 0;
static unsigned long terminal_last_drawn_length = 0;

/* ------------------------------------------------------------------ */
/* Keyboard modifier state                                            */
/* ------------------------------------------------------------------ */
static int shift_held   = 0;
static int caps_lock_on = 0;
static int ctrl_held    = 0;
static int alt_held     = 0;
static int extended_key = 0;
static int control_poll_extended = 0;
static volatile int terminal_cancel_requested = 0;
static int panic_esc_held = 0;
static int panic_f12_held = 0;
static int panic_hotkey_fired = 0;

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
static int serial_mirror_enabled = 1;
static int serial_compact_enabled = 0;
static int serial_rxecho_enabled = 1;
static int terminal_capture_mode = 0;
static int terminal_capture_done = 0;
static char *terminal_capture_out = (void *)0;
static unsigned long terminal_capture_out_size = 0;
static int display_mode = 0; /* 0=vga25, 1=vga50, 2=framebuffer */
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
static unsigned char editor_custom_colors[EDITOR_THEME_CUSTOM_GROUP_MAX] = {0x0E, 0x0D, 0x0B, 0x0C};
static char editor_sh_keywords[EDITOR_THEME_KEYWORD_MAX][EDITOR_THEME_KEYWORD_LEN] = {
	"if", "then", "else", "fi", "for", "do", "done", "while", "in", "case", "esac", "function", "echo", "exit", "cd", "ls"
};
static unsigned long editor_sh_keyword_count = 16;
static char editor_basic_keywords[EDITOR_THEME_KEYWORD_MAX][EDITOR_THEME_KEYWORD_LEN] = {
	"PRINT", "LET", "DIM", "IF", "THEN", "GOTO", "GOSUB", "RETURN", "ON", "FOR", "TO", "STEP", "NEXT", "INPUT", "DATA", "READ", "RESTORE", "TAB", "SPC", "ABS", "RND", "LEN", "VAL", "ASC", "CHR$", "STR$", "REM", "END", "STOP", "LIST", "RUN", "CLS", "CLEAR"
};
static unsigned long editor_basic_keyword_count = 33;
static char editor_custom_tokens[EDITOR_THEME_CUSTOM_GROUP_MAX][EDITOR_THEME_KEYWORD_MAX][EDITOR_THEME_KEYWORD_LEN];
static unsigned long editor_custom_token_counts[EDITOR_THEME_CUSTOM_GROUP_MAX] = {0, 0, 0, 0};
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
static int editor_selection_active = 0;
static unsigned long editor_selection_anchor = 0;
static char editor_clipboard[EDITOR_BUFFER_SIZE];
static unsigned long editor_clipboard_length = 0;
static unsigned long editor_view_top = 0;
static unsigned short editor_vga_start = 0;
static unsigned short editor_prev_end = 0;
static int editor_dirty = 0;
static int editor_find_active = 0;
static char editor_find_query[64];
static unsigned long editor_find_query_length = 0;
static char editor_find_last_query[64];
static unsigned long editor_find_last_query_length = 0;
static int editor_find_match_valid = 0;
static unsigned long editor_find_match_start = 0;
static unsigned long editor_find_match_end = 0;
static int editor_screen_saved = 0;
static unsigned long editor_screen_saved_width = 0;
static unsigned long editor_screen_saved_height = 0;
static unsigned long editor_screen_saved_cells = 0;
static unsigned short editor_screen_saved_pos = 0;
static unsigned char editor_screen_saved_color = 0x0F;
static unsigned char editor_screen_saved_style = 0;
static char editor_screen_saved_chars[EDITOR_SCREEN_SNAPSHOT_MAX_CELLS];
static unsigned char editor_screen_saved_attrs[EDITOR_SCREEN_SNAPSHOT_MAX_CELLS];
static unsigned char editor_screen_saved_styles[EDITOR_SCREEN_SNAPSHOT_MAX_CELLS];
static int script_mode_active = 0;
static int script_depth = 0;
#define SCRIPT_VAR_MAX 8
static int script_var_count = 0;
static char script_var_names[SCRIPT_VAR_MAX][16];
static char script_var_values[SCRIPT_VAR_MAX][96];
static int command_alias_count = 0;
static char command_alias_names[COMMAND_ALIAS_MAX][COMMAND_ALIAS_NAME_LEN];
static char command_alias_expansions[COMMAND_ALIAS_MAX][COMMAND_ALIAS_EXPANSION_LEN];
static int autorun_boot_pending = 0;
static unsigned long autorun_boot_deadline = 0;

static int string_equals(const char *a, const char *b);
static int string_equals_ci(const char *a, const char *b);
static const char *skip_spaces(const char *s);
static const char *read_token(const char *s, char *out, unsigned long out_size);
static unsigned long string_length(const char *s);
static void terminal_putc(char c);
static int parse_dec_u32(const char *s, unsigned int *out);
static void terminal_clear_selection(void);
static void terminal_redraw_input_line(void);
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
static unsigned long editor_move_word_left(unsigned long index);
static unsigned long editor_move_word_right(unsigned long index);
static unsigned long editor_line_number_for_index(unsigned long index);
static int editor_has_more_below(unsigned long top, unsigned long rows);
static void editor_draw_header(int can_scroll_up, int can_scroll_down);
static void editor_status_line(const char *msg);
static void editor_capture_screen(void);
static void editor_restore_screen(void);
static void editor_close(int saved, int save_attempted);
static void editor_find_open(void);
static void editor_find_cancel(void);
static int editor_find_next(int advance_from_current);
static int editor_find_prev(void);
static void editor_find_invalidate_match(void);
static int parse_display_mode_spec(const char *spec, unsigned int *width, unsigned int *height, unsigned int *bpp);
static int editor_open_file(const char *path, int hex_mode);
static int editor_has_selection(void);
static unsigned long editor_selection_start(void);
static unsigned long editor_selection_end(void);
static void editor_clear_selection(void);
static void editor_begin_selection_if_needed(void);
static void editor_finish_selection_move(void);
static void editor_delete_range(unsigned long start, unsigned long end);
static void editor_delete_selection(void);
static void terminal_shutdown(void);
static void do_reboot(void);
static void editor_copy_selection(int cut);
static int editor_insert_text(const char *text, unsigned long len);
static int parse_color_token(const char *s, unsigned char *out);
static int token_is_keyword_ci_table(const char *s, unsigned long len, char keywords[][EDITOR_THEME_KEYWORD_LEN], unsigned long keyword_count);
static int editor_path_has_ext_ci(const char *path, const char *ext);
static enum editor_lang editor_detect_language_from_path(const char *path);
static void copy_keyword_table(char dst[][EDITOR_THEME_KEYWORD_LEN], unsigned long *dst_count, char src[][EDITOR_THEME_KEYWORD_LEN], unsigned long src_count);
static int parse_keyword_list(const char *value, char out[][EDITOR_THEME_KEYWORD_LEN], unsigned long max_keywords, unsigned long *out_count);
static int parse_custom_theme_key(const char *key, const char *suffix, unsigned long *group_index_out);
static int token_is_word_only(const char *s);
static int editor_match_custom_token_at(const char *buffer, unsigned long length, unsigned long index, unsigned char *color_out, unsigned long *len_out);
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
static void cmd_ramfs2fat(const char *args);
static void uint_to_dec(unsigned long v, char *buf, unsigned long buf_sz);
static void cmd_drives(void);
static void cmd_fatmount(const char *args);
static void cmd_fatattr(const char *args);
static void cmd_serial(const char *args);
static void cmd_display(const char *args);
static void cmd_fbfont(const char *args);
static void cmd_memstat(void);
static void cmd_pagetest(void);
static void cmd_pagefault(const char *args);
static void cmd_gpfault(const char *args);
static void cmd_udfault(const char *args);
static void cmd_doublefault(const char *args);
static void cmd_exceptstat(const char *args);
static void cmd_dumpstack(const char *args);
static void cmd_selftest(const char *args);
static void cmd_exec(const char *args);
static void cmd_tasks(void);
static void cmd_tasktest(void);
static void cmd_taskspin(void);
static void cmd_shellspawn(void);
static void cmd_taskprotect(const char *args);
static void cmd_shellwatch(const char *args);
static void cmd_tasklog(const char *args);
static void cmd_motd(void);
static void cmd_autorun(const char *args);
static void cmd_taskkill(const char *args);
static void cmd_taskstop(const char *args);
static void cmd_taskcont(const char *args);
static void cmd_ticks(void);
static void cmd_elfinfo(const char *args);
static void cmd_elfsym(const char *args);
static void cmd_elfaddr(const char *args);
static void cmd_execstress(const char *args);
static void cmd_elfselftest(const char *args);
static void trigger_forced_panic(void);
static void update_panic_hotkey(void);
static void cmd_man(const char *args);
static void cmd_alias(const char *args);
static void cmd_unalias(const char *args);
static void cmd_run(const char *args);
static void cmd_dir(const char *args);
static void cmd_tree(const char *args);
static void print_help_commands(const char *args);
static int print_manual_entry(const char *topic, const char *args);
static int resolve_command_aliases(const char *in, char *out, unsigned long out_size);
static void terminal_auto_fatmount(void);
static int terminal_try_mount_boot_fat(void);
static int terminal_write_fat_text(const char *path, const char *text);
static int terminal_get_autorun_mode(void);
static void terminal_set_autorun_mode(int mode);
static int terminal_autorun_once_done(void);
static void terminal_set_autorun_once_done(int done);
static unsigned long terminal_get_autorun_delay_seconds(void);
static void terminal_set_autorun_delay_seconds(unsigned long seconds);
static int terminal_run_autorun_script_now(void);
static int terminal_autorun_should_run_on_boot(void);
static void terminal_schedule_boot_autorun(void);
static void terminal_poll_boot_autorun(void);
static void terminal_print_motd(void);
static void terminal_run_boot_autorun(void);

#pragma pack(push, 1)
struct mb2_tag_header
{
	unsigned int type;
	unsigned int size;
};

struct mb2_tag_cmdline
{
	unsigned int type;
	unsigned int size;
	char string[1];
};
#pragma pack(pop)

enum boot_display_pref
{
	BOOT_DISPLAY_AUTO = 0,
	BOOT_DISPLAY_VGA,
	BOOT_DISPLAY_FB
};

static enum boot_display_pref boot_display_preference(unsigned long mb2_info_addr)
{
	unsigned long offset;
	struct mb2_tag_header *tag;

	if (mb2_info_addr == 0) return BOOT_DISPLAY_AUTO;

	offset = mb2_info_addr + 8;
	for (;;)
	{
		char *s;
		tag = (struct mb2_tag_header *)offset;
		if (tag->type == MB2_TAG_END) break;
		if (tag->type == MB2_TAG_CMDLINE)
		{
			struct mb2_tag_cmdline *cmd = (struct mb2_tag_cmdline *)tag;
			s = cmd->string;
			while (*s != '\0')
			{
				char value[16];
				unsigned long i = 0;
				while (*s == ' ' || *s == '\t') s++;
				if (*s == '\0') break;
				if (s[0] == 'd' || s[0] == 'D')
				{
					if ((s[1] == 'i' || s[1] == 'I') &&
						(s[2] == 's' || s[2] == 'S') &&
						(s[3] == 'p' || s[3] == 'P') &&
						(s[4] == 'l' || s[4] == 'L') &&
						(s[5] == 'a' || s[5] == 'A') &&
						(s[6] == 'y' || s[6] == 'Y') &&
						s[7] == '=')
					{
						s += 8;
						while (*s != '\0' && *s != ' ' && *s != '\t' && i + 1 < sizeof(value)) value[i++] = *s++;
						value[i] = '\0';
						if (string_equals_ci(value, "fb") || string_equals_ci(value, "framebuffer")) return BOOT_DISPLAY_FB;
						if (string_equals_ci(value, "vga") || string_equals_ci(value, "text") || string_equals_ci(value, "vga25") || string_equals_ci(value, "vga50")) return BOOT_DISPLAY_VGA;
					}
				}
				while (*s != '\0' && *s != ' ' && *s != '\t') s++;
			}
		}
		offset += (tag->size + 7) & ~7u;
	}

	return BOOT_DISPLAY_AUTO;
}

static void print_help_basic(void)
{
	terminal_write_line("Basic commands:");
	terminal_write_line("  help [topic]         - Show help page or command help");
	terminal_write_line("  man <topic> [page]   - Show command manual entry");
	terminal_write_line("  man -k <word>        - Search manual entries by keyword");
	terminal_write_line("  alias <n> <cmd...>   - Create command alias");
	terminal_write_line("  unalias <name>       - Remove command alias");
	terminal_write_line("  version              - Show OS version");
	terminal_write_line("  echo <text>          - Print text (\\n \\t \\e, §NN colors, §l/§i/§u/§s styles, §r reset)");
	terminal_write_line("  glyph <0xNN>         - Print one CP437 glyph byte");
	terminal_write_line("  charmap              - Show CP437 alias escapes");
	terminal_write_line("  color show           - Show current colors");
	terminal_write_line("  color preview [text|prompt] - Preview palette or current attrs");
	terminal_write_line("  color text <0xNN>    - Set text color attr byte");
	terminal_write_line("  color prompt <0xNN>  - Set prompt color attr byte");
	terminal_write_line("  serial [on|off|show|compact <on|off>|rxecho <on|off>] - Serial options");
	terminal_write_line("  display [show|vga25|vga50|fb|mode <show|list|1080p|900p|768p|720p|WIDTHxHEIGHT[xBPP]>|cursor <show|underline|block|bar>] - Switch display mode");
	terminal_write_line("  fbfont [show|list|style <classic|blocky>|size <small|normal|large>|reset|glyph <ch> <r0..r6>|save <name>|load <name>] - FB font");
	terminal_write_line("  theme <name>         - Apply system theme from /themes");
	terminal_write_line("  themes [list]        - List system themes");
	terminal_write_line("  etheme <name>        - Apply editor theme from /edit/themes");
	terminal_write_line("  etheme edit <name>   - Edit /edit/themes/<name>.theme");
	terminal_write_line("  ethemes [list]       - List editor themes");
	terminal_write_line("  clear                - Clear screen");
	terminal_write_line("  reboot               - Reboot the system");
	terminal_write_line("  panic                - Deliberately trigger kernel panic");
	terminal_write_line("  shutdown/exit/quit   - Shut down");
	terminal_write_line("  memmap               - Physical memory map");
	terminal_write_line("  memstat              - Allocator and paging summary");
	terminal_write_line("  pagetest             - Paging allocator self-test");
	terminal_write_line("  pagefault <mode>     - Trigger PF: read|write|exec");
	terminal_write_line("  gpfault              - Trigger #GP using non-canonical address");
	terminal_write_line("  udfault              - Trigger #UD via UD2 instruction");
	terminal_write_line("  doublefault          - Simulate double-fault recovery");
	terminal_write_line("  exceptstat           - Show exception statistics");
	terminal_write_line("  dumpstack            - Dump current kernel call stack");
	terminal_write_line("  selftest exceptions [step] - Guided exception test harness");
	terminal_write_line("  elfinfo <path>       - Inspect ELF headers and symbol count");
	terminal_write_line("  elfsym <path> [f]    - List ELF symbols, optional name filter");
	terminal_write_line("  elfaddr <p> <addr>   - Resolve an address to the nearest ELF symbol");
	terminal_write_line("  exec <path>          - Load/call one ELF64 binary");
	terminal_write_line("  execstress <n> <path> - Repeat ELF run and show free-page delta");
	terminal_write_line("  elfselftest          - Run built-in ELF test matrix");
	terminal_write_line("  hexdump <a> [n]      - Hex dump memory");
}

static void print_help_fs(void)
{
	terminal_write_line("Filesystem commands:");
	terminal_write_line("  pwd                  - Print current directory");
	terminal_write_line("  ls [path]            - List entries");
	terminal_write_line("  dir [path]           - Windows-style directory listing with sizes");
	terminal_write_line("  tree [/f] [path]     - DOS-style directory tree (/f includes files)");
	terminal_write_line("  cls/type/copy/move/del/ren - DOS aliases for clear/cat/cp/mv/rm/mv");
	terminal_write_line("  cd <path>            - Change directory");
	terminal_write_line("  mkdir <path>         - Create directory");
	terminal_write_line("  touch <path>         - Create empty file");
	terminal_write_line("  write <p> <text>     - Write text");
	terminal_write_line("  cat <path>           - Read file");
	terminal_write_line("  rm [-r] [-f] <path>  - Remove path (recursive/force)");
	terminal_write_line("  cp [-r] [-n] [-i] <src> <dst> - Copy file or directory tree");
	terminal_write_line("  mv [-n] [-i] <src> <dst>      - Move/rename file or directory");
	terminal_write_line("  edit <path>          - Edit text file");
	terminal_write_line("  hexedit <path>       - Edit raw bytes in hex");
	terminal_write_line("  run [-x] <path>      - Run script (-x echoes lines)");
	terminal_write_line("  basic <path>         - Run Tiny BASIC program");
	terminal_write_line("  elfinfo <path>       - Inspect ELF headers and symbol tables");
	terminal_write_line("  elfsym <path> [f]    - List symbols from an ELF image");
	terminal_write_line("  elfaddr <p> <addr>   - Resolve an address inside an ELF image");
	terminal_write_line("  exec <path>          - Load and run ELF64 kernel binary");
	terminal_write_line("  elfselftest          - Validate built-in ELF fixtures");
	terminal_write_line("  script: foreach i in a,b do echo $(i)");
	terminal_write_line("  fatmount [0|1]       - Mount FAT32 drive (0=master, 1=slave)");
	terminal_write_line("  drives               - List detected ATA drives");
	terminal_write_line("  ramfs                - Switch generic fs commands to RAM FS");
	terminal_write_line("  ramfs2fat [map]      - Copy RAM FS tree to FAT (or show name map)");
	terminal_write_line("  fatunmount           - Unmount FAT32 data disk");
	terminal_write_line("  fatls                - List FAT32 cwd");
	terminal_write_line("  fatcat <path>        - Read FAT32 file");
	terminal_write_line("  fattouch <path>      - Create FAT32 file");
	terminal_write_line("  fatwrite <p> <txt>   - Write FAT32 file");
	terminal_write_line("  fatattr <p> [mods]   - Show/set FAT attrs (+r -h +s +a)");
	terminal_write_line("  fatrm <path>         - Remove FAT32 path");
}

static void print_help_disk(void)
{
	terminal_write_line("Disk/ATA commands:");
	terminal_write_line("  ataid                - Show ATA presence + sector count");
	terminal_write_line("  readsec <lba-hex>    - Dump one 512-byte sector");
	terminal_write_line("  writesec <lba> <txt> - Write marker text to one sector");
}

struct manual_entry
{
	const char *name;
	const char *summary;
	const char *syntax;
	const char *description;
	const char *examples;
	const char *see_also;
	const char *keywords;
};

static const struct manual_entry manual_entries[] = {
	{"help", "show built-in help pages and command manuals", "help [basic|fs|disk|commands [page]|<command> [page]]", "Use help without arguments for the summary page. Use help with a section name or command name to jump directly to that topic.", "help\nhelp fs\nhelp echo", "man commands alias", "documentation manual topics sections"},
	{"man", "show one manual entry or search the manual index", "man <topic> [page]\nman -k <word>", "Manual pages are rendered in sections and split into pages automatically when they exceed the visible terminal height.", "man ls\nman echo 2\nman -k file", "help commands alias", "manual search keyword apropos"},
	{"alias", "create, update, or list shell command aliases", "alias\nalias <name> <command...>\nalias <name>=<command...>", "Aliases expand before command dispatch. Use them to add alternate names like dir for ls or to prefill arguments.", "alias dir ls\nalias bootinfo memstat\nalias", "unalias help man", "command alias shortcut rename"},
	{"unalias", "remove a shell command alias", "unalias <name>", "Deletes one alias from the runtime alias table.", "unalias dir", "alias help man", "command alias remove delete"},
	{"version", "print the TG11-OS version string", "version", "Shows the kernel version constant compiled into the image.", "version", "help", "build version release"},
	{"echo", "print text with escapes, colors, styles, and CP437 aliases", "echo <text>", "Supports escape sequences like \\n and \\xNN, color bytes with &NN or §NN, and style toggles with &l &i &u &s and uppercase variants to disable.", "echo hello\necho &1Fblue&r\necho &lBold &iItalic&r", "glyph charmap color", "print text color style escape serial framebuffer"},
	{"glyph", "print one raw CP437 byte", "glyph <0xNN>", "Writes a single CP437 glyph byte directly to the terminal.", "glyph 0xDB", "echo charmap", "cp437 glyph byte"},
	{"charmap", "list the CP437 alias escapes supported by echo", "charmap", "Shows the named escape aliases that expand into box-drawing and symbol characters.", "charmap", "echo glyph", "cp437 aliases symbols"},
	{"color", "inspect or change terminal colors", "color show\ncolor preview [text|prompt]\ncolor text <0xNN>\ncolor prompt <0xNN>", "The terminal uses classic VGA attribute bytes for text and prompt coloring in both VGA and framebuffer modes.", "color show\ncolor text 0x1F", "echo serial", "palette attribute vga framebuffer"},
	{"serial", "configure serial mirroring and compact output", "serial [on|off|show|compact <on|off>|rxecho <on|off>]", "Controls whether terminal output is mirrored to COM1 and whether repeated spaces are compacted for easier reading in serial logs.", "serial show\nserial compact on", "echo display", "com1 mirror debug logging"},
	{"display", "inspect or switch display modes", "display show\ndisplay vga25|vga50|fb\ndisplay mode <show|list|1080p|900p|768p|720p|WIDTHxHEIGHT[xBPP]>\ndisplay cursor <show|underline|block|bar>", "Switch between VGA text modes and framebuffer output, select explicit framebuffer resolutions, and choose cursor shape.", "display show\ndisplay fb\ndisplay mode 1280x720x32", "fbfont color", "video framebuffer vga mode cursor"},
	{"fbfont", "inspect or customize the framebuffer font", "fbfont [show|list|style <classic|blocky>|size <small|normal|large>|reset|glyph <ch> <r0..r6>|save <name>|load <name>]", "Manage the built-in framebuffer font, patch glyph rows manually, and persist named font variants under /fonts.", "fbfont list\nfbfont size large\nfbfont save chunky", "display themes", "font framebuffer glyph editor"},
	{"theme", "apply a system theme", "theme <name>", "Loads a theme from /themes and applies its terminal colors.", "theme ocean", "themes etheme ethemes", "theme palette colors"},
	{"themes", "list available system themes", "themes [list]", "Enumerates the theme files stored in /themes.", "themes", "theme ethemes", "theme list palette"},
	{"etheme", "apply or edit an editor theme", "etheme <name>\netheme edit <name>", "Loads an editor theme from /edit/themes or opens that file in the editor for modification.", "etheme amber\netheme edit amber", "ethemes theme edit", "editor theme syntax colors"},
	{"ethemes", "list available editor themes", "ethemes [list]", "Enumerates editor theme files from /edit/themes.", "ethemes", "etheme theme", "editor theme list"},
	{"clear", "clear the screen", "clear", "Clears the terminal contents and redraws the prompt.", "clear", "help", "cls clear screen"},
	{"reboot", "restart the machine", "reboot", "Requests an immediate reboot.", "reboot", "shutdown", "restart reset power"},
	{"panic", "deliberately trigger a kernel panic", "panic", "Triggers an invalid opcode exception on purpose. Useful for testing panic screen handling and reboot hotkeys like Esc+F12.", "panic", "reboot memstat", "crash panic test debug"},
	{"shutdown", "shut down the machine", "shutdown\nexit\nquit", "Stops the VM or machine using the supported power-off path.", "shutdown", "reboot", "power off quit exit"},
	{"pwd", "print the current working directory", "pwd", "Shows the shell's current working directory.", "pwd", "cd ls", "filesystem cwd path"},
	{"ls", "list directory entries", "ls [path]", "Lists files and directories from the current working directory or the given path.", "ls\nls /scripts", "cd pwd dir", "filesystem list directory"},
	{"dir", "show a Windows-style directory listing", "dir [/b] [/w] [/s] [path]", "Prints one entry per line with a type column and file sizes, followed by summary totals. /b is bare names, /w is wide names, and /s recursively lists each directory with its own totals plus a grand total.", "dir\ndir /w\ndir /s scripts", "ls tree cd pwd", "filesystem directory size windows dos"},
	{"tree", "show a DOS-style directory tree", "tree [/f] [path]", "Displays a recursive tree. Add /f to include files in addition to directories.", "tree\ntree /f scripts", "dir ls cd", "filesystem recursive tree dos"},
	{"cls", "DOS alias for clear", "cls", "Runs clear.", "cls", "clear", "dos compatibility alias"},
	{"type", "DOS alias for cat", "type <path>", "Runs cat on a file path.", "type readme.txt", "cat", "dos compatibility alias"},
	{"copy", "DOS alias for cp", "copy <src> <dst>", "Runs cp with the provided arguments.", "copy a.txt b.txt", "cp", "dos compatibility alias"},
	{"move", "DOS alias for mv", "move <src> <dst>", "Runs mv with the provided arguments.", "move a.txt b.txt", "mv", "dos compatibility alias"},
	{"del", "DOS alias for rm", "del <path>", "Runs rm with the provided arguments.", "del notes.txt", "rm", "dos compatibility alias"},
	{"ren", "DOS alias for mv", "ren <old> <new>", "Runs mv for simple rename semantics.", "ren old.txt new.txt", "mv", "dos compatibility alias"},
	{"cd", "change the current working directory", "cd <path>", "Moves the shell's current directory to another path.", "cd /scripts", "pwd ls", "filesystem cwd change directory"},
	{"mkdir", "create a directory", "mkdir <path>", "Creates one directory at the given path.", "mkdir /games", "ls touch rm", "filesystem directory create"},
	{"touch", "create an empty file", "touch <path>", "Creates a zero-length file if it does not already exist.", "touch notes.txt", "write cat rm", "filesystem file create"},
	{"write", "replace a file with text", "write <path> <text>", "Writes the provided text into a file, replacing any previous contents.", "write note.txt hello", "touch cat edit", "filesystem file write text"},
	{"cat", "print a file to the terminal", "cat <path>", "Reads a text file and writes it to the terminal.", "cat scripts/tic-tac-toe.bas", "write edit hexedit", "filesystem file read text"},
	{"rm", "remove a file or directory tree", "rm [-r] [-f] <path>", "Deletes one file or recursively removes a directory tree when -r is supplied. -f suppresses some errors.", "rm note.txt\nrm -r games", "cp mv mkdir", "filesystem delete remove recursive"},
	{"cp", "copy a file or directory tree", "cp [-r] [-n] [-i] <src> <dst>", "Copies files and optionally full directory trees. -n avoids overwrite, -i asks before overwrite when supported.", "cp note.txt note2.txt\ncp -r src backup", "mv rm", "filesystem copy duplicate"},
	{"mv", "move or rename a file or directory", "mv [-n] [-i] <src> <dst>", "Renames or moves files and directories. -n avoids overwrite, -i asks before overwrite when supported.", "mv old.txt new.txt", "cp rm", "filesystem move rename"},
	{"edit", "open the built-in text editor", "edit <path>", "Starts the text editor on the chosen file.", "edit readme.txt", "hexedit write cat", "editor text file"},
	{"hexedit", "open the built-in hex editor", "hexedit <path>", "Starts the hex editor for raw byte editing.", "hexedit kernel.bin", "edit hexdump", "editor hex bytes file"},
	{"run", "execute a shell script file", "run [-x] <path>", "Runs one script file through the shell. -x echoes each line before executing it.", "run boot.scr\nrun -x test.scr", "basic edit", "script shell batch"},
	{"basic", "run a Tiny BASIC program", "basic <path>", "Loads and runs a Tiny BASIC program from the filesystem.", "basic scripts/tic-tac-toe.bas", "run edit", "basic interpreter program"},
	{"elfinfo", "inspect ELF64 headers and symbol metadata", "elfinfo <path>", "Parses an ELF64 file without executing it and prints header fields, PT_LOAD range, and how many named defined symbols were found.", "elfinfo /app.elf", "elfsym elfaddr exec", "elf symbols debug headers metadata"},
	{"elfsym", "list named ELF64 symbols", "elfsym <path> [filter]", "Lists named defined symbols from SHT_SYMTAB or SHT_DYNSYM. Supply an optional filter substring to narrow the output.", "elfsym /app.elf\nelfsym /kernel/app.elf start", "elfinfo elfaddr exec", "elf symbols symtab debug lookup"},
	{"elfaddr", "resolve an address to the nearest ELF64 symbol", "elfaddr <path> <hex-address>", "Looks up the nearest named symbol at or before the given virtual address and reports the offset into that symbol.", "elfaddr /app.elf 0xFFFF900003000000", "elfinfo elfsym exec", "elf address symbol resolve debug"},
	{"exec", "load and call an ELF64 kernel binary", "exec <path>", "Loads a small ELF64 image into kernel memory, maps its PT_LOAD segments, and calls its entry point directly.", "exec app.elf", "memstat pagetest", "elf executable loader"},
	{"execstress", "run one ELF repeatedly and check memory delta", "execstress <count> <path>", "Loads, executes, and unloads the same ELF image count times, then reports free-page delta to help detect leaks while iterating on ELF/memory changes.", "execstress 100 app.elf", "exec memstat pagetest", "elf stress test memory leak"},
	{"elfselftest", "run built-in ELF functional and leak tests", "elfselftest", "Runs /app.elf, /appw.elf, and /app2p.elf checks (return values + load/unload stability) and performs a short stress loop with free-page delta reporting.", "elfselftest", "exec execstress memstat", "elf selftest memory loader"},
	{"hexdump", "dump memory in hex", "hexdump <address> [count]", "Reads memory starting from a hex address and shows a hex dump.", "hexdump 0x100000 64", "hexedit memmap", "memory dump debug"},
	{"memmap", "show the physical memory map", "memmap", "Displays the multiboot-provided physical memory map.", "memmap", "memstat pagetest", "memory multiboot map"},
	{"memstat", "show allocator and paging state", "memstat", "Displays total and free pages, the virtual allocation window, and the active CR3 value.", "memstat", "memmap pagetest exec", "memory paging allocator cr3"},
	{"pagetest", "exercise the paging allocator", "pagetest", "Allocates pages, writes a pattern, verifies it, unmaps it, and confirms the mapping is gone.", "pagetest", "memstat exec", "paging allocator self test"},
	{"pagefault", "trigger a controlled page fault", "pagefault <read|write|exec>", "Intentionally faults using an unmapped address. read dereferences it, write stores through it, and exec calls it as code so you can verify decoded #PF bits on the panic screen.", "pagefault read\npagefault write\npagefault exec", "panic pagetest memstat", "page fault pf cr2 panic test"},
	{"gpfault", "trigger a controlled general-protection fault", "gpfault", "Triggers #GP by dereferencing a non-canonical address in long mode.", "gpfault", "udfault pagefault panic", "general protection fault gp exception test"},
	{"udfault", "trigger a controlled invalid-opcode fault", "udfault", "Executes UD2 to intentionally trigger #UD.", "udfault", "gpfault pagefault panic", "invalid opcode ud exception test"},
	{"doublefault", "simulate double-fault recovery", "doublefault", "Simulates a double fault by directly invoking the recovery handler. A double fault occurs when an exception happens while handling another exception.", "doublefault", "udfault gpfault panic", "double fault df exception recovery test"},
	{"exceptstat", "display exception statistics", "exceptstat", "Shows the count of each exception type that has occurred since system boot. Useful for detecting repeated faults under load.", "exceptstat", "panic udfault gpfault", "exception statistics counter tracking diagnostics"},
	{"dumpstack", "dump current kernel call stack", "dumpstack", "Walks the RBP chain to display the current function call stack. Useful for runtime diagnostics before intentional faults.", "dumpstack", "exceptstat panic", "stack dump backtrace call chain"},
	{"selftest", "run guided built-in test suites", "selftest exceptions [pf-read|pf-write|pf-exec|ud|gp|1|2|3|4|5]", "Prints a reboot-friendly exception test plan, or runs one selected step and triggers the expected fault immediately.", "selftest exceptions\nselftest exceptions pf-exec\nselftest exceptions 4", "pagefault gpfault udfault panic", "selftest test harness exceptions diagnostics"},
	{"ataid", "show ATA identity information", "ataid", "Detects ATA drives and shows the available sector count.", "ataid", "drives readsec writesec", "ata disk identity"},
	{"readsec", "dump one 512-byte sector", "readsec <lba-hex>", "Reads and dumps one sector from the selected ATA device.", "readsec 0x20", "writesec ataid", "ata sector read"},
	{"writesec", "write text into one sector", "writesec <lba> <text>", "Writes marker text into a single sector for low-level disk testing.", "writesec 32 hello", "readsec ataid", "ata sector write"},
	{"drives", "list detected ATA drives", "drives", "Enumerates the ATA drives seen by the kernel.", "drives", "ataid fatmount", "ata disk list"},
	{"fatmount", "mount a FAT32 drive", "fatmount [0|1]", "Mounts the selected ATA disk as the active FAT32 data drive.", "fatmount 0", "fatunmount drives ramfs", "fat32 mount disk"},
	{"fatunmount", "unmount the FAT32 drive", "fatunmount", "Detaches the active FAT32 data drive.", "fatunmount", "fatmount ramfs", "fat32 unmount disk"},
	{"ramfs", "switch generic file commands back to RAM FS", "ramfs", "Makes generic file commands operate on the RAM filesystem again instead of the FAT volume.", "ramfs", "fatmount ramfs2fat", "ramfs filesystem switch"},
	{"ramfs2fat", "copy the RAM FS tree to FAT32", "ramfs2fat [map]", "Copies the RAM filesystem tree onto the mounted FAT32 volume or prints the filename mapping when map is supplied.", "ramfs2fat\nramfs2fat map", "ramfs fatmount", "ramfs fat32 copy sync"},
	{"fatls", "list FAT32 directory entries", "fatls", "Lists the contents of the current FAT32 working directory.", "fatls", "fatcat fattouch fatwrite", "fat32 list directory"},
	{"fatcat", "print one FAT32 file", "fatcat <path>", "Reads and prints a file from the mounted FAT32 volume.", "fatcat /notes.txt", "fatwrite fattouch", "fat32 file read"},
	{"fattouch", "create an empty FAT32 file", "fattouch <path>", "Creates a zero-length file on the mounted FAT32 volume.", "fattouch /new.txt", "fatwrite fatcat", "fat32 file create"},
	{"fatwrite", "replace a FAT32 file with text", "fatwrite <path> <text>", "Writes text into a FAT32 file, replacing the previous contents.", "fatwrite /note.txt hello", "fattouch fatcat", "fat32 file write"},
	{"fatattr", "inspect or modify FAT32 attributes", "fatattr <path> [mods]", "Shows or changes FAT attribute bits like read-only, hidden, system, and archive.", "fatattr /note.txt\nfatattr /note.txt +r", "fatls fatrm", "fat32 attributes readonly hidden"},
	{"fatrm", "remove a FAT32 file or directory", "fatrm <path>", "Deletes a file or directory from the mounted FAT32 volume.", "fatrm /note.txt", "fatls fatattr", "fat32 remove delete"}
};

static char ascii_lower(char c)
{
	if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
	return c;
}

static int string_contains_ci(const char *text, const char *needle)
{
	unsigned long i;
	unsigned long j;
	if (text == (void *)0 || needle == (void *)0 || needle[0] == '\0') return 0;
	for (i = 0; text[i] != '\0'; i++)
	{
		for (j = 0; needle[j] != '\0'; j++)
		{
			if (text[i + j] == '\0') return 0;
			if (ascii_lower(text[i + j]) != ascii_lower(needle[j])) break;
		}
		if (needle[j] == '\0') return 1;
	}
	return 0;
}

static int command_alias_index(const char *name)
{
	int i;
	if (name == (void *)0 || name[0] == '\0') return -1;
	for (i = 0; i < command_alias_count; i++)
	{
		if (string_equals(command_alias_names[i], name)) return i;
	}
	return -1;
}

static const char *command_alias_lookup(const char *name)
{
	int i = command_alias_index(name);
	if (i < 0) return (void *)0;
	return command_alias_expansions[i];
}

static const struct manual_entry *find_manual_entry(const char *topic)
{
	unsigned long i;
	for (i = 0; i < sizeof(manual_entries) / sizeof(manual_entries[0]); i++)
	{
		if (string_equals(manual_entries[i].name, topic)) return &manual_entries[i];
	}
	return (void *)0;
}

static unsigned int parse_manual_page_arg(const char *args, const char *usage, unsigned int *page_out)
{
	char tok[16];
	const char *rest;
	unsigned int page = 1;
	if (page_out == (void *)0) return 0;
	*page_out = 1;
	if (args == (void *)0) return 1;
	args = skip_spaces(args);
	if (args[0] == '\0') return 1;
	rest = read_token(args, tok, sizeof(tok));
	if (rest == (void *)0 || tok[0] == '\0')
	{
		terminal_write_line(usage);
		return 0;
	}
	if (parse_dec_u32(tok, &page) != 0 || page == 0 || skip_spaces(rest)[0] != '\0')
	{
		terminal_write_line(usage);
		return 0;
	}
	*page_out = page;
	return 1;
}

static unsigned int manual_text_line_count(const char *text)
{
	unsigned int count = 1;
	unsigned long i;
	if (text == (void *)0 || text[0] == '\0') return 0;
	for (i = 0; text[i] != '\0'; i++) if (text[i] == '\n') count++;
	return count;
}

static unsigned int manual_section_line_count(const char *body)
{
	if (body == (void *)0 || body[0] == '\0') return 0;
	return 1 + manual_text_line_count(body) + 1;
}

static void manual_emit_line(const char *line, unsigned int start, unsigned int end, unsigned int *line_no)
{
	if (*line_no >= start && *line_no < end) terminal_write_line(line);
	(*line_no)++;
}

static void manual_emit_text_block(const char *text, unsigned int start, unsigned int end, unsigned int *line_no)
{
	char line[160];
	unsigned long i = 0;
	unsigned long j = 0;
	if (text == (void *)0 || text[0] == '\0') return;
	for (;;)
	{
		if (text[i] == '\n' || text[i] == '\0')
		{
			line[j] = '\0';
			manual_emit_line(line, start, end, line_no);
			j = 0;
			if (text[i] == '\0') break;
			i++;
			continue;
		}
		if (j + 1 < sizeof(line)) line[j++] = text[i];
		i++;
	}
}

static void manual_emit_section(const char *title, const char *body, unsigned int start, unsigned int end, unsigned int *line_no)
{
	if (body == (void *)0 || body[0] == '\0') return;
	manual_emit_line(title, start, end, line_no);
	manual_emit_text_block(body, start, end, line_no);
	manual_emit_line("", start, end, line_no);
}

static void manual_make_name_line(const char *name, const char *summary, char *out, unsigned long out_size)
{
	unsigned long n = 0;
	unsigned long i = 0;
	if (out_size == 0) return;
	while (name[i] != '\0' && n + 1 < out_size) out[n++] = name[i++];
	if (summary != (void *)0 && summary[0] != '\0' && n + 4 < out_size)
	{
		out[n++] = ' ';
		out[n++] = '-';
		out[n++] = ' ';
		i = 0;
		while (summary[i] != '\0' && n + 1 < out_size) out[n++] = summary[i++];
	}
	out[n] = '\0';
}

static void render_manual_entry_page(const struct manual_entry *entry, const char *topic_name, unsigned int page)
{
	char header[96];
	char name_line[160];
	char page_buf[16];
	char total_buf[16];
	unsigned int per_page;
	unsigned int total_lines;
	unsigned int total_pages;
	unsigned int start;
	unsigned int end;
	unsigned int line_no = 0;

	per_page = (unsigned int)screen_get_height();
	if (per_page < 8) per_page = 12;
	else per_page -= 3;

	manual_make_name_line(topic_name, entry->summary, name_line, sizeof(name_line));
	total_lines = 2;
	total_lines += manual_section_line_count(name_line);
	total_lines += manual_section_line_count(entry->syntax);
	total_lines += manual_section_line_count(entry->description);
	total_lines += manual_section_line_count(entry->examples);
	total_lines += manual_section_line_count(entry->see_also);
	if (total_lines == 0) total_lines = 1;
	total_pages = (total_lines + per_page - 1) / per_page;
	if (total_pages == 0) total_pages = 1;
	if (page > total_pages) page = total_pages;
	start = (page - 1) * per_page;
	end = start + per_page;

	uint_to_dec((unsigned long)page, page_buf, sizeof(page_buf));
	uint_to_dec((unsigned long)total_pages, total_buf, sizeof(total_buf));
	manual_make_name_line("Manual", topic_name, header, sizeof(header));
	manual_emit_line(header, start, end, &line_no);
	manual_emit_line("", start, end, &line_no);
	manual_emit_section("NAME", name_line, start, end, &line_no);
	manual_emit_section("SYNOPSIS", entry->syntax, start, end, &line_no);
	manual_emit_section("DESCRIPTION", entry->description, start, end, &line_no);
	manual_emit_section("EXAMPLES", entry->examples, start, end, &line_no);
	manual_emit_section("SEE ALSO", entry->see_also, start, end, &line_no);

	if (total_pages > 1)
	{
		terminal_write("Page ");
		terminal_write(page_buf);
		terminal_write("/");
		terminal_write_line(total_buf);
		terminal_write("Use: man ");
		terminal_write(topic_name);
		terminal_write_line(" <page>");
	}
}

static void render_alias_manual(const char *alias_name, const char *expansion)
{
	char target[COMMAND_ALIAS_NAME_LEN];
	char description[160];
	char see_also[64];
	const char *p;
	const struct manual_entry *entry;
	unsigned int n = 0;
	p = read_token(expansion, target, sizeof(target));
	entry = find_manual_entry(target);
	manual_emit_line("NAME", 0, 1000, &n);
	terminal_write("  "); terminal_write(alias_name); terminal_write(" - alias for "); terminal_write_line(expansion);
	manual_emit_line("", 0, 1000, &n);
	manual_emit_line("SYNOPSIS", 0, 1000, &n);
	terminal_write("  "); terminal_write(alias_name); terminal_write_line(" [args...]");
	manual_emit_line("", 0, 1000, &n);
	description[0] = '\0';
	manual_make_name_line("  Expands to", expansion, description, sizeof(description));
	manual_emit_line("DESCRIPTION", 0, 1000, &n);
	terminal_write_line(description);
	if (entry != (void *)0)
	{
		terminal_write("  Target summary: ");
		terminal_write_line(entry->summary);
	}
	manual_emit_line("", 0, 1000, &n);
	see_also[0] = '\0';
	manual_make_name_line("  alias", "unalias help", see_also, sizeof(see_also));
	manual_emit_line("SEE ALSO", 0, 1000, &n);
	terminal_write_line(see_also);
	(void)p;
}

static void print_manual_keyword_matches(const char *keyword)
{
	unsigned long i;
	int found = 0;
	if (keyword == (void *)0 || keyword[0] == '\0')
	{
		terminal_write_line("Usage: man -k <word>");
		return;
	}
	for (i = 0; i < sizeof(manual_entries) / sizeof(manual_entries[0]); i++)
	{
		if (string_contains_ci(manual_entries[i].name, keyword) ||
		    string_contains_ci(manual_entries[i].summary, keyword) ||
		    string_contains_ci(manual_entries[i].description, keyword) ||
		    string_contains_ci(manual_entries[i].keywords, keyword))
		{
			terminal_write("  ");
			terminal_write(manual_entries[i].name);
			terminal_write(" - ");
			terminal_write_line(manual_entries[i].summary);
			found = 1;
		}
	}
	for (i = 0; i < (unsigned long)command_alias_count; i++)
	{
		if (string_contains_ci(command_alias_names[i], keyword) || string_contains_ci(command_alias_expansions[i], keyword))
		{
			terminal_write("  ");
			terminal_write(command_alias_names[i]);
			terminal_write(" - alias for ");
			terminal_write_line(command_alias_expansions[i]);
			found = 1;
		}
	}
	if (!found) terminal_write_line("No manual entries matched.");
}

static int print_manual_entry(const char *topic, const char *args)
{
	unsigned int page = 1;
	const struct manual_entry *entry;
	const char *alias_expansion;
	if (topic == (void *)0 || topic[0] == '\0') return 0;

	if (string_equals(topic, "basic"))
	{
		print_help_basic();
		return 1;
	}
	if (string_equals(topic, "fs"))
	{
		print_help_fs();
		return 1;
	}
	if (string_equals(topic, "disk"))
	{
		print_help_disk();
		return 1;
	}
	if (string_equals(topic, "commands") || string_equals(topic, "cmds"))
	{
		print_help_commands(args);
		return 1;
	}

	alias_expansion = command_alias_lookup(topic);
	if (alias_expansion != (void *)0)
	{
		render_alias_manual(topic, alias_expansion);
		return 1;
	}

	entry = find_manual_entry(topic);
	if (entry == (void *)0) return 0;
	if (!parse_manual_page_arg(args, "Usage: man <topic> [page]", &page)) return 1;
	render_manual_entry_page(entry, topic, page);
	return 1;
}

static void cmd_help(const char *args)
{
	char topic[16];
	const char *rest = read_token(args, topic, sizeof(topic));
	if (rest == (void *)0 || topic[0] == '\0' || string_equals(topic, "basic"))
	{
		print_help_basic();
		terminal_write_line("Try 'help fs', 'help disk', 'help commands', 'man echo', or 'man -k file'.");
		return;
	}
	if (print_manual_entry(topic, rest))
	{
		return;
	}
	terminal_write_line("Usage: help [basic|fs|disk|commands [page]|<command> [page]]");
}

static void cmd_man(const char *args)
{
	char topic[16];
	const char *rest = read_token(args, topic, sizeof(topic));
	if (rest == (void *)0 || topic[0] == '\0')
	{
		terminal_write_line("Usage: man <topic> [page]");
		terminal_write_line("Usage: man -k <word>");
		terminal_write_line("Examples: man ls, man echo 2, man -k file");
		return;
	}
	if (string_equals(topic, "-k"))
	{
		char keyword[32];
		if (read_token(rest, keyword, sizeof(keyword)) == (void *)0 || keyword[0] == '\0')
		{
			terminal_write_line("Usage: man -k <word>");
			return;
		}
		print_manual_keyword_matches(keyword);
		return;
	}
	if (!print_manual_entry(topic, rest))
	{
		terminal_write_line("No manual entry for that topic.");
	}
}

static void terminal_putc(char c)
{
	screen_putchar(c);

	if (serial_ready && serial_mirror_enabled)
	{
		static int serial_prev_space = 0;

		if (serial_compact_enabled)
		{
			if (c == '\n' || c == '\r')
			{
				serial_prev_space = 0;
				if (c == '\n') serial_putchar('\r');
				serial_putchar(c);
			}
			else if (c == '\t' || c == ' ')
			{
				if (!serial_prev_space)
				{
					serial_putchar(' ');
					serial_prev_space = 1;
				}
			}
			else
			{
				serial_prev_space = 0;
				serial_putchar(c);
			}
		}
		else
		{
			serial_prev_space = 0;
			if (c == '\n') serial_putchar('\r');
			serial_putchar(c);
		}
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

static void terminal_draw_prompt_prefix(void)
{
	screen_set_color(terminal_prompt_color);
	screen_write_char_at((unsigned short)(prompt_vga_start - 2), '>');
	screen_write_char_at((unsigned short)(prompt_vga_start - 1), ' ');
	screen_set_color(terminal_text_color);
}

static void terminal_prompt(void)
{
	screen_set_color(terminal_prompt_color);
	terminal_write("> ");
	screen_set_color(terminal_text_color);
	prompt_vga_start = screen_get_pos();
	cursor_pos = 0;
	terminal_last_drawn_length = 0;
	terminal_clear_selection();
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
	if (s == (void *)0) return "";
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

static int char_is_basic_word(char c)
{
	if (char_is_word(c)) return 1;
	return c == '$';
}

static int starts_with_basic_word_ci(const char *s, const char *word, unsigned long *word_len_out)
{
	unsigned long i = 0;
	while (word[i] != '\0')
	{
		if (ascii_upper(s[i]) != ascii_upper(word[i])) return 0;
		i++;
	}
	if (word_len_out != (void *)0) *word_len_out = i;
	if (char_is_basic_word(s[i])) return 0;
	return 1;
}

static int token_is_keyword_ci_table(const char *s, unsigned long len, char keywords[][EDITOR_THEME_KEYWORD_LEN], unsigned long keyword_count)
{
	unsigned long k;
	for (k = 0; k < keyword_count; k++)
	{
		unsigned long i = 0;
		while (keywords[k][i] != '\0' && i < len)
		{
			if (ascii_upper(keywords[k][i]) != ascii_upper(s[i])) break;
			i++;
		}
		if (i == len && keywords[k][i] == '\0') return 1;
	}
	return 0;
}

static void copy_keyword_table(char dst[][EDITOR_THEME_KEYWORD_LEN], unsigned long *dst_count, char src[][EDITOR_THEME_KEYWORD_LEN], unsigned long src_count)
{
	unsigned long i;
	unsigned long j;
	if (src_count > EDITOR_THEME_KEYWORD_MAX) src_count = EDITOR_THEME_KEYWORD_MAX;
	for (i = 0; i < src_count; i++)
	{
		for (j = 0; j + 1 < EDITOR_THEME_KEYWORD_LEN && src[i][j] != '\0'; j++) dst[i][j] = src[i][j];
		dst[i][j] = '\0';
	}
	*dst_count = src_count;
}

static int parse_keyword_list(const char *value, char out[][EDITOR_THEME_KEYWORD_LEN], unsigned long max_keywords, unsigned long *out_count)
{
	unsigned long count = 0;
	unsigned long i = 0;
	if (value == (void *)0 || out == (void *)0 || out_count == (void *)0 || max_keywords == 0) return -1;

	while (value[i] != '\0')
	{
		unsigned long token_len = 0;
		while (value[i] == ' ' || value[i] == '\t' || value[i] == ',') i++;
		if (value[i] == '\0') break;
		if (count >= max_keywords) break;

		while (value[i] != '\0' && value[i] != ',')
		{
			if (token_len + 1 < EDITOR_THEME_KEYWORD_LEN) out[count][token_len++] = value[i];
			i++;
		}

		while (token_len > 0 && (out[count][token_len - 1] == ' ' || out[count][token_len - 1] == '\t')) token_len--;
		out[count][token_len] = '\0';
		if (token_len > 0) count++;
		if (value[i] == ',') i++;
	}

	if (count == 0) return -1;
	*out_count = count;
	return 0;
}

static int parse_custom_theme_key(const char *key, const char *suffix, unsigned long *group_index_out)
{
	unsigned long i;
	if (key == (void *)0 || suffix == (void *)0 || group_index_out == (void *)0) return 0;
	if (key[0] != 'c' || key[1] != 'u' || key[2] != 's' || key[3] != 't') return 0;
	if (key[4] < '1' || key[4] > ('0' + EDITOR_THEME_CUSTOM_GROUP_MAX)) return 0;
	if (key[5] != '_') return 0;
	i = 0;
	while (suffix[i] != '\0')
	{
		if (key[6 + i] != suffix[i]) return 0;
		i++;
	}
	if (key[6 + i] != '\0') return 0;
	*group_index_out = (unsigned long)(key[4] - '1');
	return 1;
}

static int token_is_word_only(const char *s)
{
	unsigned long i = 0;
	if (s == (void *)0 || s[0] == '\0') return 0;
	while (s[i] != '\0')
	{
		if (!char_is_word(s[i])) return 0;
		i++;
	}
	return 1;
}

static int editor_match_custom_token_at(const char *buffer, unsigned long length, unsigned long index, unsigned char *color_out, unsigned long *len_out)
{
	unsigned long g;
	unsigned long k;
	unsigned long best_len = 0;
	unsigned char best_color = 0;

	if (buffer == (void *)0 || color_out == (void *)0 || len_out == (void *)0) return 0;
	if (index >= length) return 0;

	for (g = 0; g < EDITOR_THEME_CUSTOM_GROUP_MAX; g++)
	{
		for (k = 0; k < editor_custom_token_counts[g]; k++)
		{
			unsigned long i = 0;
			int match = 1;
			char *token = editor_custom_tokens[g][k];
			if (token[0] == '\0') continue;

			while (token[i] != '\0')
			{
				if (index + i >= length)
				{
					match = 0;
					break;
				}
				if (ascii_upper(buffer[index + i]) != ascii_upper(token[i]))
				{
					match = 0;
					break;
				}
				i++;
			}
			if (!match || i == 0) continue;

			if (token_is_word_only(token))
			{
				if (index > 0 && char_is_word(buffer[index - 1])) continue;
				if (index + i < length && char_is_word(buffer[index + i])) continue;
			}

			if (i > best_len)
			{
				best_len = i;
				best_color = editor_custom_colors[g];
			}
		}
	}

	if (best_len == 0) return 0;
	*color_out = best_color;
	*len_out = best_len;
	return 1;
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
	if (!editor_active)
	{
		screen_clear();
		prompt_vga_start = screen_get_pos();
		terminal_last_drawn_length = 0;
	}
	if (!editor_active && !script_mode_active && prompt_vga_start >= 2)
	{
		terminal_redraw_input_line();
	}

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
	unsigned long grp;
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
	char sh_keywords[EDITOR_THEME_KEYWORD_MAX][EDITOR_THEME_KEYWORD_LEN];
	unsigned long sh_keyword_count = 0;
	char basic_keywords[EDITOR_THEME_KEYWORD_MAX][EDITOR_THEME_KEYWORD_LEN];
	unsigned long basic_keyword_count = 0;
	unsigned char custom_colors[EDITOR_THEME_CUSTOM_GROUP_MAX];
	char custom_tokens[EDITOR_THEME_CUSTOM_GROUP_MAX][EDITOR_THEME_KEYWORD_MAX][EDITOR_THEME_KEYWORD_LEN];
	unsigned long custom_token_counts[EDITOR_THEME_CUSTOM_GROUP_MAX];
	const char *p = text;

	copy_keyword_table(sh_keywords, &sh_keyword_count, editor_sh_keywords, editor_sh_keyword_count);
	copy_keyword_table(basic_keywords, &basic_keyword_count, editor_basic_keywords, editor_basic_keyword_count);
	for (grp = 0; grp < EDITOR_THEME_CUSTOM_GROUP_MAX; grp++)
	{
		custom_colors[grp] = editor_custom_colors[grp];
		copy_keyword_table(custom_tokens[grp], &custom_token_counts[grp], editor_custom_tokens[grp], editor_custom_token_counts[grp]);
	}

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

		if (string_equals(key, "sh_keywords"))
		{
			unsigned long parsed_count;
			if (parse_keyword_list(val, sh_keywords, EDITOR_THEME_KEYWORD_MAX, &parsed_count) == 0) sh_keyword_count = parsed_count;
			continue;
		}
		if (string_equals(key, "basic_keywords"))
		{
			unsigned long parsed_count;
			if (parse_keyword_list(val, basic_keywords, EDITOR_THEME_KEYWORD_MAX, &parsed_count) == 0) basic_keyword_count = parsed_count;
			continue;
		}
		if (parse_custom_theme_key(key, "keywords", &grp) ||
			parse_custom_theme_key(key, "tokens", &grp) ||
			parse_custom_theme_key(key, "words", &grp) ||
			parse_custom_theme_key(key, "others", &grp))
		{
			unsigned long parsed_count;
			if (parse_keyword_list(val, custom_tokens[grp], EDITOR_THEME_KEYWORD_MAX, &parsed_count) == 0) custom_token_counts[grp] = parsed_count;
			continue;
		}

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
		else if (parse_custom_theme_key(key, "color", &grp) ||
			parse_custom_theme_key(key, "keyword", &grp) ||
			parse_custom_theme_key(key, "comment", &grp) ||
			parse_custom_theme_key(key, "string", &grp) ||
			parse_custom_theme_key(key, "other", &grp))
		{
			custom_colors[grp] = c;
		}
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
	for (grp = 0; grp < EDITOR_THEME_CUSTOM_GROUP_MAX; grp++)
	{
		editor_custom_colors[grp] = custom_colors[grp];
		copy_keyword_table(editor_custom_tokens[grp], &editor_custom_token_counts[grp], custom_tokens[grp], custom_token_counts[grp]);
	}
	copy_keyword_table(editor_sh_keywords, &editor_sh_keyword_count, sh_keywords, sh_keyword_count);
	copy_keyword_table(editor_basic_keywords, &editor_basic_keyword_count, basic_keywords, basic_keyword_count);
	if (editor_active) screen_set_color(editor_text_color);
	if (editor_active) editor_render();

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
	fs_mkdir(FBFONT_DIR);
	fs_mkdir(ETC_DIR);
	fs_mkdir("/scripts");
	fs_mkdir("/edit");
	fs_mkdir(EDITOR_THEME_DIR);

	if (fs_read_text(MOTD_PATH, &existing) != 0)
	{
		fs_write_text(MOTD_PATH,
			"Welcome to TG11-OS!\n"
			"Type 'help' to list commands.\n"
			"Type 'man <topic>' for details.\n");
	}
	if (fs_read_text(AUTORUN_PATH, &existing) != 0)
	{
		fs_write_text(AUTORUN_PATH,
			"# TG11 boot autorun script\n"
			"# one command per line, same syntax as manual shell scripts\n"
			"# examples:\n"
			"# theme synthwave\n"
			"# shellwatch on\n");
	}
	if (fs_read_text(AUTORUN_MODE_PATH, &existing) != 0)
	{
		fs_write_text(AUTORUN_MODE_PATH, "off\n");
	}
	if (fs_read_text(AUTORUN_ONCE_STATE_PATH, &existing) != 0)
	{
		fs_write_text(AUTORUN_ONCE_STATE_PATH, "0\n");
	}
	if (fs_read_text(AUTORUN_DELAY_PATH, &existing) != 0)
	{
		fs_write_text(AUTORUN_DELAY_PATH, "8\n");
	}

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
	if (fs_read_text(SYSTEM_THEME_DIR "/synthwave.theme", &existing) != 0)
	{
		fs_write_text(SYSTEM_THEME_DIR "/synthwave.theme",
			"# Synthwave system palette\n"
			"terminal_text=0x5E\n"
			"terminal_prompt=0x5F\n");
	}

	if (fs_read_text(FBFONT_DIR "/default.fbf", &existing) != 0)
	{
		fs_write_text(FBFONT_DIR "/default.fbf",
			"# TG11 default framebuffer font profile\n"
			"style=classic\n"
			"size=normal\n");
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
			"sh_keywords=if,then,else,fi,for,do,done,while,in,case,esac,function,echo,exit,cd,ls\n"
			"basic_keyword=0x0D\n"
			"basic_comment=0x08\n"
			"basic_string=0x0B\n"
			"basic_keywords=PRINT,LET,DIM,IF,THEN,GOTO,GOSUB,RETURN,ON,FOR,TO,STEP,NEXT,INPUT,DATA,READ,RESTORE,TAB,SPC,ABS,RND,LEN,VAL,ASC,CHR$,STR$,REM,END,STOP,LIST,RUN\n"
			"# Custom groups: cust1..cust4\n"
			"# Use custN_color plus custN_keywords (or custN_tokens)\n"
			"# Example Markdown symbols:\n"
			"# cust1_color=0x0E\n"
			"# cust1_keywords=#,##,###,*,**,(),[],---\n");
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
			"sh_keywords=if,then,else,fi,for,do,done,while,in,case,esac,function,echo,exit,cd,ls\n"
			"basic_keyword=0x1D\n"
			"basic_comment=0x13\n"
			"basic_string=0x1E\n"
			"basic_keywords=PRINT,LET,DIM,IF,THEN,GOTO,GOSUB,RETURN,ON,FOR,TO,STEP,NEXT,INPUT,DATA,READ,RESTORE,TAB,SPC,ABS,RND,LEN,VAL,ASC,CHR$,STR$,REM,END,STOP,LIST,RUN\n"
			"# Custom groups available: cust1..cust4\n");
	}
	if (fs_read_text(EDITOR_THEME_DIR "/synthwave.theme", &existing) != 0)
	{
		fs_write_text(EDITOR_THEME_DIR "/synthwave.theme",
			"# Synthwave editor palette\n"
			"editor_header=0x5E\n"
			"editor_path=0x5D\n"
			"editor_rule=0x58\n"
			"editor_line=0x5D\n"
			"editor_text=0x5E\n"
			"sh_keyword=0x5E\n"
			"sh_comment=0x5D\n"
			"sh_string=0x5B\n"
			"sh_keywords=if,then,else,fi,for,do,done,while,in,case,esac,function,echo,exit,cd,ls\n"
			"basic_keyword=0x5E\n"
			"basic_comment=0x5D\n"
			"basic_string=0x5B\n"
			"basic_keywords=PRINT,LET,DIM,IF,THEN,GOTO,GOSUB,RETURN,ON,FOR,TO,STEP,NEXT,INPUT,DATA,READ,RESTORE,TAB,SPC,ABS,RND,LEN,VAL,ASC,CHR$,STR$,REM,END,STOP,LIST,RUN\n"
			"# Markdown-style custom groups\n"
			"cust1_color=0x5E\n"
			"cust1_keywords=#,##,###,####,#####,######,---\n"
			"cust2_color=0x5D\n"
			"cust2_keywords=*,**,_,__,`\n"
			"cust3_color=0x5B\n"
			"cust3_keywords=(,),[,],{,}\n");
	}
	if (fs_read_text("/scripts/demo.sh", &existing) != 0)
	{
		fs_write_text("/scripts/demo.sh",
			"# TG11-OS shell demo\n"
			"echo --- TG11 DEMO START ---\n"
			"echo Version: $(version)\n"
			"echo Working dir: $(pwd)\n"
			"touch demo.txt\n"
			"write demo.txt Hello from TG11 demo script on $(version)\n"
			"echo demo.txt says:\n"
			"cat demo.txt\n"
			"foreach item in alpha,beta,gamma do echo loop item = $(item)\n"
			"echo --- TG11 DEMO END ---\n");
	}
	if (fs_read_text("/scripts/c64-demo.sh", &existing) != 0)
	{
		fs_write_text("/scripts/c64-demo.sh",
			"# TG11 shell demo for visuals\n"
			"clear"
			"echo --- C64 style demo start ---\n"
			"theme c64\n"
			"color preview prompt\n"
			"echo \\boxul\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxur\n"
			"echo \\boxv TG11 C64 THEME \\boxv\n"
			"echo \\boxll\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxh\\boxlr\n"
			"echo \\blk\\blk\\blk  \\shade1\\shade2\\shade3\n"
			"theme default\n"
			"color text 0x0F\n"
			"color prompt 0x0B\n"
			"echo --- C64 style demo end ---\n");
	}
	if (fs_read_text("/scripts/demo.bas", &existing) != 0)
	{
		fs_write_text("/scripts/demo.bas",
			"10 PRINT \"TG11 Tiny BASIC demo\"\n"
			"20 LET A = 0\n"
			"30 PRINT A\n"
			"40 ADD A 1\n"
			"50 IF A < 5 THEN 30\n"
			"60 PRINT \"Done counting\"\n"
			"70 END\n");
	}
	if (fs_read_text("/scripts/tiny-basic-demo.bas", &existing) != 0)
	{
		fs_write_text("/scripts/tiny-basic-demo.bas",
			"10 PRINT \"TG11 Tiny BASIC v4 demo\";\n"
			"20 PRINT \" (arrays + funcs)\"\n"
			"30 DIM A(5)\n"
			"40 LET NAME$ = \"friend\"\n"
			"50 INPUT \"Your name\"; NAME$\n"
			"60 PRINT \"Hello, \", NAME$\n"
			"70 LET SUM = 0\n"
			"80 FOR I = 0 TO 5\n"
			"90 LET A(I) = I * I\n"
			"100 ADD SUM A(I)\n"
			"110 NEXT I\n"
			"120 PRINT \"SUM=\"; SPC(1); SUM\n"
			"130 PRINT \"LEN(name)=\", LEN(NAME$), \" ASC(first)=\", ASC(NAME$)\n"
			"140 PRINT \"VAL(\\\"123\\\")=\", VAL(\"123\"), \" ABS(-9)=\", ABS(-9)\n"
			"150 INPUT \"Pick mode (1..3)\"; MODE\n"
			"160 ON MODE GOSUB 300,320,340\n"
			"170 RESTORE\n"
			"180 READ X, Y, LABEL$\n"
			"190 PRINT TAB(2), \"DATA:\", X, Y, LABEL$\n"
			"200 PRINT \"RND(10)=\", RND(10), \" CHR$(33)=\", CHR$(33)\n"
			"210 PRINT \"STR$(SUM)=\", STR$(SUM)\n"
			"220 PRINT \"Done\"\n"
			"230 END\n"
			"300 PRINT \"Mode 1\";\n"
			"310 PRINT \" selected\"\n"
			"315 RETURN\n"
			"320 PRINT \"Mode 2 selected\"\n"
			"330 RETURN\n"
			"340 PRINT \"Mode 3 selected\"\n"
			"350 RETURN\n"
			"900 DATA 7, 42, \"synthwave\"\n");
	}
	if (fs_read_text("/scripts/tic-tac-toe.bas", &existing) != 0)
	{
		fs_write_text("/scripts/tic-tac-toe.bas",
			"10 CLS\n"
			"20 DIM B(9)\n"
			"30 P = 1\n"
			"40 FOR I = 1 TO 9\n"
			"50 B(I) = 0\n"
			"60 NEXT I\n"
			"70 GOSUB 500\n"
			"80 PRINT\n"
			"90 IF P = 1 THEN 120\n"
			"100 PRINT \"Player O, choose a square (1-9): \";\n"
			"110 GOTO 130\n"
			"120 PRINT \"Player X, choose a square (1-9): \";\n"
			"130 INPUT M\n"
			"140 IF M < 1 THEN 170\n"
			"150 IF M > 9 THEN 170\n"
			"160 GOTO 190\n"
			"170 PRINT \"Invalid move.\"\n"
			"180 GOTO 70\n"
			"190 IF B(M) <> 0 THEN 210\n"
			"200 GOTO 230\n"
			"210 PRINT \"That square is taken.\"\n"
			"220 GOTO 70\n"
			"230 B(M) = P\n"
			"240 GOSUB 1000\n"
			"250 IF W = 0 THEN 280\n"
			"260 GOSUB 1500\n"
			"270 END\n"
			"280 GOSUB 2000\n"
			"290 IF D = 0 THEN 320\n"
			"300 GOSUB 1700\n"
			"310 END\n"
			"320 IF P = 1 THEN 340\n"
			"330 P = 1\n"
			"335 GOTO 70\n"
			"340 P = 2\n"
			"350 GOTO 70\n"
			"500 CLS\n"
			"510 PRINT \" TIC-TAC-TOE\"\n"
			"520 PRINT\n"
			"530 FOR R = 0 TO 2\n"
			"540 A = R * 3 + 1\n"
			"550 GOSUB 800\n"
			"560 PRINT \" \"; C$;\n"
			"570 PRINT \" |\";\n"
			"580 A = R * 3 + 2\n"
			"590 GOSUB 800\n"
			"600 PRINT \" \"; C$;\n"
			"610 PRINT \" |\";\n"
			"620 A = R * 3 + 3\n"
			"630 GOSUB 800\n"
			"640 PRINT \" \"; C$\n"
			"650 IF R < 2 THEN 670\n"
			"660 GOTO 680\n"
			"670 PRINT \"---+---+---\"\n"
			"680 NEXT R\n"
			"690 RETURN\n"
			"800 IF B(A) = 1 THEN 840\n"
			"810 IF B(A) = 2 THEN 860\n"
			"820 C$ = STR$(A)\n"
			"830 RETURN\n"
			"840 C$ = \"X\"\n"
			"850 RETURN\n"
			"860 C$ = \"O\"\n"
			"870 RETURN\n"
			"1000 W = 0\n"
			"1010 GOSUB 1100\n"
			"1020 IF W <> 0 THEN 1070\n"
			"1030 GOSUB 1230\n"
			"1040 IF W <> 0 THEN 1070\n"
			"1050 GOSUB 1360\n"
			"1060 IF W <> 0 THEN 1070\n"
			"1070 RETURN\n"
			"1100 IF B(1) = 0 THEN 1140\n"
			"1110 IF B(1) = B(2) THEN 1120\n"
			"1115 GOTO 1140\n"
			"1120 IF B(2) = B(3) THEN 1130\n"
			"1125 GOTO 1140\n"
			"1130 W = B(1)\n"
			"1135 RETURN\n"
			"1140 IF B(4) = 0 THEN 1180\n"
			"1150 IF B(4) = B(5) THEN 1160\n"
			"1155 GOTO 1180\n"
			"1160 IF B(5) = B(6) THEN 1170\n"
			"1165 GOTO 1180\n"
			"1170 W = B(4)\n"
			"1175 RETURN\n"
			"1180 IF B(7) = 0 THEN 1220\n"
			"1190 IF B(7) = B(8) THEN 1200\n"
			"1195 GOTO 1220\n"
			"1200 IF B(8) = B(9) THEN 1210\n"
			"1205 GOTO 1220\n"
			"1210 W = B(7)\n"
			"1215 RETURN\n"
			"1220 RETURN\n"
			"1230 IF B(1) = 0 THEN 1270\n"
			"1240 IF B(1) = B(4) THEN 1250\n"
			"1245 GOTO 1270\n"
			"1250 IF B(4) = B(7) THEN 1260\n"
			"1255 GOTO 1270\n"
			"1260 W = B(1)\n"
			"1265 RETURN\n"
			"1270 IF B(2) = 0 THEN 1310\n"
			"1280 IF B(2) = B(5) THEN 1290\n"
			"1285 GOTO 1310\n"
			"1290 IF B(5) = B(8) THEN 1300\n"
			"1295 GOTO 1310\n"
			"1300 W = B(2)\n"
			"1305 RETURN\n"
			"1310 IF B(3) = 0 THEN 1350\n"
			"1320 IF B(3) = B(6) THEN 1330\n"
			"1325 GOTO 1350\n"
			"1330 IF B(6) = B(9) THEN 1340\n"
			"1335 GOTO 1350\n"
			"1340 W = B(3)\n"
			"1345 RETURN\n"
			"1350 RETURN\n"
			"1360 IF B(1) = 0 THEN 1400\n"
			"1370 IF B(1) = B(5) THEN 1380\n"
			"1375 GOTO 1400\n"
			"1380 IF B(5) = B(9) THEN 1390\n"
			"1385 GOTO 1400\n"
			"1390 W = B(1)\n"
			"1395 RETURN\n"
			"1400 IF B(3) = 0 THEN 1440\n"
			"1410 IF B(3) = B(5) THEN 1420\n"
			"1415 GOTO 1440\n"
			"1420 IF B(5) = B(7) THEN 1430\n"
			"1425 GOTO 1440\n"
			"1430 W = B(3)\n"
			"1435 RETURN\n"
			"1440 RETURN\n"
			"1500 GOSUB 500\n"
			"1510 PRINT\n"
			"1520 IF W = 1 THEN 1540\n"
			"1530 PRINT \"Player O wins!\"\n"
			"1535 RETURN\n"
			"1540 PRINT \"Player X wins!\"\n"
			"1550 RETURN\n"
			"1700 GOSUB 500\n"
			"1710 PRINT\n"
			"1720 PRINT \"It's a draw!\"\n"
			"1730 RETURN\n"
			"2000 D = 1\n"
			"2010 FOR I = 1 TO 9\n"
			"2020 IF B(I) = 0 THEN 2040\n"
			"2030 GOTO 2050\n"
			"2040 D = 0\n"
			"2050 NEXT I\n"
			"2060 RETURN\n");
	}
	if (fs_read_text("/scripts/colors.sh", &existing) != 0)
	{
		fs_write_text("/scripts/colors.sh",
			"# TG11 color preview demo\n"
			"echo --- Color Preview Demo ---\n"
			"color preview\n"
			"color preview text\n"
			"color preview prompt\n"
			"echo --- End ---\n");
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
		{"blkup", 0xDF}, {"blkdn", 0xDC}, 
		{"blkl", 0xDD}, {"blkr", 0xDE}, {"blk", 0xDB},
		{"dboxh", 0xCD}, {"dboxv", 0xBA}, {"dboxul", 0xC9}, {"dboxur", 0xBB},
		{"dboxll", 0xC8}, {"dboxlr", 0xBC},
		{"shade1", 0xB0}, {"shade2", 0xB1}, {"shade3", 0xB2},
		{"deg", 0xF8}, {"pm", 0xF1}, {"dot", 0xFA},
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

static unsigned char color_bold_variant(unsigned char color)
{
	return (unsigned char)((color & 0xF0) | ((color & 0x0F) | 0x08));
}

static void terminal_write_colored(const char *text, unsigned char color)
{
	unsigned char prev = screen_get_color();
	screen_set_color(color);
	terminal_write(text);
	screen_set_color(prev);
}

static void terminal_write_echo_text(const char *text)
{
	unsigned char base_color = terminal_text_color;
	unsigned char base_style = 0;
	unsigned long i = 0;
	screen_set_color(base_color);
	screen_set_style(base_style);

	while (text[i] != '\0')
	{
		if (text[i] == (char)0xA7 || text[i] == '&')
		{
			/* Style codes: l/L=bold, i/I=italic, u/U=underline, s/S=strike, r=reset */
			/* All non-hex letters — no ambiguity with &XX color codes */
			unsigned char style = screen_get_style();
			char code = text[i + 1];
			if (code == 'l') { screen_set_style((unsigned char)(style | SCREEN_STYLE_BOLD)); i += 2; continue; }
			if (code == 'L') { screen_set_style((unsigned char)(style & (unsigned char)~SCREEN_STYLE_BOLD)); i += 2; continue; }
			if (code == 'i') { screen_set_style((unsigned char)(style | SCREEN_STYLE_ITALIC)); i += 2; continue; }
			if (code == 'I') { screen_set_style((unsigned char)(style & (unsigned char)~SCREEN_STYLE_ITALIC)); i += 2; continue; }
			if (code == 'u') { screen_set_style((unsigned char)(style | SCREEN_STYLE_UNDERLINE)); i += 2; continue; }
			if (code == 'U') { screen_set_style((unsigned char)(style & (unsigned char)~SCREEN_STYLE_UNDERLINE)); i += 2; continue; }
			if (code == 's') { screen_set_style((unsigned char)(style | SCREEN_STYLE_STRIKE)); i += 2; continue; }
			if (code == 'S') { screen_set_style((unsigned char)(style & (unsigned char)~SCREEN_STYLE_STRIKE)); i += 2; continue; }
			if (code == 'r') { screen_set_color(base_color); screen_set_style(base_style); i += 2; continue; }
			/* Hex color: &XX or §XX (two hex digits, not a style code) */
			if (hex_nibble(text[i + 1]) >= 0 && hex_nibble(text[i + 2]) >= 0)
			{
				unsigned char next_color = (unsigned char)((hex_nibble(text[i + 1]) << 4) | hex_nibble(text[i + 2]));
				screen_set_color(next_color);
				i += 3;
				continue;
			}
		}
		if (text[i] == '\\')
		{
			char esc = text[i + 1];
			if (esc == 'n') { terminal_putc('\n'); i += 2; continue; }
			if (esc == 'r') { terminal_putc('\r'); i += 2; continue; }
			if (esc == 't') { terminal_putc('\t'); i += 2; continue; }
			if (esc == 'e') { terminal_putc(27); i += 2; continue; }
			if (esc == '\\') { terminal_putc('\\'); i += 2; continue; }
		}
		terminal_putc(text[i]);
		i++;
	}

	screen_set_color(base_color);
	screen_set_style(base_style);
}

static unsigned long editor_visible_rows(void)
{
	unsigned long sw = screen_get_width();
	unsigned long start_row;
	unsigned long h = screen_get_height();
	if (sw == 0) sw = 80;
	start_row = (unsigned long)(editor_vga_start / sw);
	if (h == 0) h = 25;
	if (start_row >= h) return 1;
	return h - start_row;
}

static unsigned long editor_text_width(void)
{
	unsigned long sw = screen_get_width();
	if (sw == 0) sw = 80;
	if (sw <= EDITOR_TEXT_GUTTER) return 1;
	return sw - EDITOR_TEXT_GUTTER;
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

static unsigned long editor_move_word_left(unsigned long index)
{
	while (index > 0 && !char_is_word(editor_buffer[index - 1])) index--;
	while (index > 0 && char_is_word(editor_buffer[index - 1])) index--;
	return index;
}

static unsigned long editor_move_word_right(unsigned long index)
{
	while (index < editor_length && char_is_word(editor_buffer[index])) index++;
	while (index < editor_length && !char_is_word(editor_buffer[index])) index++;
	return index;
}

static int editor_has_selection(void)
{
	return editor_selection_active && editor_selection_anchor != editor_cursor;
}

static unsigned long editor_selection_start(void)
{
	return editor_selection_anchor < editor_cursor ? editor_selection_anchor : editor_cursor;
}

static unsigned long editor_selection_end(void)
{
	return editor_selection_anchor < editor_cursor ? editor_cursor : editor_selection_anchor;
}

static void editor_clear_selection(void)
{
	editor_selection_active = 0;
	editor_selection_anchor = editor_cursor;
}

static void editor_begin_selection_if_needed(void)
{
	if (!shift_held) return;
	if (!editor_selection_active)
	{
		editor_selection_active = 1;
		editor_selection_anchor = editor_cursor;
	}
}

static void editor_finish_selection_move(void)
{
	if (!shift_held)
	{
		editor_clear_selection();
		return;
	}
	if (editor_selection_anchor == editor_cursor) editor_selection_active = 0;
}

static void editor_delete_range(unsigned long start, unsigned long end)
{
	unsigned long i;
	if (end <= start || start > editor_length) return;
	if (end > editor_length) end = editor_length;
	for (i = start; i + (end - start) < editor_length; i++) editor_buffer[i] = editor_buffer[i + (end - start)];
	editor_length -= (end - start);
	editor_buffer[editor_length] = '\0';
	editor_cursor = start;
	editor_dirty = 1;
	editor_find_invalidate_match();
	editor_clear_selection();
}

static void editor_delete_selection(void)
{
	if (!editor_has_selection()) return;
	editor_delete_range(editor_selection_start(), editor_selection_end());
}

static void editor_copy_selection(int cut)
{
	unsigned long i;
	unsigned long start;
	unsigned long end;
	if (!editor_has_selection())
	{
		editor_status_line(cut ? "[editor] nothing selected to cut" : "[editor] nothing selected to copy");
		return;
	}
	start = editor_selection_start();
	end = editor_selection_end();
	editor_clipboard_length = end - start;
	for (i = 0; i < editor_clipboard_length; i++) editor_clipboard[i] = editor_buffer[start + i];
	editor_clipboard[editor_clipboard_length] = '\0';
	if (cut)
	{
		editor_delete_range(start, end);
		editor_status_line("[editor] cut");
	}
	else
	{
		editor_status_line("[editor] copied");
	}
}

static int editor_insert_text(const char *text, unsigned long len)
{
	unsigned long i;
	if (text == (void *)0 || len == 0) return 1;
	if (editor_has_selection()) editor_delete_selection();
	if (editor_length + len >= EDITOR_BUFFER_SIZE) return 0;
	for (i = editor_length; i > editor_cursor; i--) editor_buffer[i + len - 1] = editor_buffer[i - 1];
	for (i = 0; i < len; i++) editor_buffer[editor_cursor + i] = text[i];
	editor_cursor += len;
	editor_length += len;
	editor_buffer[editor_length] = '\0';
	editor_dirty = 1;
	editor_find_invalidate_match();
	editor_clear_selection();
	return 1;
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
		unsigned long sw = screen_get_width();
		unsigned long sh = screen_get_height();
		if (sw == 0) sw = 80;
		if (sh == 0) sh = 25;
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
		for (clear_off = editor_vga_start; clear_off < (sw * sh); clear_off++) screen_write_char_at(clear_off, ' ');
		for (row = 0; row < rows; row++)
		{
			unsigned long row_start = editor_view_top + row * EDITOR_HEX_BYTES_PER_ROW;
			unsigned short row_off = (unsigned short)(editor_vga_start + row * sw);
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
		editor_prev_end = (unsigned short)(editor_vga_start + (rows - 1) * sw);
		{
			unsigned long r = (editor_cursor - editor_view_top) / EDITOR_HEX_BYTES_PER_ROW;
			unsigned long c = (editor_cursor - editor_view_top) % EDITOR_HEX_BYTES_PER_ROW;
			unsigned short cursor_off;
			if (r >= rows) r = rows - 1;
			if (c >= EDITOR_HEX_BYTES_PER_ROW) c = EDITOR_HEX_BYTES_PER_ROW - 1;
			cursor_off = (unsigned short)(editor_vga_start + r * sw + EDITOR_HEX_DATA_COL + c * 3 + editor_hex_nibble);
			if (cursor_off >= sw * sh) cursor_off = (unsigned short)(sw * sh - 1);
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
	{
		unsigned long sw = screen_get_width();
		unsigned long sh = screen_get_height();
		if (sw == 0) sw = 80;
		if (sh == 0) sh = 25;
		for (clear_off = editor_vga_start; clear_off < (sw * sh); clear_off++) screen_write_char_at(clear_off, ' ');
	}
	{
		unsigned long row = 0;
		unsigned long row_start = editor_view_top;
		unsigned long width = editor_text_width();
		unsigned long sw = screen_get_width();
		unsigned long sel_start = 0;
		unsigned long sel_end = 0;
		if (sw == 0) sw = 80;
		if (editor_has_selection())
		{
			sel_start = editor_selection_start();
			sel_end = editor_selection_end();
		}
		while (row < rows)
		{
			unsigned short row_off = (unsigned short)(editor_vga_start + row * sw);
			unsigned long row_end = row_start;
			unsigned long col = 0;
			int in_comment = 0;
			int in_string = 0;
			unsigned long token_remaining = 0;
			unsigned long custom_token_remaining = 0;
			unsigned char token_color = editor_text_color;
			unsigned char custom_token_color = editor_text_color;
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
						if (token_is_keyword_ci_table(&editor_buffer[row_end], tlen, editor_sh_keywords, editor_sh_keyword_count))
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
					else if ((row_end == row_start || !char_is_basic_word(editor_buffer[row_end - 1])) && char_is_basic_word(ch))
					{
						unsigned long tlen = 0;
						unsigned long rem_len = 0;
						while (row_end + tlen < editor_length && char_is_basic_word(editor_buffer[row_end + tlen])) tlen++;
						if (starts_with_basic_word_ci(&editor_buffer[row_end], "REM", &rem_len))
						{
							in_comment = 1;
							ch_color = editor_basic_comment_color;
							token_color = editor_basic_comment_color;
							if (rem_len > 0) token_remaining = rem_len - 1;
						}
						else if (token_is_keyword_ci_table(&editor_buffer[row_end], tlen, editor_basic_keywords, editor_basic_keyword_count))
						{
							ch_color = editor_basic_keyword_color;
							token_color = editor_basic_keyword_color;
							if (tlen > 0) token_remaining = tlen - 1;
						}
					}
				}

				if (custom_token_remaining > 0)
				{
					ch_color = custom_token_color;
					custom_token_remaining--;
				}
				else
				{
					unsigned char custom_color;
					unsigned long custom_len;
					if (editor_match_custom_token_at(editor_buffer, editor_length, row_end, &custom_color, &custom_len))
					{
						ch_color = custom_color;
						custom_token_color = custom_color;
						if (custom_len > 0) custom_token_remaining = custom_len - 1;
					}
				}

				if (editor_has_selection() && row_end >= sel_start && row_end < sel_end)
				{
					ch_color = (unsigned char)(0x70 | (ch_color & 0x0F));
				}
				else if (editor_find_match_valid && row_end >= editor_find_match_start && row_end < editor_find_match_end)
				{
					ch_color = (unsigned char)(0x30 | (ch_color & 0x0F));
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
	{
		unsigned long sw = screen_get_width();
		unsigned long sh = screen_get_height();
		if (sw == 0) sw = 80;
		if (sh == 0) sh = 25;
		editor_prev_end = (unsigned short)(editor_vga_start + (rows - 1) * sw);
	{
		unsigned long cursor_index = editor_cursor;
		unsigned long r;
		unsigned long c;
		unsigned short cursor_off;
		if (cursor_index > 0 && cursor_index <= editor_length)
		{
			unsigned long row_start = editor_visual_row_start(cursor_index);
			if (row_start == cursor_index && editor_buffer[cursor_index - 1] != '\n') cursor_index--;
		}
		r = editor_cursor_row_from_top(editor_view_top, cursor_index);
		c = editor_visual_row_col(cursor_index);
		if (cursor_index != editor_cursor) c++;
		if (r >= rows) r = rows - 1;
		if (c >= editor_text_width()) c = editor_text_width() - 1;
		cursor_off = (unsigned short)(editor_vga_start + r * sw + EDITOR_TEXT_GUTTER + c);
		if (cursor_off >= sw * sh) cursor_off = (unsigned short)(sw * sh - 1);
		screen_set_hw_cursor(cursor_off);
		screen_set_pos(cursor_off);
	}
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
			unsigned char data[FS_MAX_FILE_SIZE];
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

static int terminal_has_selection(void)
{
	return terminal_selection_active && terminal_selection_anchor != cursor_pos;
}

static unsigned long terminal_selection_start(void)
{
	return terminal_selection_anchor < cursor_pos ? terminal_selection_anchor : cursor_pos;
}

static unsigned long terminal_selection_end(void)
{
	return terminal_selection_anchor < cursor_pos ? cursor_pos : terminal_selection_anchor;
}

static void terminal_clear_selection(void)
{
	terminal_selection_active = 0;
	terminal_selection_anchor = cursor_pos;
}

static void terminal_begin_selection_if_needed(void)
{
	if (!shift_held) return;
	if (!terminal_selection_active)
	{
		terminal_selection_active = 1;
		terminal_selection_anchor = cursor_pos;
	}
}

static void terminal_finish_selection_move(void)
{
	if (!shift_held)
	{
		terminal_clear_selection();
		return;
	}
	if (terminal_selection_anchor == cursor_pos) terminal_selection_active = 0;
}

static void terminal_redraw_input_line(void)
{
	unsigned long i;
	unsigned long sel_start = 0;
	unsigned long sel_end = 0;
	unsigned long clear_to = terminal_last_drawn_length > input_length ? terminal_last_drawn_length : input_length;
	if (clear_to < input_length + 1) clear_to = input_length + 1;
	terminal_draw_prompt_prefix();
	if (terminal_has_selection())
	{
		sel_start = terminal_selection_start();
		sel_end = terminal_selection_end();
	}
	for (i = 0; i < clear_to && i < INPUT_BUFFER_SIZE - 1; i++)
	{
		if (i < input_length)
		{
			unsigned char color = terminal_text_color;
			if (terminal_has_selection() && i >= sel_start && i < sel_end) color = (unsigned char)(0x70 | (color & 0x0F));
			screen_set_color(color);
			screen_write_char_at((unsigned short)(prompt_vga_start + i), input_buffer[i]);
		}
		else
		{
			screen_set_color(terminal_text_color);
			screen_write_char_at((unsigned short)(prompt_vga_start + i), ' ');
		}
	}
	terminal_last_drawn_length = input_length;
	sync_screen_pos();
	screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
	screen_set_color(terminal_text_color);
	terminal_selection_anchor = terminal_selection_active ? terminal_selection_anchor : cursor_pos;
}

static void terminal_delete_range(unsigned long start, unsigned long end)
{
	unsigned long i;
	if (end <= start || start > input_length) return;
	if (end > input_length) end = input_length;
	for (i = start; i + (end - start) < input_length; i++) input_buffer[i] = input_buffer[i + (end - start)];
	input_length -= (end - start);
	input_buffer[input_length] = '\0';
	cursor_pos = start;
	terminal_clear_selection();
	terminal_redraw_input_line();
}

static void terminal_delete_selection(void)
{
	if (!terminal_has_selection()) return;
	terminal_delete_range(terminal_selection_start(), terminal_selection_end());
}

static void terminal_copy_selection(int cut)
{
	unsigned long i;
	unsigned long start;
	unsigned long end;
	if (!terminal_has_selection()) return;
	start = terminal_selection_start();
	end = terminal_selection_end();
	editor_clipboard_length = end - start;
	for (i = 0; i < editor_clipboard_length; i++) editor_clipboard[i] = input_buffer[start + i];
	editor_clipboard[editor_clipboard_length] = '\0';
	if (cut) terminal_delete_range(start, end);
}

static int terminal_insert_text(const char *text, unsigned long len)
{
	unsigned long i;
	if (text == (void *)0 || len == 0) return 1;
	if (terminal_has_selection()) terminal_delete_selection();
	if (input_length + len >= INPUT_BUFFER_SIZE) return 0;
	for (i = input_length; i > cursor_pos; i--) input_buffer[i + len - 1] = input_buffer[i - 1];
	for (i = 0; i < len; i++) input_buffer[cursor_pos + i] = text[i];
	input_length += len;
	cursor_pos += len;
	input_buffer[input_length] = '\0';
	terminal_clear_selection();
	terminal_redraw_input_line();
	return 1;
}

static void clear_input_line(void)
{
	unsigned long i;
	unsigned long clear_to = terminal_last_drawn_length > input_length ? terminal_last_drawn_length : input_length;
	for (i = 0; i < clear_to; i++)
		screen_write_char_at((unsigned short)(prompt_vga_start + i), ' ');
	input_length = 0;
	cursor_pos   = 0;
	input_buffer[0] = '\0';
	terminal_last_drawn_length = 0;
	terminal_clear_selection();
	screen_set_pos(prompt_vga_start);
	screen_set_hw_cursor(prompt_vga_start);
}

static void draw_from_buffer(void)
{
	cursor_pos = input_length;
	terminal_clear_selection();
	terminal_redraw_input_line();
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
	"help", "man", "alias", "unalias", "version", "echo", "glyph", "charmap", "color", "serial", "display", "themes", "theme", "ethemes", "etheme", "clear", "reboot",
	"panic",
	"quit", "exit", "shutdown",
	"pwd", "ls", "dir", "tree", "cd", "mkdir", "touch", "write", "cat", "type", "rm", "del", "cp", "copy", "mv", "move", "ren", "edit", "hexedit", "run", "basic", "cls",
	"hexdump", "memmap", "memstat", "pagetest", "pagefault", "gpfault", "udfault", "doublefault", "exceptstat", "dumpstack", "selftest", "elfinfo", "elfsym", "elfaddr", "exec", "execstress", "elfselftest", "tasks", "tasktest", "taskspin", "shellspawn", "shellwatch", "taskprotect", "tasklog", "taskkill", "taskstop", "taskcont", "ticks", "motd", "autorun", "ataid", "readsec", "writesec", "drives", "fatmount", "ramfs", "ramfs2fat", "fatunmount", "fatls", "fatcat", "fattouch", "fatwrite", "fatattr", "fatrm", (void *)0
};

static int parse_dec_u32(const char *s, unsigned int *out)
{
	unsigned long v = 0;
	unsigned long i = 0;
	if (s == (void *)0 || s[0] == '\0' || out == (void *)0) return -1;
	while (s[i] != '\0')
	{
		if (s[i] < '0' || s[i] > '9') return -1;
		v = v * 10 + (unsigned long)(s[i] - '0');
		if (v > 0xFFFFFFFFUL) return -1;
		i++;
	}
	*out = (unsigned int)v;
	return 0;
}

static int read_shell_file_bytes(const char *op, const char *path, unsigned char *buf, unsigned long capacity, unsigned long *out_size)
{
	char resolved[128];

	if (path == (void *)0 || path[0] == '\0' || buf == (void *)0 || out_size == (void *)0) return -1;
	if (fat_mode_active())
	{
		if (fat_resolve_path(path, resolved, sizeof(resolved)) != 0)
		{
			terminal_write(op);
			terminal_write_line(": invalid FAT path");
			return -1;
		}
		if (fat32_read_file_path(resolved, buf, capacity, out_size) != 0)
		{
			terminal_write(op);
			terminal_write_line(": read failed");
			return -1;
		}
	}
	else
	{
		if (fs_read_file(path, buf, capacity, out_size) != 0)
		{
			terminal_write(op);
			terminal_write_line(": read failed");
			return -1;
		}
	}
	return 0;
}

static const char *elf_type_name(unsigned short type)
{
	switch (type)
	{
		case 1: return "REL";
		case 2: return "EXEC";
		case 3: return "DYN";
		case 4: return "CORE";
		default: return "OTHER";
	}
}

static const char *elf_symbol_type_name(unsigned char info)
{
	switch (info & 0x0Fu)
	{
		case 0: return "NOTYPE";
		case 1: return "OBJECT";
		case 2: return "FUNC";
		case 3: return "SECTION";
		case 4: return "FILE";
		default: return "OTHER";
	}
}

static const char *elf_symbol_bind_name(unsigned char info)
{
	switch ((unsigned char)(info >> 4))
	{
		case 0: return "LOCAL";
		case 1: return "GLOBAL";
		case 2: return "WEAK";
		default: return "OTHER";
	}
}

static const char *elf_phdr_type_name(unsigned int type)
{
	switch (type)
	{
		case 0: return "NULL";
		case 1: return "LOAD";
		case 2: return "DYNAMIC";
		case 3: return "INTERP";
		case 4: return "NOTE";
		case 5: return "SHLIB";
		case 6: return "PHDR";
		case 7: return "TLS";
		default: return "OTHER";
	}
}

static const char *elf_section_type_name(unsigned int type)
{
	switch (type)
	{
		case 0: return "NULL";
		case 1: return "PROGBITS";
		case 2: return "SYMTAB";
		case 3: return "STRTAB";
		case 4: return "RELA";
		case 5: return "HASH";
		case 6: return "DYNAMIC";
		case 7: return "NOTE";
		case 8: return "NOBITS";
		case 9: return "REL";
		case 11: return "DYNSYM";
		default: return "OTHER";
	}
}

struct elf_symbol_print_ctx
{
	const char *filter;
	unsigned long total;
	unsigned long matched;
};

static int print_elf_symbol(const elf_symbol_t *sym, void *ctx)
{
	struct elf_symbol_print_ctx *print_ctx = (struct elf_symbol_print_ctx *)ctx;
	char size_buf[24];

	print_ctx->total++;
	if (print_ctx->filter != (void *)0 && print_ctx->filter[0] != '\0' && !string_contains_ci(sym->name, print_ctx->filter))
		return 0;

	print_ctx->matched++;
	terminal_write("  ");
	terminal_write(elf_symbol_bind_name(sym->info));
	terminal_write(" ");
	terminal_write(elf_symbol_type_name(sym->info));
	terminal_write(" ");
	terminal_write_hex64(sym->value);
	terminal_write(" size=");
	uint_to_dec(sym->size, size_buf, sizeof(size_buf));
	terminal_write(size_buf);
	terminal_write(" ");
	terminal_write_line(sym->name);
	return 0;
}

struct elf_phdr_print_ctx
{
	unsigned long count;
};

struct elf_section_print_ctx
{
	unsigned long count;
};

static int print_elf_phdr(const elf_program_header_info_t *ph, void *ctx)
{
	struct elf_phdr_print_ctx *print_ctx = (struct elf_phdr_print_ctx *)ctx;
	char n[16];
	char flags[4];

	flags[0] = (ph->flags & 0x4u) ? 'R' : '-';
	flags[1] = (ph->flags & 0x2u) ? 'W' : '-';
	flags[2] = (ph->flags & 0x1u) ? 'X' : '-';
	flags[3] = '\0';

	print_ctx->count++;
	terminal_write("    [");
	uint_to_dec(ph->index, n, sizeof(n));
	terminal_write(n);
	terminal_write("] ");
	terminal_write(elf_phdr_type_name(ph->type));
	terminal_write(" flags=");
	terminal_write(flags);
	terminal_write(" vaddr=");
	terminal_write_hex64(ph->vaddr);
	terminal_write(" off=");
	terminal_write_hex64(ph->offset);
	terminal_write(" filesz=");
	uint_to_dec(ph->filesz, n, sizeof(n));
	terminal_write(n);
	terminal_write(" memsz=");
	uint_to_dec(ph->memsz, n, sizeof(n));
	terminal_write_line(n);
	return 0;
}

static int print_elf_section(const elf_section_info_t *section, void *ctx)
{
	struct elf_section_print_ctx *print_ctx = (struct elf_section_print_ctx *)ctx;
	char n[16];

	print_ctx->count++;
	terminal_write("    [");
	uint_to_dec(section->index, n, sizeof(n));
	terminal_write(n);
	terminal_write("] ");
	if (section->name != (void *)0 && section->name[0] != '\0') terminal_write(section->name);
	else terminal_write("<noname>");
	terminal_write(" type=");
	terminal_write(elf_section_type_name(section->type));
	terminal_write(" addr=");
	terminal_write_hex64(section->addr);
	terminal_write(" off=");
	terminal_write_hex64(section->offset);
	terminal_write(" size=");
	uint_to_dec(section->size, n, sizeof(n));
	terminal_write(n);
	terminal_write(" flags=");
	terminal_write_hex64(section->flags);
	terminal_putc('\n');
	return 0;
}

static void print_help_commands(const char *args)
{
	unsigned int page = 1;
	unsigned int total = 0;
	unsigned int builtins = 0;
	unsigned int pages;
	unsigned int per_page = 12;
	unsigned int start;
	unsigned int end;
	unsigned int i;
	char tok[16];
	char num[16];
	const char *p = read_token(args, tok, sizeof(tok));

	if (p != (void *)0 && tok[0] != '\0')
	{
		if (parse_dec_u32(tok, &page) != 0 || page == 0)
		{
			terminal_write_line("Usage: help commands [page]");
			return;
		}
	}

	while (cmd_list[builtins] != (void *)0) builtins++;
	total = builtins + (unsigned int)command_alias_count;
	pages = (total + per_page - 1) / per_page;
	if (pages == 0) pages = 1;
	if (page > pages) page = pages;

	start = (page - 1) * per_page;
	end = start + per_page;
	if (end > total) end = total;

	terminal_write("Commands page ");
	uint_to_dec((unsigned long)page, num, sizeof(num));
	terminal_write(num);
	terminal_write("/");
	uint_to_dec((unsigned long)pages, num, sizeof(num));
	terminal_write_line(num);

	for (i = start; i < end; i++)
	{
		terminal_write("  ");
		if (i < builtins)
		{
			terminal_write_line(cmd_list[i]);
		}
		else
		{
			unsigned int alias_index = i - builtins;
			terminal_write(command_alias_names[alias_index]);
			terminal_write(" -> ");
			terminal_write_line(command_alias_expansions[alias_index]);
		}
	}

	terminal_write_line("Use: help commands <page>");
}

static int command_is_builtin_name(const char *name)
{
	int i;
	for (i = 0; cmd_list[i] != (void *)0; i++)
	{
		if (string_equals(cmd_list[i], name)) return 1;
	}
	return 0;
}

static void cmd_alias(const char *args)
{
	char tok[COMMAND_ALIAS_NAME_LEN + COMMAND_ALIAS_EXPANSION_LEN + 2];
	char name[COMMAND_ALIAS_NAME_LEN];
	char expansion[COMMAND_ALIAS_EXPANSION_LEN];
	const char *p;
	const char *rest;
	unsigned long i;
	int index;

	p = read_token(args, tok, sizeof(tok));
	if (p == (void *)0 || tok[0] == '\0')
	{
		if (command_alias_count == 0)
		{
			terminal_write_line("alias: no aliases defined");
			return;
		}
		for (i = 0; i < (unsigned long)command_alias_count; i++)
		{
			terminal_write("alias ");
			terminal_write(command_alias_names[i]);
			terminal_write("=");
			terminal_write_line(command_alias_expansions[i]);
		}
		return;
	}

	for (i = 0; tok[i] != '\0' && tok[i] != '=' && i + 1 < sizeof(name); i++) name[i] = tok[i];
	name[i] = '\0';
	if (name[0] == '\0' || !token_is_word_only(name))
	{
		terminal_write_line("alias: invalid name");
		return;
	}
	if (command_is_builtin_name(name))
	{
		terminal_write_line("alias: name collides with a built-in command");
		return;
	}

	expansion[0] = '\0';
	if (tok[i] == '=')
	{
		unsigned long j = 0;
		i++;
		while (tok[i] != '\0' && j + 1 < sizeof(expansion)) expansion[j++] = tok[i++];
		rest = skip_spaces(p);
		if (rest[0] != '\0' && j + 1 < sizeof(expansion))
		{
			if (j > 0) expansion[j++] = ' ';
			for (i = 0; rest[i] != '\0' && j + 1 < sizeof(expansion); i++) expansion[j++] = rest[i];
		}
		expansion[j] = '\0';
	}
	else
	{
		rest = skip_spaces(p);
		if (rest[0] == '\0')
		{
			index = command_alias_index(name);
			if (index < 0)
			{
				terminal_write_line("alias: not found");
				return;
			}
			terminal_write("alias ");
			terminal_write(command_alias_names[index]);
			terminal_write("=");
			terminal_write_line(command_alias_expansions[index]);
			return;
		}
		for (i = 0; rest[i] != '\0' && i + 1 < sizeof(expansion); i++) expansion[i] = rest[i];
		expansion[i] = '\0';
	}

	if (expansion[0] == '\0')
	{
		terminal_write_line("Usage: alias <name> <command...>");
		terminal_write_line("Usage: alias <name>=<command...>");
		return;
	}

	index = command_alias_index(name);
	if (index < 0)
	{
		if (command_alias_count >= COMMAND_ALIAS_MAX)
		{
			terminal_write_line("alias: alias table full");
			return;
		}
		index = command_alias_count++;
	}
	for (i = 0; i + 1 < COMMAND_ALIAS_NAME_LEN && name[i] != '\0'; i++) command_alias_names[index][i] = name[i];
	command_alias_names[index][i] = '\0';
	for (i = 0; i + 1 < COMMAND_ALIAS_EXPANSION_LEN && expansion[i] != '\0'; i++) command_alias_expansions[index][i] = expansion[i];
	command_alias_expansions[index][i] = '\0';
	terminal_write("alias: ");
	terminal_write(name);
	terminal_write(" -> ");
	terminal_write_line(expansion);
}

static void cmd_unalias(const char *args)
{
	char name[COMMAND_ALIAS_NAME_LEN];
	int index;
	int i;
	if (read_token(args, name, sizeof(name)) == (void *)0 || name[0] == '\0')
	{
		terminal_write_line("Usage: unalias <name>");
		return;
	}
	index = command_alias_index(name);
	if (index < 0)
	{
		terminal_write_line("unalias: not found");
		return;
	}
	for (i = index; i + 1 < command_alias_count; i++)
	{
		unsigned long j;
		for (j = 0; j < COMMAND_ALIAS_NAME_LEN; j++) command_alias_names[i][j] = command_alias_names[i + 1][j];
		for (j = 0; j < COMMAND_ALIAS_EXPANSION_LEN; j++) command_alias_expansions[i][j] = command_alias_expansions[i + 1][j];
	}
	command_alias_count--;
	terminal_write("unalias: removed ");
	terminal_write_line(name);
}

static int resolve_command_aliases(const char *in, char *out, unsigned long out_size)
{
	char current[INPUT_BUFFER_SIZE];
	char next[INPUT_BUFFER_SIZE];
	char name[COMMAND_ALIAS_NAME_LEN];
	const char *p;
	const char *alias_expansion;
	const char *rest;
	unsigned long i;
	unsigned long n;
	int depth;

	if (in == (void *)0 || out == (void *)0 || out_size == 0) return -1;
	for (i = 0; in[i] != '\0' && i + 1 < sizeof(current); i++) current[i] = in[i];
	current[i] = '\0';

	for (depth = 0; depth < COMMAND_ALIAS_MAX; depth++)
	{
		p = read_token(current, name, sizeof(name));
		if (p == (void *)0) return -1;
		if (name[0] == '\0') break;
		alias_expansion = command_alias_lookup(name);
		if (alias_expansion == (void *)0) break;
		rest = skip_spaces(p);
		n = 0;
		for (i = 0; alias_expansion[i] != '\0' && n + 1 < sizeof(next); i++) next[n++] = alias_expansion[i];
		if (rest[0] != '\0' && n + 1 < sizeof(next)) next[n++] = ' ';
		for (i = 0; rest[i] != '\0' && n + 1 < sizeof(next); i++) next[n++] = rest[i];
		next[n] = '\0';
		for (i = 0; next[i] != '\0' && i + 1 < sizeof(current); i++) current[i] = next[i];
		current[i] = '\0';
	}
	if (depth == COMMAND_ALIAS_MAX)
	{
		terminal_write_line("alias: expansion loop too deep");
		return -1;
	}
	for (i = 0; current[i] != '\0' && i + 1 < out_size; i++) out[i] = current[i];
	out[i] = '\0';
	return 0;
}

static void handle_tab(void)
{
	int i, match_count = 0;
	int alias_index;
	unsigned long j;
	const char *last = (void *)0;

	for (i = 0; cmd_list[i]; i++)
		if (string_starts_with(cmd_list[i], input_buffer)) { match_count++; last = cmd_list[i]; }
	for (alias_index = 0; alias_index < command_alias_count; alias_index++)
		if (string_starts_with(command_alias_names[alias_index], input_buffer)) { match_count++; last = command_alias_names[alias_index]; }

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
		for (alias_index = 0; alias_index < command_alias_count; alias_index++)
			if (string_starts_with(command_alias_names[alias_index], input_buffer)) { terminal_write(command_alias_names[alias_index]); terminal_putc(' '); }
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

static void handle_backspace(int from_serial)
{
	unsigned long i;
	if (terminal_has_selection())
	{
		terminal_delete_selection();
		if (!from_serial && serial_ready && serial_mirror_enabled && !serial_compact_enabled) serial_write("\b \b");
		return;
	}
	if (cursor_pos == 0) return;
	cursor_pos--;
	for (i = cursor_pos; i < input_length - 1; i++) input_buffer[i] = input_buffer[i + 1];
	input_length--;
	input_buffer[input_length] = '\0';
	terminal_clear_selection();
	for (i = cursor_pos; i < input_length; i++)
		screen_write_char_at((unsigned short)(prompt_vga_start + i), input_buffer[i]);
	screen_write_char_at((unsigned short)(prompt_vga_start + input_length), ' ');
	sync_screen_pos();
	screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
	if (!from_serial && serial_ready && serial_mirror_enabled && !serial_compact_enabled) serial_write("\b \b");
}

static void terminal_request_cancel(void)
{
	terminal_cancel_requested = 1;
}

static int terminal_take_cancel_request(void)
{
	if (!terminal_cancel_requested) return 0;
	terminal_cancel_requested = 0;
	return 1;
}

/*
 * Poll only emergency controls while a command loop is running.
 * This intentionally avoids full line-editing/command submission,
 * preventing recursive run_command() calls.
 */
static void terminal_poll_control_hotkeys(void)
{
	while (scancode_queue_tail != scancode_queue_head)
	{
		unsigned char sc = scancode_queue[scancode_queue_tail];
		scancode_queue_tail = (scancode_queue_tail + 1) % SCANCODE_QUEUE_SIZE;

		if (sc == 0xE0) { control_poll_extended = 1; continue; }
		if (control_poll_extended)
		{
			control_poll_extended = 0;
			if (sc == 0x1D) { ctrl_held = 1; continue; }
			if (sc == 0x9D) { ctrl_held = 0; continue; }
			if (sc == 0x38) { alt_held = 1; continue; }
			if (sc == 0xB8) { alt_held = 0; continue; }
			if (sc == 0x13 && ctrl_held && shift_held) { do_reboot(); return; } /* Ctrl+Shift+R */
			continue;
		}

		if (sc == 0x1D) { ctrl_held = 1; continue; }
		if (sc == 0x9D) { ctrl_held = 0; continue; }
		if (sc == 0x38) { alt_held = 1; continue; }
		if (sc == 0xB8) { alt_held = 0; continue; }

		if (sc == 0x2E && ctrl_held)
		{
			terminal_request_cancel();
			terminal_write_line("^C");
			continue;
		}
		if (sc == 0x10 && ctrl_held && shift_held)
		{
			terminal_shutdown();
			return;
		}
	}
}

static void terminal_abort_input_line(void)
{
	input_length = 0;
	cursor_pos = 0;
	input_buffer[0] = '\0';
	terminal_clear_selection();
	sync_screen_pos();
	terminal_write_line("^C");
	terminal_prompt();
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

static void trigger_forced_panic(void)
{
	/* Intentional page fault for panic-path testing. */
	*((volatile unsigned long *)0) = 0xDEADBEEFUL;
	for (;;) arch_halt();
}

static unsigned long pagefault_test_addr(void)
{
	unsigned long addr = memory_virtual_limit() + MEMORY_PAGE_SIZE;
	unsigned int i;

	for (i = 0; i < 64; i++)
	{
		if (paging_get_phys(addr) == 0) return addr;
		addr += MEMORY_PAGE_SIZE;
	}

	return memory_virtual_limit() + (128UL * MEMORY_PAGE_SIZE);
}

static void cmd_pagefault(const char *args)
{
	char mode[16];
	const char *p = read_token(args, mode, sizeof(mode));
	unsigned long fault_addr;

	if (p == (void *)0 || mode[0] == '\0' || skip_spaces(p)[0] != '\0')
	{
		terminal_write_line("Usage: pagefault <read|write|exec>");
		return;
	}

	terminal_write("[pagefault] mode: ");
	terminal_write_line(mode);
	fault_addr = pagefault_test_addr();
	terminal_write("[pagefault] addr: ");
	terminal_write_hex64(fault_addr);
	terminal_putc('\n');

	if (string_equals(mode, "read"))
	{
		volatile unsigned long value = *((volatile unsigned long *)fault_addr);
		(void)value;
	}
	else if (string_equals(mode, "write"))
	{
		*((volatile unsigned long *)fault_addr) = 0x5046554CUL;
	}
	else if (string_equals(mode, "exec"))
	{
		typedef void (*pf_exec_t)(void);
		pf_exec_t fn = (pf_exec_t)fault_addr;
		fn();
	}
	else
	{
		terminal_write_line("Usage: pagefault <read|write|exec>");
		return;
	}

	for (;;) arch_halt();
}

static void cmd_gpfault(const char *args)
{
	if (skip_spaces(args)[0] != '\0')
	{
		terminal_write_line("Usage: gpfault");
		return;
	}

	terminal_write_line("[gpfault] using non-canonical address 0x0000800000000000");
	{
		volatile unsigned long value = *((volatile unsigned long *)0x0000800000000000UL);
		(void)value;
	}

	for (;;) arch_halt();
}

static void cmd_udfault(const char *args)
{
	if (skip_spaces(args)[0] != '\0')
	{
		terminal_write_line("Usage: udfault");
		return;
	}

	terminal_write_line("[udfault] executing UD2");
	__asm__ volatile("ud2");
	for (;;) arch_halt();
}

static void cmd_doublefault(const char *args)
{
	struct exception_frame frame;
	unsigned long current_rsp;

	if (skip_spaces(args)[0] != '\0')
	{
		terminal_write_line("Usage: doublefault");
		return;
	}

	terminal_write_line("[doublefault] simulating double fault recovery");

	/* Get current RSP using inline assembly */
	current_rsp = 0;
	__asm__ volatile("mov %%rsp, %0" : "=r"(current_rsp));

	/* Build a frame representing a double fault */
	frame.rax = 0;
	frame.rbx = 0;
	frame.rcx = 0;
	frame.rdx = 0;
	frame.rsi = 0;
	frame.rdi = 0;
	frame.rbp = 0;
	frame.r8 = 0;
	frame.r9 = 0;
	frame.r10 = 0;
	frame.r11 = 0;
	frame.r12 = 0;
	frame.r13 = 0;
	frame.r14 = 0;
	frame.r15 = 0;
	frame.vector = 8;         /* Vector 8 = Double Fault */
	frame.error_code = 0;
	frame.rip = (unsigned long)cmd_doublefault;  /* Use function address */
	frame.cs = 0x08;
	frame.rflags = 0x202;
	frame.rsp = current_rsp;
	frame.ss = 0x10;

	/* Call the double fault handler - this will display the panic screen */
	double_fault_handler(&frame);

	for (;;) arch_halt();
}

static void cmd_exceptstat(const char *args)
{
	const char *names[] = {
		"#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
		"#DF", "CSO", "#TS", "#NP", "#SS", "#GP", "#PF", "Res",
		"#MF", "#AC", "#MC", "#XF", "#VE", "#CP", "Res", "Res",
		"Res", "Res", "Res", "Res", "Res", "Res", "#SX", "Res"
	};
	int i;
	unsigned long total = 0;

	if (skip_spaces(args)[0] != '\0')
	{
		terminal_write_line("Usage: exceptstat");
		return;
	}

	terminal_write_line("Exception Statistics (since boot):");
	terminal_write_line("  Vec  Exception  Count");
	terminal_write_line("  --- ---------- -------");

	for (i = 0; i < 32; i++)
	{
		unsigned long count = idt_get_exception_count((unsigned char)i);
		if (count > 0)
		{
			char vec_str[4];
			char count_str[16];
			unsigned long temp;

			/* Format vector number */
			vec_str[0] = ' ';
			if (i < 10)
			{
				vec_str[1] = '0' + i;
				vec_str[2] = ' ';
				vec_str[3] = '\0';
			}
			else
			{
				vec_str[1] = '0' + (i / 10);
				vec_str[2] = '0' + (i % 10);
				vec_str[3] = '\0';
			}

			/* Format count */
			temp = count;
			int digits = 0;
			while (temp > 0) { digits++; temp /= 10; }
			if (digits == 0) digits = 1;
			for (int j = digits - 1; j >= 0; j--)
			{
				count_str[j] = '0' + (count % 10);
				count /= 10;
			}
			count_str[digits] = '\0';

			terminal_write(vec_str);
			terminal_write("  ");
			terminal_write(names[i]);
			terminal_write(count < 100 ? "        " : count < 1000 ? "       " : "      ");
			terminal_write(count_str);
			terminal_write("\n");
			total += count;
		}
	}

	terminal_write_line("  --- ---------- -------");
	char total_str[16];
	unsigned long temp = total;
	int digits = 0;
	while (temp > 0) { digits++; temp /= 10; }
	if (digits == 0) digits = 1;
	for (int j = digits - 1; j >= 0; j--)
	{
		total_str[j] = '0' + (total % 10);
		total /= 10;
	}
	total_str[digits] = '\0';
	terminal_write("  TOTAL COUNT ");
	terminal_write(total < 1000000 ? "      " : "     ");
	terminal_write(total_str);
	terminal_write("\n");
}

static void cmd_dumpstack(const char *args)
{
	unsigned long current_rbp = 0;

	if (skip_spaces(args)[0] != '\0')
	{
		terminal_write_line("Usage: dumpstack");
		return;
	}

	/* Get current RBP */
	__asm__ volatile("mov %%rbp, %0" : "=r"(current_rbp));

	terminal_write_line("=== Kernel Stack Dump ===");
	terminal_write_line("Walking RBP chain from entry point:");
	idt_display_backtrace(current_rbp);
	terminal_write_line("=== End Stack Dump ===");
}

static void cmd_selftest(const char *args)
{
	char suite[16];
	char step[16];
	const char *p = read_token(args, suite, sizeof(suite));

	if (p == (void *)0 || suite[0] == '\0')
	{
		terminal_write_line("Usage: selftest exceptions [pf-read|pf-write|pf-exec|ud|gp|1|2|3|4|5]");
		return;
	}
	if (!string_equals(suite, "exceptions"))
	{
		terminal_write_line("selftest: only 'exceptions' suite is available right now");
		terminal_write_line("Usage: selftest exceptions [pf-read|pf-write|pf-exec|ud|gp|1|2|3|4|5]");
		return;
	}

	p = read_token(p, step, sizeof(step));
	if (p == (void *)0)
	{
		terminal_write_line("Usage: selftest exceptions [pf-read|pf-write|pf-exec|ud|gp|1|2|3|4|5]");
		return;
	}

	if (step[0] == '\0')
	{
		terminal_write_line("selftest exceptions: reboot-friendly plan");
		terminal_write_line("  1. selftest exceptions pf-read   -> expect #PF vec=0x0E, P=0 W=0 I=0");
		terminal_write_line("  2. selftest exceptions pf-write  -> expect #PF vec=0x0E, P=0 W=1 I=0");
		terminal_write_line("  3. selftest exceptions pf-exec   -> expect #PF vec=0x0E, P=0 W=0 I=1");
		terminal_write_line("  4. selftest exceptions ud        -> expect #UD vec=0x06");
		terminal_write_line("  5. selftest exceptions gp        -> expect #GP vec=0x0D");
		terminal_write_line("Run one step at a time; each step intentionally panics.");
		return;
	}
	if (skip_spaces(p)[0] != '\0')
	{
		terminal_write_line("Usage: selftest exceptions [pf-read|pf-write|pf-exec|ud|gp|1|2|3|4|5]");
		return;
	}

	if (string_equals(step, "pf-read") || string_equals(step, "1"))
	{
		terminal_write_line("[selftest] step 1/5: #PF read expected (vec=0x0E, P=0 W=0 I=0)");
		cmd_pagefault("read");
		return;
	}
	if (string_equals(step, "pf-write") || string_equals(step, "2"))
	{
		terminal_write_line("[selftest] step 2/5: #PF write expected (vec=0x0E, P=0 W=1 I=0)");
		cmd_pagefault("write");
		return;
	}
	if (string_equals(step, "pf-exec") || string_equals(step, "3"))
	{
		terminal_write_line("[selftest] step 3/5: #PF exec expected (vec=0x0E, P=0 W=0 I=1)");
		cmd_pagefault("exec");
		return;
	}
	if (string_equals(step, "ud") || string_equals(step, "4"))
	{
		terminal_write_line("[selftest] step 4/5: #UD expected (vec=0x06)");
		cmd_udfault("");
		return;
	}
	if (string_equals(step, "gp") || string_equals(step, "5"))
	{
		terminal_write_line("[selftest] step 5/5: #GP expected (vec=0x0D)");
		cmd_gpfault("");
		return;
	}

	terminal_write_line("Usage: selftest exceptions [pf-read|pf-write|pf-exec|ud|gp|1|2|3|4|5]");
}

static void update_panic_hotkey(void)
{
	if (panic_esc_held && panic_f12_held)
	{
		if (!panic_hotkey_fired)
		{
			panic_hotkey_fired = 1;
			terminal_write_line("[SYSTEM] Esc+F12 detected");
			trigger_forced_panic();
		}
		return;
	}
	panic_hotkey_fired = 0;
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

static void cmd_memstat(void)
{
	char num[32];
	unsigned long total_pages = memory_total_pages();
	unsigned long free_pages = memory_free_pages();
	unsigned long used_pages = (total_pages >= free_pages) ? (total_pages - free_pages) : 0;
	unsigned long total_kib = total_pages * (MEMORY_PAGE_SIZE / 1024UL);
	unsigned long free_kib = free_pages * (MEMORY_PAGE_SIZE / 1024UL);
	unsigned long used_kib = used_pages * (MEMORY_PAGE_SIZE / 1024UL);

	terminal_write_line("Memory status:");
	terminal_write("  total pages: ");
	uint_to_dec(total_pages, num, sizeof(num));
	terminal_write_line(num);
	terminal_write("  free pages:  ");
	uint_to_dec(free_pages, num, sizeof(num));
	terminal_write_line(num);
	terminal_write("  used pages:  ");
	uint_to_dec(used_pages, num, sizeof(num));
	terminal_write_line(num);
	terminal_write("  total KiB:   ");
	uint_to_dec(total_kib, num, sizeof(num));
	terminal_write_line(num);
	terminal_write("  free KiB:    ");
	uint_to_dec(free_kib, num, sizeof(num));
	terminal_write_line(num);
	terminal_write("  used KiB:    ");
	uint_to_dec(used_kib, num, sizeof(num));
	terminal_write_line(num);

	terminal_write("  virt base:   ");
	terminal_write_hex64(memory_virtual_base());
	terminal_putc('\n');
	terminal_write("  virt limit:  ");
	terminal_write_hex64(memory_virtual_limit());
	terminal_putc('\n');
	terminal_write("  cr3:         ");
	terminal_write_hex64(arch_read_cr3());
	terminal_putc('\n');
}

static void cmd_pagetest(void)
{
	void *ptr;
	unsigned char *p;
	unsigned long phys;
	unsigned int i;
	unsigned int errors = 0;
	char num[32];

	terminal_write_line("pagetest: allocating 2 pages via virt_alloc_pages...");
	ptr = virt_alloc_pages(2);
	if (ptr == (void *)0)
	{
		terminal_write_line("pagetest: FAIL - virt_alloc_pages returned null");
		return;
	}
	terminal_write("  virtual addr: ");
	terminal_write_hex64((unsigned long)ptr);
	terminal_putc('\n');

	phys = paging_get_phys((unsigned long)ptr);
	terminal_write("  physical addr: ");
	terminal_write_hex64(phys);
	terminal_putc('\n');

	if (phys == 0)
	{
		terminal_write_line("pagetest: FAIL - paging_get_phys returned 0");
		virt_free_pages(ptr, 2);
		return;
	}

	/* Write pattern */
	p = (unsigned char *)ptr;
	for (i = 0; i < 8192; i++) p[i] = (unsigned char)(i & 0xFF);

	/* Verify pattern */
	for (i = 0; i < 8192; i++)
	{
		if (p[i] != (unsigned char)(i & 0xFF)) errors++;
	}

	if (errors == 0)
	{
		terminal_write_line("  write/read: OK");
	}
	else
	{
		terminal_write("  write/read: FAIL - ");
		uint_to_dec((unsigned long)errors, num, sizeof(num));
		terminal_write(num);
		terminal_write_line(" mismatches");
	}

	/* Test second page's physical address is contiguous */
	{
		unsigned long phys2 = paging_get_phys((unsigned long)ptr + MEMORY_PAGE_SIZE);
		terminal_write("  page1 phys: ");
		terminal_write_hex64(phys);
		terminal_putc('\n');
		terminal_write("  page2 phys: ");
		terminal_write_hex64(phys2);
		terminal_putc('\n');
		(void)phys2;
	}

	virt_free_pages(ptr, 2);
	terminal_write_line("  freed: OK");

	/* Verify pages are gone */
	{
		unsigned long after = paging_get_phys((unsigned long)ptr);
		if (after == 0)
			terminal_write_line("  unmap verify: OK");
		else
			terminal_write_line("  unmap verify: FAIL - page still present after free");
	}

	if (errors == 0)
		terminal_write_line("pagetest: PASSED");
	else
		terminal_write_line("pagetest: FAILED");
}

static void cmd_exec(const char *args)
{
	char path[128];
	unsigned char buf[8192];
	unsigned long size = 0;
	elf_exec_t prog;
	int rc;
	const char *p = read_token(args, path, sizeof(path));

	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: exec <path>");
		return;
	}

	if (read_shell_file_bytes("exec", path, buf, sizeof(buf), &size) != 0) return;

	terminal_write("[exec] loading ");
	terminal_write(path);
	terminal_write_line(" ...");

	rc = elf_load(buf, size, &prog);
	if (rc != 0)
	{
		terminal_write("[exec] load error: ");
		switch (rc)
		{
			case ELF_ERR_MAGIC:  terminal_write_line("bad ELF magic"); break;
			case ELF_ERR_CLASS:  terminal_write_line("not ELF64"); break;
			case ELF_ERR_TYPE:   terminal_write_line("not executable or shared"); break;
			case ELF_ERR_ARCH:   terminal_write_line("not x86-64"); break;
			case ELF_ERR_PHDR:   terminal_write_line("bad program headers"); break;
			case ELF_ERR_MAP:    terminal_write_line("page mapping failed"); break;
			case ELF_ERR_RANGE:  terminal_write_line("segment exceeds file"); break;
			default:             terminal_write_line("unknown error"); break;
		}
		return;
	}

	terminal_write("[exec] entry: ");
	terminal_write_hex64(prog.entry);
	terminal_putc('\n');
	terminal_write_line("[exec] calling...");

	{
#define USER_STACK_TOP   0x800000UL
#define USER_STACK_PAGES 8
#define USER_STACK_BASE  (USER_STACK_TOP - (unsigned long)(USER_STACK_PAGES) * 4096UL)
		unsigned long page;
		unsigned long phys_pages[USER_STACK_PAGES];
		int map_ok = 1;
		int i;
		unsigned long flags = PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_USER | PAGE_FLAG_NO_EXECUTE;

		/* Map user stack pages */
		for (i = 0; i < USER_STACK_PAGES; i++) phys_pages[i] = 0;
		for (i = 0; i < USER_STACK_PAGES && map_ok; i++)
		{
			page = USER_STACK_BASE + (unsigned long)i * 4096UL;
			phys_pages[i] = phys_alloc_page();
			if (phys_pages[i] == 0 || paging_map_page(page, phys_pages[i], flags) != 0)
			{
				if (phys_pages[i] != 0) { phys_free_page(phys_pages[i]); phys_pages[i] = 0; }
				map_ok = 0;
			}
		}

		if (!map_ok)
		{
			/* Unwind any pages already mapped */
			for (i = 0; i < USER_STACK_PAGES; i++)
			{
				if (phys_pages[i] != 0)
				{
					paging_unmap_page(USER_STACK_BASE + (unsigned long)i * 4096UL);
					phys_free_page(phys_pages[i]);
				}
			}
			terminal_write_line("[exec] failed to map user stack");
			elf_unload(buf, size);
			return;
		}

		task_exec_user(prog.entry, USER_STACK_TOP);

		/* Unmap user stack pages */
		for (i = 0; i < USER_STACK_PAGES; i++)
		{
			page = USER_STACK_BASE + (unsigned long)i * 4096UL;
			paging_unmap_page(page);
			if (phys_pages[i] != 0) phys_free_page(phys_pages[i]);
		}
	}

	elf_unload(buf, size);
	terminal_write_line("[exec] done");
}

struct task_test_ctx { unsigned int id; };
static struct task_test_ctx tasktest_ctx_a = { 1 };
static struct task_test_ctx tasktest_ctx_b = { 2 };

static void task_test_fn(void *arg)
{
	struct task_test_ctx *c = (struct task_test_ctx *)arg;
	unsigned int i;
	for (i = 0; i < 3; i++)
	{
		terminal_write("[tasktest] worker ");
		terminal_write(c->id == 1 ? "A" : "B");
		terminal_write(" step ");
		terminal_write(i == 0 ? "0" : i == 1 ? "1" : "2");
		terminal_write_line("");
		task_yield();
	}
}

static void cmd_tasks(void)
{
	task_print_list();
}

static void cmd_tasktest(void)
{
	if (task_create("ttest-a", task_test_fn, &tasktest_ctx_a) < 0 ||
	    task_create("ttest-b", task_test_fn, &tasktest_ctx_b) < 0)
	{
		terminal_write_line("[tasktest] task_create failed");
		return;
	}
	terminal_write_line("[tasktest] tasks created; yielding to let them run...");
	{
		unsigned int n;
		for (n = 0; n < 10; n++) task_yield();
	}
	terminal_write_line("[tasktest] back in kernel shell");
}

static volatile unsigned long taskspin_counter = 0;

static void task_spin_fn(void *arg)
{
	(void)arg;
	for (;;)
	{
		taskspin_counter++;
		if ((taskspin_counter & 0x3FFFUL) == 0) task_yield();
	}
}

static void cmd_taskspin(void)
{
	int id = task_create("spin", task_spin_fn, (void *)0);
	if (id < 0)
	{
		terminal_write_line("taskspin: task_create failed");
		return;
	}
	terminal_write("taskspin: started task ");
	{
		char n[16];
		uint_to_dec((unsigned long)id, n, sizeof(n));
		terminal_write_line(n);
	}
}

static void cmd_shellspawn(void)
{
	int id = kernel_ensure_shell_task();
	if (id < 0)
	{
		terminal_write_line("shellspawn: failed to start shell task");
		return;
	}
	terminal_write("shellspawn: shell task id ");
	{
		char n[16];
		uint_to_dec((unsigned long)id, n, sizeof(n));
		terminal_write_line(n);
	}
}

static void cmd_taskprotect(const char *args)
{
	char target[24];
	char mode[8];
	unsigned int id;
	const char *rest = read_token(args, target, sizeof(target));

	if (rest == (void *)0 || target[0] == '\0')
	{
		terminal_write_line("Usage: taskprotect <id|name> <on|off>");
		terminal_write_line("Usage: taskprotect <id> status");
		return;
	}

	if (read_token(rest, mode, sizeof(mode)) == (void *)0 || mode[0] == '\0')
	{
		terminal_write_line("Usage: taskprotect <id|name> <on|off>");
		terminal_write_line("Usage: taskprotect <id> status");
		return;
	}

	if (parse_dec_u32(target, &id) == 0)
	{
		if (string_equals(mode, "status"))
		{
			terminal_write("taskprotect: ");
			terminal_write(task_is_protected_id(id) ? "ON" : "OFF");
			terminal_putc('\n');
			return;
		}
		if (string_equals(mode, "on") || string_equals(mode, "off"))
		{
			if (task_set_protection_by_id(id, string_equals(mode, "on")) != 0)
			{
				terminal_write_line("taskprotect: failed");
				return;
			}
			terminal_write_line("taskprotect: updated");
			return;
		}
	}
	else
	{
		if (string_equals(mode, "on") || string_equals(mode, "off"))
		{
			if (task_set_protection_by_name(target, string_equals(mode, "on")) != 0)
			{
				terminal_write_line("taskprotect: failed");
				return;
			}
			terminal_write_line("taskprotect: updated");
			return;
		}
	}

	terminal_write_line("Usage: taskprotect <id|name> <on|off>");
	terminal_write_line("Usage: taskprotect <id> status");
}

static void cmd_shellwatch(const char *args)
{
	char tok[8];
	if (read_token(args, tok, sizeof(tok)) == (void *)0 || tok[0] == '\0' || string_equals(tok, "show"))
	{
		terminal_write("shellwatch: ");
		terminal_write_line(kernel_shell_watchdog_enabled() ? "on" : "off");
		return;
	}
	if (string_equals(tok, "on"))
	{
		kernel_set_shell_watchdog(1);
		terminal_write_line("shellwatch: enabled");
		return;
	}
	if (string_equals(tok, "off"))
	{
		kernel_set_shell_watchdog(0);
		terminal_write_line("shellwatch: disabled");
		return;
	}
	terminal_write_line("Usage: shellwatch [on|off|show]");
}

static void cmd_tasklog(const char *args)
{
	char tok[8];
	if (read_token(args, tok, sizeof(tok)) == (void *)0 || tok[0] == '\0' || string_equals(tok, "show"))
	{
		terminal_write("tasklog: ");
		terminal_write_line(task_event_log_enabled() ? "on" : "off");
		return;
	}
	if (string_equals(tok, "on"))
	{
		task_set_event_log(1);
		terminal_write_line("tasklog: enabled");
		return;
	}
	if (string_equals(tok, "off"))
	{
		task_set_event_log(0);
		terminal_write_line("tasklog: disabled");
		return;
	}
	terminal_write_line("Usage: tasklog [on|off|show]");
}

static void cmd_motd(void)
{
	terminal_print_motd();
}

static void cmd_autorun(const char *args)
{
	char tok[16];
	const char *rest;
	int mode;

	rest = read_token(args, tok, sizeof(tok));
	if (rest == (void *)0 || tok[0] == '\0' || string_equals(tok, "show"))
	{
		char n[16];
		unsigned long delay_seconds = terminal_get_autorun_delay_seconds();
		mode = terminal_get_autorun_mode();
		terminal_write("autorun: mode=");
		if (mode == 0) terminal_write("off");
		else if (mode == 2) terminal_write("once");
		else terminal_write("always");
		terminal_write(" delay=");
		uint_to_dec(delay_seconds, n, sizeof(n));
		terminal_write(n);
		terminal_write("s");
		if (mode == 2)
		{
			terminal_write(" state=");
			terminal_write(terminal_autorun_once_done() ? "done" : "armed");
		}
		if (autorun_boot_pending)
		{
			unsigned long now = timer_ticks();
			terminal_write(" pending=");
			if (now < autorun_boot_deadline)
			{
				char n[16];
				unsigned long left = (autorun_boot_deadline - now + 99UL) / 100UL;
				uint_to_dec(left, n, sizeof(n));
				terminal_write(n);
				terminal_write("s");
			}
			else terminal_write("ready");
		}
		terminal_putc('\n');
		return;
	}
	if (string_equals(tok, "delay"))
	{
		char n[16];
		char value_tok[16];
		unsigned int seconds;

		if (read_token(rest, value_tok, sizeof(value_tok)) == (void *)0 || value_tok[0] == '\0' ||
			string_equals(value_tok, "show"))
		{
			uint_to_dec(terminal_get_autorun_delay_seconds(), n, sizeof(n));
			terminal_write("autorun: delay=");
			terminal_write(n);
			terminal_write_line("s");
			return;
		}

		if (parse_dec_u32(value_tok, &seconds) != 0 || seconds > 3600U)
		{
			terminal_write_line("Usage: autorun delay <0..3600>");
			return;
		}

		terminal_set_autorun_delay_seconds((unsigned long)seconds);
		if (autorun_boot_pending) terminal_schedule_boot_autorun();
		uint_to_dec((unsigned long)seconds, n, sizeof(n));
		terminal_write("autorun: delay set to ");
		terminal_write(n);
		terminal_write_line("s");
		return;
	}

	if (string_equals(tok, "off"))
	{
		terminal_set_autorun_mode(0);
		autorun_boot_pending = 0;
		terminal_write_line("autorun: mode off");
		return;
	}
	if (string_equals(tok, "always") || string_equals(tok, "on"))
	{
		terminal_set_autorun_mode(1);
		terminal_schedule_boot_autorun();
		terminal_write_line("autorun: mode always");
		return;
	}
	if (string_equals(tok, "once"))
	{
		terminal_set_autorun_mode(2);
		terminal_set_autorun_once_done(0);
		terminal_schedule_boot_autorun();
		terminal_write_line("autorun: mode once (armed)");
		return;
	}
	if (string_equals(tok, "rearm"))
	{
		terminal_set_autorun_once_done(0);
		terminal_schedule_boot_autorun();
		terminal_write_line("autorun: once state re-armed");
		return;
	}
	if (string_equals(tok, "stop"))
	{
		autorun_boot_pending = 0;
		terminal_write_line("autorun: canceled for this boot");
		return;
	}
	if (string_equals(tok, "run"))
	{
		if (terminal_run_autorun_script_now()) terminal_write_line("autorun: executed");
		else terminal_write_line("autorun: no script found");
		return;
	}

	terminal_write_line("Usage: autorun [show|off|always|once|rearm|stop|run|delay <0..3600>]");
}

static void cmd_taskkill(const char *args)
{
	char tok[16];
	unsigned int id;
	int killed = 0;

	if (read_token(args, tok, sizeof(tok)) == (void *)0 || tok[0] == '\0')
	{
		terminal_write_line("Usage: taskkill <id>|all");
		return;
	}

	if (string_equals(tok, "all"))
	{
		killed = task_kill_all();
		if (killed == 0) terminal_write_line("taskkill: no killable tasks");
		else terminal_write_line("taskkill: terminated matching tasks");
		return;
	}

	if (parse_dec_u32(tok, &id) != 0)
	{
		terminal_write_line("Usage: taskkill <id>|all");
		return;
	}

	if (task_kill(id) != 0)
	{
		terminal_write_line("taskkill: failed (invalid/protected/running task)");
		return;
	}
	terminal_write_line("taskkill: terminated");
}

static void cmd_taskstop(const char *args)
{
	char tok[16];
	unsigned int id;
	if (read_token(args, tok, sizeof(tok)) == (void *)0 || tok[0] == '\0' || parse_dec_u32(tok, &id) != 0)
	{
		terminal_write_line("Usage: taskstop <id>");
		return;
	}
	if (task_stop(id) != 0)
	{
		terminal_write_line("taskstop: failed");
		return;
	}
	terminal_write_line("taskstop: stopped");
}

static void cmd_taskcont(const char *args)
{
	char tok[16];
	unsigned int id;
	if (read_token(args, tok, sizeof(tok)) == (void *)0 || tok[0] == '\0' || parse_dec_u32(tok, &id) != 0)
	{
		terminal_write_line("Usage: taskcont <id>");
		return;
	}
	if (task_continue(id) != 0)
	{
		terminal_write_line("taskcont: failed");
		return;
	}
	terminal_write_line("taskcont: resumed");
}

static void cmd_ticks(void)
{
	char n[24];
	uint_to_dec(timer_ticks(), n, sizeof(n));
	terminal_write("ticks: ");
	terminal_write_line(n);
}

static void cmd_elfinfo(const char *args)
{
	char path[128];
	unsigned char buf[8192];
	unsigned long size = 0;
	elf_info_t info;
	struct elf_phdr_print_ctx phdr_ctx;
	struct elf_section_print_ctx section_ctx;
	int rc;
	const char *p = read_token(args, path, sizeof(path));

	if (p == (void *)0 || path[0] == '\0' || skip_spaces(p)[0] != '\0')
	{
		terminal_write_line("Usage: elfinfo <path>");
		return;
	}
	if (read_shell_file_bytes("elfinfo", path, buf, sizeof(buf), &size) != 0) return;

	rc = elf_get_info(buf, size, &info);
	if (rc != ELF_OK)
	{
		terminal_write("elfinfo: parse error ");
		if (rc < 0)
		{
			char n[16];
			terminal_putc('-');
			uint_to_dec((unsigned long)(-rc), n, sizeof(n));
			terminal_write_line(n);
		}
		else
		{
			char n[16];
			uint_to_dec((unsigned long)rc, n, sizeof(n));
			terminal_write_line(n);
		}
		return;
	}

	terminal_write("elfinfo: ");
	terminal_write_line(path);
	terminal_write("  type: ");
	terminal_write(elf_type_name(info.type));
	terminal_write("  machine: ");
	if (info.machine == 62) terminal_write_line("x86-64");
	else terminal_write_line("other");
	terminal_write("  entry: ");
	terminal_write_hex64(info.entry);
	terminal_putc('\n');
	terminal_write("  phnum: ");
	{
		char n[16];
		uint_to_dec(info.phnum, n, sizeof(n));
		terminal_write(n);
		terminal_write("  shnum: ");
		uint_to_dec(info.shnum, n, sizeof(n));
		terminal_write_line(n);
	}
	terminal_write("  load range: ");
	if (info.load_end > info.load_base)
	{
		terminal_write_hex64(info.load_base);
		terminal_write(" - ");
		terminal_write_hex64(info.load_end);
		terminal_putc('\n');
	}
	else
	{
		terminal_write_line("none");
	}
	terminal_write("  symbols: ");
	{
		char n[16];
		uint_to_dec(info.symbol_count, n, sizeof(n));
		terminal_write_line(n);
	}

	terminal_write_line("  program headers:");
	phdr_ctx.count = 0;
	rc = elf_visit_program_headers(buf, size, print_elf_phdr, &phdr_ctx);
	if (rc == ELF_ERR_PHDR)
	{
		terminal_write_line("    none");
	}
	else if (rc != ELF_OK)
	{
		terminal_write_line("    <parse failed>");
	}
	else if (phdr_ctx.count == 0)
	{
		terminal_write_line("    none");
	}

	terminal_write_line("  sections:");
	section_ctx.count = 0;
	rc = elf_visit_sections(buf, size, print_elf_section, &section_ctx);
	if (rc == ELF_ERR_SHDR)
	{
		terminal_write_line("    none");
	}
	else if (rc != ELF_OK)
	{
		terminal_write_line("    <parse failed>");
	}
	else if (section_ctx.count == 0)
	{
		terminal_write_line("    none");
	}
}

static void cmd_elfsym(const char *args)
{
	char path[128];
	char filter[64];
	unsigned char buf[8192];
	unsigned long size = 0;
	struct elf_symbol_print_ctx ctx;
	int rc;
	const char *p = read_token(args, path, sizeof(path));

	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: elfsym <path> [filter]");
		return;
	}
	p = read_token(p, filter, sizeof(filter));
	if (p == (void *)0)
	{
		terminal_write_line("Usage: elfsym <path> [filter]");
		return;
	}
	if (filter[0] != '\0' && skip_spaces(p)[0] != '\0')
	{
		terminal_write_line("Usage: elfsym <path> [filter]");
		return;
	}
	if (read_shell_file_bytes("elfsym", path, buf, sizeof(buf), &size) != 0) return;

	ctx.filter = filter[0] == '\0' ? (void *)0 : filter;
	ctx.total = 0;
	ctx.matched = 0;

	terminal_write("elfsym: ");
	terminal_write_line(path);
	rc = elf_visit_symbols(buf, size, print_elf_symbol, &ctx);
	if (rc == ELF_ERR_NOSYM)
	{
		terminal_write_line("  no named defined symbols");
		return;
	}
	if (rc != ELF_OK)
	{
		terminal_write_line("  parse failed");
		return;
	}
	terminal_write("  matched ");
	{
		char n[16];
		uint_to_dec(ctx.matched, n, sizeof(n));
		terminal_write(n);
		terminal_write(" of ");
		uint_to_dec(ctx.total, n, sizeof(n));
		terminal_write_line(n);
	}
}

static void cmd_elfaddr(const char *args)
{
	char path[128];
	char addr_tok[32];
	unsigned char buf[8192];
	unsigned long size = 0;
	unsigned long addr;
	unsigned long offset;
	elf_symbol_t sym;
	const char *end;
	const char *p = read_token(args, path, sizeof(path));
	int rc;

	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: elfaddr <path> <hex-address>");
		return;
	}
	p = read_token(p, addr_tok, sizeof(addr_tok));
	if (p == (void *)0 || addr_tok[0] == '\0' || skip_spaces(p)[0] != '\0')
	{
		terminal_write_line("Usage: elfaddr <path> <hex-address>");
		return;
	}
	addr = parse_hex(addr_tok, &end);
	if (*end != '\0')
	{
		terminal_write_line("elfaddr: bad hex address");
		return;
	}
	if (read_shell_file_bytes("elfaddr", path, buf, sizeof(buf), &size) != 0) return;

	rc = elf_find_symbol_by_addr(buf, size, addr, &sym, &offset);
	if (rc == ELF_ERR_NOSYM)
	{
		terminal_write_line("elfaddr: no matching symbol");
		return;
	}
	if (rc != ELF_OK)
	{
		terminal_write_line("elfaddr: parse failed");
		return;
	}

	terminal_write("elfaddr: ");
	terminal_write_hex64(addr);
	terminal_write(" -> ");
	terminal_write(sym.name);
	terminal_write("+");
	terminal_write_hex64(offset);
	terminal_write(" (");
	terminal_write(elf_symbol_type_name(sym.info));
	terminal_write(", ");
	terminal_write(elf_symbol_bind_name(sym.info));
	terminal_write(")");
	terminal_putc('\n');
}

static void cmd_execstress(const char *args)
{
	char count_tok[16];
	char path[128];
	unsigned char buf[8192];
	unsigned long size = 0;
	unsigned int run_count = 0;
	unsigned long before_free;
	unsigned long after_free;
	unsigned long i;
	unsigned long ok_runs = 0;
	int last_rc = 0;
	long last_ret = 0;
	const char *p = read_token(args, count_tok, sizeof(count_tok));

	if (p == (void *)0 || count_tok[0] == '\0' || parse_dec_u32(count_tok, &run_count) != 0 || run_count == 0)
	{
		terminal_write_line("Usage: execstress <count> <path>");
		return;
	}
	p = read_token(p, path, sizeof(path));
	if (p == (void *)0 || path[0] == '\0' || skip_spaces(p)[0] != '\0')
	{
		terminal_write_line("Usage: execstress <count> <path>");
		return;
	}

	if (read_shell_file_bytes("execstress", path, buf, sizeof(buf), &size) != 0) return;
	terminal_take_cancel_request(); /* clear stale request */

	before_free = memory_free_pages();
	for (i = 0; i < (unsigned long)run_count; i++)
	{
		elf_exec_t prog;
		terminal_poll_control_hotkeys();
		if (terminal_take_cancel_request())
		{
			terminal_write_line("execstress: canceled by user");
			break;
		}
		last_rc = elf_load(buf, size, &prog);
		if (last_rc != 0) break;
		last_ret = elf_call(&prog);
		elf_unload(buf, size);
		ok_runs++;
	}
	after_free = memory_free_pages();

	terminal_write("execstress: requested ");
	{
		char n[16];
		uint_to_dec((unsigned long)run_count, n, sizeof(n));
		terminal_write(n);
	}
	terminal_write(", completed ");
	{
		char n[16];
		uint_to_dec(ok_runs, n, sizeof(n));
		terminal_write(n);
	}
	terminal_putc('\n');

	terminal_write("  free pages before: ");
	{
		char n[16];
		uint_to_dec(before_free, n, sizeof(n));
		terminal_write_line(n);
	}
	terminal_write("  free pages after:  ");
	{
		char n[16];
		uint_to_dec(after_free, n, sizeof(n));
		terminal_write_line(n);
	}

	terminal_write("  delta pages: ");
	if (after_free >= before_free)
	{
		char n[16];
		terminal_putc('+');
		uint_to_dec(after_free - before_free, n, sizeof(n));
		terminal_write_line(n);
	}
	else
	{
		char n[16];
		terminal_putc('-');
		uint_to_dec(before_free - after_free, n, sizeof(n));
		terminal_write_line(n);
	}

	if (ok_runs < (unsigned long)run_count)
	{
		terminal_write("  stopped at run ");
		{
			char n[16];
			uint_to_dec(ok_runs + 1, n, sizeof(n));
			terminal_write(n);
		}
		terminal_write(", elf_load rc=");
		if (last_rc < 0)
		{
			char n[16];
			terminal_putc('-');
			uint_to_dec((unsigned long)(-last_rc), n, sizeof(n));
			terminal_write_line(n);
		}
		else
		{
			char n[16];
			uint_to_dec((unsigned long)last_rc, n, sizeof(n));
			terminal_write_line(n);
		}
	}
	else
	{
		terminal_write("  last return: ");
		if (last_ret < 0)
		{
			char n[16];
			terminal_putc('-');
			uint_to_dec((unsigned long)(-last_ret), n, sizeof(n));
			terminal_write_line(n);
		}
		else
		{
			char n[16];
			uint_to_dec((unsigned long)last_ret, n, sizeof(n));
			terminal_write_line(n);
		}
	}
}

static void cmd_elfselftest(const char *args)
{
	unsigned long before_free;
	unsigned long after_free;
	unsigned long i;
	unsigned long failures = 0;
	const struct elf_case {
		const char *path;
		long expected_ret;
	} tests[] = {
		{"/app.elf", 42},
		{"/appw.elf", 7},
		{"/app2p.elf", 99}
	};

	if (skip_spaces(args)[0] != '\0')
	{
		terminal_write_line("Usage: elfselftest");
		return;
	}

	terminal_write_line("elfselftest: running built-in ELF matrix...");
	terminal_take_cancel_request(); /* clear stale request */

	for (i = 0; i < (unsigned long)(sizeof(tests) / sizeof(tests[0])); i++)
	{
		unsigned char buf[8192];
		unsigned long size = 0;
		elf_exec_t prog;
		int rc;
		long ret;

		terminal_poll_control_hotkeys();
		if (terminal_take_cancel_request())
		{
			terminal_write_line("elfselftest: canceled by user");
			break;
		}

		if (fs_read_file(tests[i].path, buf, sizeof(buf), &size) != 0)
		{
			terminal_write("  FAIL "); terminal_write(tests[i].path); terminal_write_line(" : read failed");
			failures++;
			continue;
		}
		rc = elf_load(buf, size, &prog);
		if (rc != 0)
		{
			terminal_write("  FAIL "); terminal_write(tests[i].path); terminal_write(" : elf_load rc=");
			if (rc < 0)
			{
				char n[16];
				terminal_putc('-');
				uint_to_dec((unsigned long)(-rc), n, sizeof(n));
				terminal_write_line(n);
			}
			else
			{
				char n[16];
				uint_to_dec((unsigned long)rc, n, sizeof(n));
				terminal_write_line(n);
			}
			failures++;
			continue;
		}
		ret = elf_call(&prog);
		elf_unload(buf, size);
		if (ret != tests[i].expected_ret)
		{
			terminal_write("  FAIL "); terminal_write(tests[i].path); terminal_write(" : ret=");
			if (ret < 0)
			{
				char n[16];
				terminal_putc('-');
				uint_to_dec((unsigned long)(-ret), n, sizeof(n));
				terminal_write(n);
			}
			else
			{
				char n[16];
				uint_to_dec((unsigned long)ret, n, sizeof(n));
				terminal_write(n);
			}
			terminal_write(" expected=");
			{
				char n[16];
				if (tests[i].expected_ret < 0)
				{
					terminal_putc('-');
					uint_to_dec((unsigned long)(-tests[i].expected_ret), n, sizeof(n));
				}
				else
				{
					uint_to_dec((unsigned long)tests[i].expected_ret, n, sizeof(n));
				}
				terminal_write_line(n);
			}
			failures++;
		}
		else
		{
			terminal_write("  PASS "); terminal_write_line(tests[i].path);
		}
	}

	{
		unsigned char buf[8192];
		unsigned long size = 0;
		elf_info_t info;
		elf_symbol_t sym;
		unsigned long offset = 0;
		if (fs_read_file("/app.elf", buf, sizeof(buf), &size) != 0)
		{
			terminal_write_line("  FAIL symbols /app.elf : read failed");
			failures++;
		}
		else if (elf_get_info(buf, size, &info) != ELF_OK || info.symbol_count == 0)
		{
			terminal_write_line("  FAIL symbols /app.elf : symbol table missing");
			failures++;
		}
		else if (elf_find_symbol_by_addr(buf, size, info.entry, &sym, &offset) != ELF_OK || offset != 0 || !string_equals(sym.name, "_start"))
		{
			terminal_write_line("  FAIL symbols /app.elf : entry lookup mismatch");
			failures++;
		}
		else
		{
			terminal_write_line("  PASS symbols /app.elf");
		}
	}

	/* Short stress pass on multi-page image to catch regressions quickly. */
	before_free = memory_free_pages();
	{
		unsigned char buf[8192];
		unsigned long size = 0;
		unsigned long ok_runs = 0;
		int rc = 0;
		if (fs_read_file("/app2p.elf", buf, sizeof(buf), &size) != 0)
		{
			terminal_write_line("  FAIL stress /app2p.elf : read failed");
			failures++;
		}
		else
		{
			for (i = 0; i < 256UL; i++)
			{
				elf_exec_t prog;
				rc = elf_load(buf, size, &prog);
				if (rc != 0) break;
				(void)elf_call(&prog);
				elf_unload(buf, size);
				ok_runs++;
			}
			if (ok_runs != 256UL)
			{
				terminal_write("  FAIL stress /app2p.elf at run ");
				{
					char n[16];
					uint_to_dec(ok_runs + 1, n, sizeof(n));
					terminal_write(n);
				}
				terminal_write(" rc=");
				if (rc < 0)
				{
					char n[16];
					terminal_putc('-');
					uint_to_dec((unsigned long)(-rc), n, sizeof(n));
					terminal_write_line(n);
				}
				else
				{
					char n[16];
					uint_to_dec((unsigned long)rc, n, sizeof(n));
					terminal_write_line(n);
				}
				failures++;
			}
			else
			{
				terminal_write_line("  PASS stress /app2p.elf x256");
			}
		}
	}

	after_free = memory_free_pages();
	terminal_write("  free-page delta: ");
	if (after_free >= before_free)
	{
		char n[16];
		terminal_putc('+');
		uint_to_dec(after_free - before_free, n, sizeof(n));
		terminal_write_line(n);
	}
	else
	{
		char n[16];
		terminal_putc('-');
		uint_to_dec(before_free - after_free, n, sizeof(n));
		terminal_write_line(n);
		failures++;
	}

	if (failures == 0) terminal_write_line("elfselftest: PASSED");
	else terminal_write_line("elfselftest: FAILED");
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

static int trim_dir_entry_name(const char *name, char *out, unsigned long out_size)
{
	unsigned long i = 0;
	if (out_size == 0) return -1;
	while (name[i] != '\0' && i + 1 < out_size)
	{
		if (name[i] == '/' && name[i + 1] == '\0') break;
		out[i] = name[i];
		i++;
	}
	out[i] = '\0';
	return 0;
}

static int dir_name_compare_ci(const char *a, const char *b)
{
	unsigned long i = 0;
	for (;;)
	{
		char ca = ascii_upper(a[i]);
		char cb = ascii_upper(b[i]);
		if (ca != cb) return (ca < cb) ? -1 : 1;
		if (ca == '\0') return 0;
		i++;
	}
}

static void dir_sort_entries(char names[][40], int types[], unsigned long sizes[], int count)
{
	int i;
	int j;
	for (i = 0; i < count; i++)
	{
		for (j = i + 1; j < count; j++)
		{
			if (dir_name_compare_ci(names[i], names[j]) > 0)
			{
				char tmp_name[40];
				int tmp_type;
				unsigned long tmp_size;
				unsigned long k;
				for (k = 0; k < sizeof(tmp_name); k++) tmp_name[k] = names[i][k];
				for (k = 0; k < sizeof(tmp_name); k++) names[i][k] = names[j][k];
				for (k = 0; k < sizeof(tmp_name); k++) names[j][k] = tmp_name[k];
				tmp_type = types[i];
				types[i] = types[j];
				types[j] = tmp_type;
				tmp_size = sizes[i];
				sizes[i] = sizes[j];
				sizes[j] = tmp_size;
			}
		}
	}
}

static int build_child_path(const char *base, const char *name, int use_fat, char *out, unsigned long out_size)
{
	unsigned long n = 0;
	unsigned long i = 0;
	if (out == (void *)0 || out_size == 0) return -1;
	if (base == (void *)0 || base[0] == '\0' || string_equals(base, "."))
		base = use_fat ? fat_cwd : "";
	if (!use_fat && (base[0] == '\0' || string_equals(base, ".")))
	{
		while (name[i] != '\0' && n + 1 < out_size) out[n++] = name[i++];
		out[n] = '\0';
		return 0;
	}
	if (use_fat && base[0] == '\0') base = "/";
	while (base[n] != '\0' && n + 1 < out_size) { out[n] = base[n]; n++; }
	if (n == 0 && use_fat) out[n++] = '/';
	if (n > 0 && out[n - 1] != '/' && n + 1 < out_size) out[n++] = '/';
	while (name[i] != '\0' && n + 1 < out_size) out[n++] = name[i++];
	out[n] = '\0';
	return (name[i] == '\0') ? 0 : -1;
}

static void dir_print_entry(int is_dir, unsigned long size, const char *name)
{
	char num[32];
	terminal_write("  ");
	if (is_dir)
	{
		terminal_write("<DIR>          ");
	}
	else
	{
		unsigned long digits;
		unsigned long pad;
		uint_to_dec(size, num, sizeof(num));
		digits = string_length(num);
		pad = (digits < 13) ? (13 - digits) : 0;
		while (pad-- > 0) terminal_putc(' ');
		terminal_write(num);
		terminal_putc(' ');
	}
	terminal_write_line(name);
}

static void dir_print_totals(unsigned long file_count, unsigned long total_bytes, unsigned long dir_count)
{
	char num[32];
	terminal_write("     ");
	uint_to_dec(file_count, num, sizeof(num));
	terminal_write(num);
	terminal_write_line(" File(s)");
	terminal_write("  ");
	uint_to_dec(total_bytes, num, sizeof(num));
	terminal_write(num);
	terminal_write_line(" bytes");
	terminal_write("     ");
	uint_to_dec(dir_count, num, sizeof(num));
	terminal_write(num);
	terminal_write_line(" Dir(s)");
}

static int dir_collect_entries_for_path(const char *path, int use_fat, char entries[][40], int entry_types[], unsigned long entry_sizes[], int *entry_count)
{
	int i;
	if (entry_count == (void *)0) return -1;
	*entry_count = 0;

	if (use_fat)
	{
		char full_path[128];
		char fat_names[64][40];
		int fat_count;
		if (fat_resolve_path((path == (void *)0 || path[0] == '\0') ? "." : path, full_path, sizeof(full_path)) != 0) return -1;
		if (fat32_ls_path(full_path, fat_names, 64, &fat_count) != 0) return -1;
		for (i = 0; i < fat_count; i++)
		{
			char child_name[40];
			char child_path[128];
			int child_is_dir;
			unsigned long child_size;
			trim_dir_entry_name(fat_names[i], child_name, sizeof(child_name));
			if (build_child_path(full_path, child_name, 1, child_path, sizeof(child_path)) != 0) continue;
			if (fat32_stat_path(child_path, &child_is_dir, &child_size) != 0) continue;
			if (*entry_count >= FS_MAX_LIST) break;
			{
				unsigned long j = 0;
				while (child_name[j] != '\0' && j + 1 < sizeof(entries[0])) { entries[*entry_count][j] = child_name[j]; j++; }
				entries[*entry_count][j] = '\0';
			}
			entry_types[*entry_count] = child_is_dir;
			entry_sizes[*entry_count] = child_size;
			(*entry_count)++;
		}
		return 0;
	}

	{
		char names[FS_MAX_LIST][FS_NAME_MAX + 2];
		int types[FS_MAX_LIST];
		int count;
		if (fs_ls((path == (void *)0 || path[0] == '\0') ? (void *)0 : path, names, types, FS_MAX_LIST, &count) != 0) return -1;
		for (i = 0; i < count; i++)
		{
			char child_name[FS_NAME_MAX + 1];
			char child_path[128];
			int child_is_dir;
			unsigned long child_size;
			trim_dir_entry_name(names[i], child_name, sizeof(child_name));
			if (build_child_path(path, child_name, 0, child_path, sizeof(child_path)) != 0) continue;
			if (fs_stat(child_path, &child_is_dir, &child_size) != 0) continue;
			if (*entry_count >= FS_MAX_LIST) break;
			{
				unsigned long j = 0;
				while (child_name[j] != '\0' && j + 1 < sizeof(entries[0])) { entries[*entry_count][j] = child_name[j]; j++; }
				entries[*entry_count][j] = '\0';
			}
			entry_types[*entry_count] = child_is_dir;
			entry_sizes[*entry_count] = child_size;
			(*entry_count)++;
		}
	}

	return 0;
}

static void dir_recursive_walk(const char *path, int use_fat, int opt_bare, int opt_wide, unsigned long *out_files, unsigned long *out_dirs, unsigned long *out_bytes)
{
	char entries[FS_MAX_LIST][40];
	int entry_types[FS_MAX_LIST];
	unsigned long entry_sizes[FS_MAX_LIST];
	int entry_count = 0;
	int i;
	unsigned long local_files = 0;
	unsigned long local_dirs = 0;
	unsigned long local_bytes = 0;

	if (dir_collect_entries_for_path(path, use_fat, entries, entry_types, entry_sizes, &entry_count) != 0) return;
	dir_sort_entries(entries, entry_types, entry_sizes, entry_count);

	if (!opt_bare)
	{
		terminal_putc('\n');
		terminal_write(" Directory of ");
		if (path == (void *)0 || path[0] == '\0') terminal_write_line(use_fat ? fat_cwd : ".");
		else terminal_write_line(path);
		terminal_putc('\n');
	}

	if (opt_wide)
	{
		int col = 0;
		for (i = 0; i < entry_count; i++)
		{
			char shown[40];
			unsigned long j = 0;
			if (entry_types[i])
			{
				while (entries[i][j] != '\0' && j + 2 < sizeof(shown)) { shown[j] = entries[i][j]; j++; }
				shown[j++] = '/';
				shown[j] = '\0';
			}
			else
			{
				while (entries[i][j] != '\0' && j + 1 < sizeof(shown)) { shown[j] = entries[i][j]; j++; }
				shown[j] = '\0';
			}
			terminal_write("  ");
			terminal_write(shown);
			if (col < 3)
			{
				unsigned long w = string_length(shown);
				while (w++ < 18) terminal_putc(' ');
			}
			col++;
			if (col == 4 || i + 1 == entry_count)
			{
				terminal_putc('\n');
				col = 0;
			}
			if (entry_types[i]) local_dirs++;
			else { local_files++; local_bytes += entry_sizes[i]; }
		}
	}
	else if (opt_bare)
	{
		for (i = 0; i < entry_count; i++)
		{
			char child_path[128];
			if (build_child_path(path, entries[i], use_fat, child_path, sizeof(child_path)) == 0) terminal_write_line(child_path);
			else terminal_write_line(entries[i]);
			if (entry_types[i]) local_dirs++;
			else { local_files++; local_bytes += entry_sizes[i]; }
		}
	}
	else
	{
		for (i = 0; i < entry_count; i++)
		{
			dir_print_entry(entry_types[i], entry_sizes[i], entries[i]);
			if (entry_types[i]) local_dirs++;
			else { local_files++; local_bytes += entry_sizes[i]; }
		}
	}

	if (!opt_bare)
	{
		dir_print_totals(local_files, local_bytes, local_dirs);
	}

	if (out_files != (void *)0) *out_files += local_files;
	if (out_dirs != (void *)0) *out_dirs += local_dirs;
	if (out_bytes != (void *)0) *out_bytes += local_bytes;

	for (i = 0; i < entry_count; i++)
	{
		char child_path[128];
		if (!entry_types[i]) continue;
		if (build_child_path(path, entries[i], use_fat, child_path, sizeof(child_path)) != 0) continue;
		dir_recursive_walk(child_path, use_fat, opt_bare, opt_wide, out_files, out_dirs, out_bytes);
	}
}

static void cmd_dir(const char *args)
{
	char target[128];
	char tok[32];
	const char *p = args;
	int opt_bare = 0;
	int opt_wide = 0;
	int opt_recursive = 0;
	char entries[FS_MAX_LIST][40];
	int entry_types[FS_MAX_LIST];
	unsigned long entry_sizes[FS_MAX_LIST];
	int entry_count = 0;
	int i;
	unsigned long file_count = 0;
	unsigned long dir_count = 0;
	unsigned long total_bytes = 0;
	unsigned long free_bytes = 0;

	target[0] = '\0';
	for (;;)
	{
		p = read_token(p, tok, sizeof(tok));
		if (p == (void *)0)
		{
			terminal_write_line("dir: argument too long");
			return;
		}
		if (tok[0] == '\0') break;
		if (tok[0] == '/' || tok[0] == '-')
		{
			unsigned long k = 1;
			while (tok[k] != '\0')
			{
				char c = ascii_upper(tok[k]);
				if (c == 'B') opt_bare = 1;
				else if (c == 'W') opt_wide = 1;
				else if (c == 'S') opt_recursive = 1;
				else { terminal_write_line("dir: unknown option (use /b /w /s)"); return; }
				k++;
			}
			continue;
		}
		if (target[0] != '\0')
		{
			terminal_write_line("Usage: dir [/b] [/w] [/s] [path]");
			return;
		}
		{
			unsigned long k = 0;
			while (tok[k] != '\0' && k + 1 < sizeof(target)) { target[k] = tok[k]; k++; }
			target[k] = '\0';
		}
	}

	if (opt_wide && opt_bare) opt_wide = 0;
	if (opt_recursive)
	{
		int use_fat = fat_mode_active();
		int is_dir = 0;
		unsigned long size = 0;
		char root_path[128];
		unsigned long total_files = 0;
		unsigned long total_dirs = 0;
		unsigned long total_bytes = 0;
		unsigned long free_bytes = 0;

		if (use_fat)
		{
			if (fat_resolve_path(target[0] == '\0' ? "." : target, root_path, sizeof(root_path)) != 0)
			{
				terminal_write_line("dir: invalid FAT path");
				return;
			}
			if (fat32_stat_path(root_path, &is_dir, &size) != 0 || !is_dir)
			{
				terminal_write_line("dir: path is not a directory");
				return;
			}
			if (fat32_get_free_bytes(&free_bytes) != 0) free_bytes = 0;
		}
		else
		{
			if (fs_stat(target[0] == '\0' ? (void *)0 : target, &is_dir, &size) != 0 || !is_dir)
			{
				terminal_write_line("dir: path is not a directory");
				return;
			}
			if (target[0] == '\0') root_path[0] = '\0';
			else
			{
				unsigned long k = 0;
				while (target[k] != '\0' && k + 1 < sizeof(root_path)) { root_path[k] = target[k]; k++; }
				root_path[k] = '\0';
			}
			free_bytes = fs_free_bytes();
		}

		dir_recursive_walk(root_path, use_fat, opt_bare, opt_wide, &total_files, &total_dirs, &total_bytes);
		if (!opt_bare)
		{
			terminal_putc('\n');
			terminal_write_line("Total files listed:");
			dir_print_totals(total_files, total_bytes, total_dirs);
			{
				char num[32];
				terminal_write("  ");
				uint_to_dec(free_bytes, num, sizeof(num));
				terminal_write(num);
				terminal_write_line(" bytes free");
			}
		}
		return;
	}

	if (fat_mode_active())
	{
		char full_path[128];
		int is_dir;
		unsigned long size;
		if (fat_resolve_path(target[0] == '\0' ? "." : target, full_path, sizeof(full_path)) != 0)
		{
			terminal_write_line("dir: invalid FAT path");
			return;
		}
		if (fat32_stat_path(full_path, &is_dir, &size) != 0)
		{
			terminal_write_line("dir: invalid path");
			return;
		}
		if (!is_dir)
		{
			char name[40];
			trim_dir_entry_name(target[0] == '\0' ? full_path : target, name, sizeof(name));
			if (name[0] == '\0')
			{
				unsigned long j = 0;
				while (full_path[j] != '\0' && j + 1 < sizeof(entries[0])) { entries[0][j] = full_path[j]; j++; }
				entries[0][j] = '\0';
			}
			else
			{
				unsigned long j = 0;
				while (name[j] != '\0' && j + 1 < sizeof(entries[0])) { entries[0][j] = name[j]; j++; }
				entries[0][j] = '\0';
			}
			entry_types[0] = 0;
			entry_sizes[0] = size;
			entry_count = 1;
			if (fat32_get_free_bytes(&free_bytes) != 0) free_bytes = 0;
		}
		else
		{
			char fat_names[64][40];
			int fat_count;
			if (fat32_ls_path(full_path, fat_names, 64, &fat_count) != 0)
			{
				terminal_write_line("dir: invalid path");
				return;
			}
			for (i = 0; i < fat_count; i++)
			{
				char child_name[40];
				char child_path[128];
				int child_is_dir;
				unsigned long child_size;
				trim_dir_entry_name(fat_names[i], child_name, sizeof(child_name));
				if (build_child_path(full_path, child_name, 1, child_path, sizeof(child_path)) != 0) continue;
				if (fat32_stat_path(child_path, &child_is_dir, &child_size) != 0) continue;
				if (entry_count >= FS_MAX_LIST) break;
				{
					unsigned long j = 0;
					while (child_name[j] != '\0' && j + 1 < sizeof(entries[0])) { entries[entry_count][j] = child_name[j]; j++; }
					entries[entry_count][j] = '\0';
				}
				entry_types[entry_count] = child_is_dir;
				entry_sizes[entry_count] = child_size;
				entry_count++;
			}
			if (fat32_get_free_bytes(&free_bytes) != 0) free_bytes = 0;
		}
	}
	else
	{
		int is_dir;
		unsigned long size;
		if (fs_stat(target[0] == '\0' ? (void *)0 : target, &is_dir, &size) != 0)
		{
			terminal_write_line("dir: invalid path");
			return;
		}
		if (!is_dir)
		{
			unsigned long j = 0;
			while (target[j] != '\0' && j + 1 < sizeof(entries[0])) { entries[0][j] = target[j]; j++; }
			entries[0][j] = '\0';
			entry_types[0] = 0;
			entry_sizes[0] = size;
			entry_count = 1;
			free_bytes = fs_free_bytes();
		}
		else
		{
			char names[FS_MAX_LIST][FS_NAME_MAX + 2];
			int types[FS_MAX_LIST];
			int count;
			if (fs_ls(target[0] == '\0' ? (void *)0 : target, names, types, FS_MAX_LIST, &count) != 0)
			{
				terminal_write_line("dir: invalid path");
				return;
			}
			for (i = 0; i < count; i++)
			{
				char child_name[FS_NAME_MAX + 1];
				char child_path[128];
				int child_is_dir;
				unsigned long child_size;
				trim_dir_entry_name(names[i], child_name, sizeof(child_name));
				if (build_child_path(target, child_name, 0, child_path, sizeof(child_path)) != 0) continue;
				if (fs_stat(child_path, &child_is_dir, &child_size) != 0) continue;
				if (entry_count >= FS_MAX_LIST) break;
				{
					unsigned long j = 0;
					while (child_name[j] != '\0' && j + 1 < sizeof(entries[0])) { entries[entry_count][j] = child_name[j]; j++; }
					entries[entry_count][j] = '\0';
				}
				entry_types[entry_count] = child_is_dir;
				entry_sizes[entry_count] = child_size;
				entry_count++;
			}
			free_bytes = fs_free_bytes();
		}
	}

	dir_sort_entries(entries, entry_types, entry_sizes, entry_count);
	if (opt_wide)
	{
		int col = 0;
		for (i = 0; i < entry_count; i++)
		{
			char shown[40];
			char num[3];
			unsigned long j = 0;
			if (entry_types[i])
			{
				while (entries[i][j] != '\0' && j + 2 < sizeof(shown)) { shown[j] = entries[i][j]; j++; }
				shown[j++] = '/';
				shown[j] = '\0';
			}
			else
			{
				while (entries[i][j] != '\0' && j + 1 < sizeof(shown)) { shown[j] = entries[i][j]; j++; }
				shown[j] = '\0';
			}
			terminal_write("  ");
			terminal_write(shown);
			if (col < 3)
			{
				unsigned long w = string_length(shown);
				while (w++ < 18) terminal_putc(' ');
			}
			col++;
			if (col == 4 || i + 1 == entry_count)
			{
				terminal_putc('\n');
				col = 0;
			}
			(void)num;
			if (entry_types[i]) dir_count++;
			else { file_count++; total_bytes += entry_sizes[i]; }
		}
	}
	else if (opt_bare)
	{
		for (i = 0; i < entry_count; i++)
		{
			terminal_write_line(entries[i]);
			if (entry_types[i]) dir_count++;
			else { file_count++; total_bytes += entry_sizes[i]; }
		}
	}
	else
	{
	for (i = 0; i < entry_count; i++)
	{
		dir_print_entry(entry_types[i], entry_sizes[i], entries[i]);
		if (entry_types[i]) dir_count++;
		else { file_count++; total_bytes += entry_sizes[i]; }
	}
	}

	if (!opt_bare)
	{
		dir_print_totals(file_count, total_bytes, dir_count);
		{
			char num[32];
			terminal_write("  ");
			uint_to_dec(free_bytes, num, sizeof(num));
			terminal_write(num);
			terminal_write_line(" bytes free");
		}
	}
}

static int tree_collect_entries(const char *path, int use_fat, char names[][40], int types[], int *count)
{
	int i;
	if (use_fat)
	{
		char full_path[128];
		char fat_names[64][40];
		int fat_count;
		if (fat_resolve_path(path[0] == '\0' ? "." : path, full_path, sizeof(full_path)) != 0) return -1;
		if (fat32_ls_path(full_path, fat_names, 64, &fat_count) != 0) return -1;
		for (i = 0; i < fat_count && i < FS_MAX_LIST; i++)
		{
			char child_name[40];
			char child_path[128];
			int child_is_dir;
			unsigned long child_size;
			trim_dir_entry_name(fat_names[i], child_name, sizeof(child_name));
			if (build_child_path(full_path, child_name, 1, child_path, sizeof(child_path)) != 0) continue;
			if (fat32_stat_path(child_path, &child_is_dir, &child_size) != 0) continue;
			{
				unsigned long j = 0;
				while (child_name[j] != '\0' && j + 1 < sizeof(names[0])) { names[*count][j] = child_name[j]; j++; }
				names[*count][j] = '\0';
			}
			types[*count] = child_is_dir;
			(*count)++;
			if (*count >= FS_MAX_LIST) break;
		}
		return 0;
	}
	else
	{
		char raw_names[FS_MAX_LIST][FS_NAME_MAX + 2];
		int raw_types[FS_MAX_LIST];
		int raw_count;
		if (fs_ls(path[0] == '\0' ? (void *)0 : path, raw_names, raw_types, FS_MAX_LIST, &raw_count) != 0) return -1;
		for (i = 0; i < raw_count && i < FS_MAX_LIST; i++)
		{
			char child_name[FS_NAME_MAX + 1];
			trim_dir_entry_name(raw_names[i], child_name, sizeof(child_name));
			{
				unsigned long j = 0;
				while (child_name[j] != '\0' && j + 1 < sizeof(names[0])) { names[*count][j] = child_name[j]; j++; }
				names[*count][j] = '\0';
			}
			types[*count] = raw_types[i] ? 1 : 0;
			(*count)++;
			if (*count >= FS_MAX_LIST) break;
		}
		return 0;
	}
}

static void tree_walk(const char *path, int use_fat, int show_files, int depth)
{
	char names[FS_MAX_LIST][40];
	int types[FS_MAX_LIST];
	unsigned long dummy_sizes[FS_MAX_LIST];
	int count = 0;
	int i;
	if (tree_collect_entries(path, use_fat, names, types, &count) != 0) return;
	for (i = 0; i < count; i++) dummy_sizes[i] = 0;
	dir_sort_entries(names, types, dummy_sizes, count);
	for (i = 0; i < count; i++)
	{
		char child_path[128];
		int d;
		if (!show_files && !types[i]) continue;
		for (d = 0; d < depth; d++) terminal_write("  ");
		terminal_write("|- ");
		terminal_write_line(names[i]);
		if (types[i])
		{
			if (build_child_path(path, names[i], use_fat, child_path, sizeof(child_path)) == 0)
			{
				tree_walk(child_path, use_fat, show_files, depth + 1);
			}
		}
	}
}

static void cmd_tree(const char *args)
{
	char tok[32];
	char target[128];
	const char *p = args;
	int show_files = 0;
	int use_fat = fat_mode_active();
	int is_dir = 0;
	unsigned long size = 0;

	target[0] = '\0';
	for (;;)
	{
		p = read_token(p, tok, sizeof(tok));
		if (p == (void *)0) { terminal_write_line("tree: argument too long"); return; }
		if (tok[0] == '\0') break;
		if (tok[0] == '/' || tok[0] == '-')
		{
			unsigned long k = 1;
			while (tok[k] != '\0')
			{
				char c = ascii_upper(tok[k]);
				if (c == 'F') show_files = 1;
				else { terminal_write_line("tree: unknown option (use /f)"); return; }
				k++;
			}
			continue;
		}
		if (target[0] != '\0') { terminal_write_line("Usage: tree [/f] [path]"); return; }
		{
			unsigned long k = 0;
			while (tok[k] != '\0' && k + 1 < sizeof(target)) { target[k] = tok[k]; k++; }
			target[k] = '\0';
		}
	}

	if (use_fat)
	{
		char full_path[128];
		if (fat_resolve_path(target[0] == '\0' ? "." : target, full_path, sizeof(full_path)) != 0) { terminal_write_line("tree: invalid path"); return; }
		if (fat32_stat_path(full_path, &is_dir, &size) != 0) { terminal_write_line("tree: invalid path"); return; }
		terminal_write_line(full_path);
		if (!is_dir)
		{
			if (show_files) terminal_write_line("|- (file)");
			return;
		}
		tree_walk(full_path, 1, show_files, 1);
	}
	else
	{
		if (fs_stat(target[0] == '\0' ? (void *)0 : target, &is_dir, &size) != 0) { terminal_write_line("tree: invalid path"); return; }
		if (target[0] == '\0') terminal_write_line("."); else terminal_write_line(target);
		if (!is_dir)
		{
			if (show_files) terminal_write_line("|- (file)");
			return;
		}
		tree_walk(target, 0, show_files, 1);
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
		unsigned char data[FS_MAX_FILE_SIZE];
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

static int rm_path_join(char *out, unsigned long out_size, const char *base, const char *name)
{
	unsigned long n = 0;
	unsigned long i = 0;
	if (out == (void *)0 || out_size == 0 || base == (void *)0 || name == (void *)0) return -1;
	if (base[0] == '/' && base[1] == '\0')
	{
		if (out_size < 2) return -1;
		out[n++] = '/';
	}
	else
	{
		while (base[n] != '\0' && n + 1 < out_size)
		{
			out[n] = base[n];
			n++;
		}
		if (base[n] != '\0') return -1;
		if (n == 0 || out[n - 1] != '/')
		{
			if (n + 1 >= out_size) return -1;
			out[n++] = '/';
		}
	}
	while (name[i] != '\0' && name[i] != '/' && name[i] != '\\')
	{
		if (n + 1 >= out_size) return -1;
		out[n++] = name[i++];
	}
	out[n] = '\0';
	return (i == 0) ? -1 : 0;
}

static int rm_ramfs_recursive(const char *path, int recursive, int force)
{
	char names[FS_MAX_LIST][FS_NAME_MAX + 2];
	int types[FS_MAX_LIST];
	int count;
	int i;

	if (path == (void *)0 || path[0] == '\0') return force ? 0 : -1;
	if (path[0] == '/' && path[1] == '\0') return force ? 0 : -1;

	if (fs_ls(path, names, types, FS_MAX_LIST, &count) == 0)
	{
		if (!recursive) return force ? 0 : fs_rm(path);
		while (count > 0)
		{
			for (i = 0; i < count; i++)
			{
				char child_path[128];
				if (rm_path_join(child_path, sizeof(child_path), path, names[i]) != 0)
				{
					if (!force) return -1;
					continue;
				}
				if (rm_ramfs_recursive(child_path, 1, force) != 0 && !force) return -1;
			}
			if (fs_ls(path, names, types, FS_MAX_LIST, &count) != 0) break;
		}
	}
	if (fs_rm(path) != 0) return force ? 0 : -1;
	return 0;
}

static int rm_fat_recursive(const char *path, int recursive, int force)
{
	char names[64][40];
	int count;
	int i;

	if (path == (void *)0 || path[0] == '\0') return force ? 0 : -1;
	if (path[0] == '/' && path[1] == '\0') return force ? 0 : -1;

	if (fat32_ls_path(path, names, 64, &count) == 0)
	{
		if (!recursive) return force ? 0 : fat32_remove_path(path);
		while (count > 0)
		{
			for (i = 0; i < count; i++)
			{
				char child_path[128];
				if (rm_path_join(child_path, sizeof(child_path), path, names[i]) != 0)
				{
					if (!force) return -1;
					continue;
				}
				if (rm_fat_recursive(child_path, 1, force) != 0 && !force) return -1;
			}
			if (fat32_ls_path(path, names, 64, &count) != 0) break;
		}
	}
	if (fat32_remove_path(path) != 0) return force ? 0 : -1;
	return 0;
}

static const char *path_basename_part(const char *path)
{
	const char *base = path;
	if (path == (void *)0) return "";
	while (*path != '\0')
	{
		if (*path == '/' || *path == '\\') base = path + 1;
		path++;
	}
	return base;
}

static int ramfs_path_is_dir(const char *path)
{
	char names[FS_MAX_LIST][FS_NAME_MAX + 2];
	int types[FS_MAX_LIST];
	int count;
	return fs_ls(path, names, types, FS_MAX_LIST, &count) == 0;
}

static int fat_path_is_dir(const char *path)
{
	unsigned char attr;
	return fat32_get_attr_path(path, &attr) == 0 && (attr & 0x10) != 0;
}

static int ramfs_path_exists(const char *path)
{
	unsigned long size = 0;
	char names[FS_MAX_LIST][FS_NAME_MAX + 2];
	int types[FS_MAX_LIST];
	int count;
	if (path == (void *)0 || path[0] == '\0') return 0;
	if (fs_read_file(path, (void *)0, 0, &size) == 0) return 1;
	return fs_ls(path, names, types, FS_MAX_LIST, &count) == 0;
}

static int fat_path_exists(const char *path)
{
	unsigned char attr;
	if (path == (void *)0 || path[0] == '\0') return 0;
	return fat32_get_attr_path(path, &attr) == 0;
}

static void path_copy_text(char *dst, unsigned long dst_size, const char *src)
{
	unsigned long i = 0;
	if (dst == (void *)0 || dst_size == 0) return;
	while (src != (void *)0 && src[i] != '\0' && i + 1 < dst_size)
	{
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

static int prompt_overwrite(const char *op, const char *path)
{
	char answer[8];
	terminal_write(op);
	terminal_write(": overwrite ");
	terminal_write(path);
	terminal_write("? [y/N] ");
	if (terminal_read_line(answer, sizeof(answer)) != 0) return 0;
	return answer[0] == 'y' || answer[0] == 'Y';
}

static int resolve_copy_target_ramfs(const char *src, const char *dst, char *target, unsigned long target_size)
{
	if (ramfs_path_is_dir(dst)) return rm_path_join(target, target_size, dst, path_basename_part(src));
	path_copy_text(target, target_size, dst);
	return 0;
}

static int resolve_copy_target_fat(const char *src, const char *dst, char *target, unsigned long target_size)
{
	if (fat_path_is_dir(dst)) return rm_path_join(target, target_size, dst, path_basename_part(src));
	path_copy_text(target, target_size, dst);
	return 0;
}

static int cp_ramfs_recursive(const char *src, const char *dst, int recursive)
{
	char target[128];
	if (ramfs_path_is_dir(src))
	{
		char names[FS_MAX_LIST][FS_NAME_MAX + 2];
		int types[FS_MAX_LIST];
		int count;
		int i;
		if (!recursive) return -1;
		if (ramfs_path_is_dir(dst))
		{
			if (rm_path_join(target, sizeof(target), dst, path_basename_part(src)) != 0) return -1;
		}
		else
		{
			if (rm_path_join(target, sizeof(target), "/", dst) != 0 && dst[0] == '\0') return -1;
			{
				unsigned long j = 0;
				while (dst[j] != '\0' && j + 1 < sizeof(target)) { target[j] = dst[j]; j++; }
				target[j] = '\0';
			}
		}
		if (!ramfs_path_is_dir(target) && fs_mkdir(target) != 0) return -1;
		if (fs_ls(src, names, types, FS_MAX_LIST, &count) != 0) return -1;
		for (i = 0; i < count; i++)
		{
			char child_src[128];
			char child_dst[128];
			if (rm_path_join(child_src, sizeof(child_src), src, names[i]) != 0) return -1;
			if (rm_path_join(child_dst, sizeof(child_dst), target, names[i]) != 0) return -1;
			if (cp_ramfs_recursive(child_src, child_dst, 1) != 0) return -1;
		}
		return 0;
	}
	else
	{
		unsigned char data[FS_MAX_FILE_SIZE];
		unsigned long size;
		if (ramfs_path_is_dir(dst))
		{
			if (rm_path_join(target, sizeof(target), dst, path_basename_part(src)) != 0) return -1;
		}
		else
		{
			unsigned long j = 0;
			while (dst[j] != '\0' && j + 1 < sizeof(target)) { target[j] = dst[j]; j++; }
			target[j] = '\0';
		}
		if (fs_read_file(src, data, sizeof(data), &size) != 0) return -1;
		return fs_write_file(target, data, size);
	}
}

static int cp_fat_recursive(const char *src, const char *dst, int recursive)
{
	char target[128];
	if (fat_path_is_dir(src))
	{
		char names[64][40];
		int count;
		int i;
		if (!recursive) return -1;
		if (fat_path_is_dir(dst))
		{
			if (rm_path_join(target, sizeof(target), dst, path_basename_part(src)) != 0) return -1;
		}
		else
		{
			unsigned long j = 0;
			while (dst[j] != '\0' && j + 1 < sizeof(target)) { target[j] = dst[j]; j++; }
			target[j] = '\0';
		}
		if (!fat_path_is_dir(target) && fat32_mkdir_path(target) != 0) return -1;
		if (fat32_ls_path(src, names, 64, &count) != 0) return -1;
		for (i = 0; i < count; i++)
		{
			char child_src[128];
			char child_dst[128];
			if (rm_path_join(child_src, sizeof(child_src), src, names[i]) != 0) return -1;
			if (rm_path_join(child_dst, sizeof(child_dst), target, names[i]) != 0) return -1;
			if (cp_fat_recursive(child_src, child_dst, 1) != 0) return -1;
		}
		return 0;
	}
	else
	{
		unsigned char data[FS_MAX_FILE_SIZE];
		unsigned long size;
		if (fat_path_is_dir(dst))
		{
			if (rm_path_join(target, sizeof(target), dst, path_basename_part(src)) != 0) return -1;
		}
		else
		{
			unsigned long j = 0;
			while (dst[j] != '\0' && j + 1 < sizeof(target)) { target[j] = dst[j]; j++; }
			target[j] = '\0';
		}
		if (fat32_read_file_path(src, data, sizeof(data), &size) != 0) return -1;
		return fat32_write_file_path(target, data, size);
	}
}

static void cmd_rm(const char *args)
{
	int recursive = 0;
	int force = 0;
	char path[128];
	char full_path[128];
	char tok[32];
	const char *p = args;

	for (;;)
	{
		p = read_token(p, tok, sizeof(tok));
		if (p == (void *)0)
		{
			terminal_write_line("Usage: rm [-r] [-f] <path>");
			return;
		}
		if (tok[0] == '\0')
		{
			terminal_write_line("Usage: rm [-r] [-f] <path>");
			return;
		}
		if (tok[0] != '-') break;
		if (tok[1] == '\0')
		{
			terminal_write_line("Usage: rm [-r] [-f] <path>");
			return;
		}
		{
			unsigned long i = 1;
			while (tok[i] != '\0')
			{
				if (tok[i] == 'r') recursive = 1;
				else if (tok[i] == 'f') force = 1;
				else
				{
					terminal_write_line("Usage: rm [-r] [-f] <path>");
					return;
				}
				i++;
			}
		}
	}

	{
		unsigned long i = 0;
		while (tok[i] != '\0' && i + 1 < sizeof(path))
		{
			path[i] = tok[i];
			i++;
		}
		path[i] = '\0';
	}
	if (path[0] == '\0')
	{
		terminal_write_line("Usage: rm [-r] [-f] <path>");
		return;
	}
	if (fat_mode_active())
	{
		if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0)
		{
			if (!force) terminal_write_line("rm: failed");
			return;
		}
		if (rm_fat_recursive(full_path, recursive || force, force) != 0 && !force)
			terminal_write_line("rm: failed (use -r or -f for directories)");
		return;
	}
	if (rm_ramfs_recursive(path, recursive || force, force) != 0 && !force)
		terminal_write_line("rm: failed (use -r or -f for directories)");
}

static void cmd_cp(const char *args)
{
	int recursive = 0;
	int no_clobber = 0;
	int interactive = 0;
	char tok[32];
	char src[128];
	char dst[128];
	char target[128];
	char src_full[128];
	char dst_full[128];
	const char *p = args;

	for (;;)
	{
		p = read_token(p, tok, sizeof(tok));
		if (p == (void *)0)
		{
			terminal_write_line("Usage: cp [-r] [-n] [-i] <src> <dst>");
			return;
		}
		if (tok[0] == '\0')
		{
			terminal_write_line("Usage: cp [-r] [-n] [-i] <src> <dst>");
			return;
		}
		if (tok[0] != '-') break;
		{
			unsigned long i = 1;
			while (tok[i] != '\0')
			{
				if (tok[i] == 'r' || tok[i] == 'R') recursive = 1;
				else if (tok[i] == 'n') no_clobber = 1;
				else if (tok[i] == 'i') interactive = 1;
				else
				{
					terminal_write_line("Usage: cp [-r] [-n] [-i] <src> <dst>");
					return;
				}
				i++;
			}
		}
	}
	{
		unsigned long i = 0;
		while (tok[i] != '\0' && i + 1 < sizeof(src)) { src[i] = tok[i]; i++; }
		src[i] = '\0';
	}
	if (src[0] == '\0')
	{
		terminal_write_line("Usage: cp [-r] [-n] [-i] <src> <dst>");
		return;
	}
	p = read_token(p, dst, sizeof(dst));
	if (p == (void *)0 || dst[0] == '\0')
	{
		terminal_write_line("Usage: cp [-r] [-n] [-i] <src> <dst>");
		return;
	}
	if (fat_mode_active())
	{
		if (fat_resolve_path(src, src_full, sizeof(src_full)) != 0 || fat_resolve_path(dst, dst_full, sizeof(dst_full)) != 0)
		{
			terminal_write_line("cp: failed");
			return;
		}
		if (resolve_copy_target_fat(src_full, dst_full, target, sizeof(target)) != 0)
		{
			terminal_write_line("cp: failed");
			return;
		}
		if (fat_path_exists(target))
		{
			if (no_clobber) return;
			if (interactive && !prompt_overwrite("cp", target)) return;
			if (rm_fat_recursive(target, 1, 0) != 0)
			{
				terminal_write_line("cp: failed");
				return;
			}
		}
		if (cp_fat_recursive(src_full, dst_full, recursive) != 0)
		{
			terminal_write_line("cp: failed (use -r for directories)");
		}
		return;
	}
	if (resolve_copy_target_ramfs(src, dst, target, sizeof(target)) != 0)
	{
		terminal_write_line("cp: failed");
		return;
	}
	if (ramfs_path_exists(target))
	{
		if (no_clobber) return;
		if (interactive && !prompt_overwrite("cp", target)) return;
		if (rm_ramfs_recursive(target, 1, 0) != 0)
		{
			terminal_write_line("cp: failed");
			return;
		}
	}
	if (cp_ramfs_recursive(src, dst, recursive) != 0) terminal_write_line("cp: failed (use -r for directories)");
}

static void cmd_mv(const char *args)
{
	int no_clobber = 0;
	int interactive = 0;
	char tok[32];
	char src[128];
	char dst[128];
	char target[128];
	char src_full[128];
	char dst_full[128];
	const char *p = args;

	for (;;)
	{
		p = read_token(p, tok, sizeof(tok));
		if (p == (void *)0)
		{
			terminal_write_line("Usage: mv [-n] [-i] <src> <dst>");
			return;
		}
		if (tok[0] == '\0')
		{
			terminal_write_line("Usage: mv [-n] [-i] <src> <dst>");
			return;
		}
		if (tok[0] != '-') break;
		{
			unsigned long i = 1;
			while (tok[i] != '\0')
			{
				if (tok[i] == 'n') no_clobber = 1;
				else if (tok[i] == 'i') interactive = 1;
				else
				{
					terminal_write_line("Usage: mv [-n] [-i] <src> <dst>");
					return;
				}
				i++;
			}
		}
	}
	path_copy_text(src, sizeof(src), tok);
	if (src[0] == '\0')
	{
		terminal_write_line("Usage: mv [-n] [-i] <src> <dst>");
		return;
	}
	p = read_token(p, dst, sizeof(dst));
	if (p == (void *)0 || dst[0] == '\0')
	{
		terminal_write_line("Usage: mv [-n] [-i] <src> <dst>");
		return;
	}
	if (fat_mode_active())
	{
		if (fat_resolve_path(src, src_full, sizeof(src_full)) != 0 || fat_resolve_path(dst, dst_full, sizeof(dst_full)) != 0)
		{
			terminal_write_line("mv: failed");
			return;
		}
		if (fat_path_is_dir(dst_full))
		{
			if (rm_path_join(target, sizeof(target), dst_full, path_basename_part(src_full)) != 0)
			{
				terminal_write_line("mv: failed");
				return;
			}
		}
		else
		{
			unsigned long i = 0;
			while (dst_full[i] != '\0' && i + 1 < sizeof(target)) { target[i] = dst_full[i]; i++; }
			target[i] = '\0';
		}
		if (fat_path_exists(target))
		{
			if (no_clobber) return;
			if (interactive && !prompt_overwrite("mv", target)) return;
			if (rm_fat_recursive(target, 1, 0) != 0)
			{
				terminal_write_line("mv: failed");
				return;
			}
		}
		if (cp_fat_recursive(src_full, target, 1) != 0 || rm_fat_recursive(src_full, 1, 0) != 0)
		{
			terminal_write_line("mv: failed");
		}
		return;
	}
	if (ramfs_path_is_dir(dst))
	{
		if (rm_path_join(target, sizeof(target), dst, path_basename_part(src)) != 0)
		{
			terminal_write_line("mv: failed");
			return;
		}
		if (ramfs_path_exists(target))
		{
			if (no_clobber) return;
			if (interactive && !prompt_overwrite("mv", target)) return;
			if (rm_ramfs_recursive(target, 1, 0) != 0)
			{
				terminal_write_line("mv: failed");
				return;
			}
		}
		if (fs_mv(src, target) != 0) terminal_write_line("mv: failed");
		return;
	}
	if (ramfs_path_exists(dst))
	{
		if (no_clobber) return;
		if (interactive && !prompt_overwrite("mv", dst)) return;
		if (rm_ramfs_recursive(dst, 1, 0) != 0)
		{
			terminal_write_line("mv: failed");
			return;
		}
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
	if (editor_find_active)
	{
		terminal_write("Find: ");
		terminal_write(editor_find_query);
		terminal_write_line("_  Enter=next  Shift+Enter=prev  Esc=cancel");
	}
	else if (editor_hex_mode) terminal_write_line("Hex: 0-9/A-F edit | arrows PgUp PgDn Home End | Ctrl+Home/End");
	else terminal_write_line("Ctrl+S save | Ctrl+F find | Ctrl+C/X/V copy cut paste | Shift+arrows select");
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
	editor_find_active = 0;
	editor_find_query_length = 0;
	editor_find_query[0] = '\0';
	editor_find_invalidate_match();
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

	editor_capture_screen();
	screen_clear();
	editor_cursor = editor_length;
	editor_clear_selection();
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

static void editor_capture_screen(void)
{
	unsigned long i;
	if (editor_screen_saved) return;
	editor_screen_saved_width = screen_get_width();
	editor_screen_saved_height = screen_get_height();
	editor_screen_saved_cells = editor_screen_saved_width * editor_screen_saved_height;
	if (editor_screen_saved_cells > EDITOR_SCREEN_SNAPSHOT_MAX_CELLS)
		editor_screen_saved_cells = EDITOR_SCREEN_SNAPSHOT_MAX_CELLS;
	editor_screen_saved_pos = screen_get_pos();
	editor_screen_saved_color = screen_get_color();
	editor_screen_saved_style = screen_get_style();
	for (i = 0; i < editor_screen_saved_cells; i++)
		screen_read_cell_at((unsigned short)i, &editor_screen_saved_chars[i], &editor_screen_saved_attrs[i], &editor_screen_saved_styles[i]);
	editor_screen_saved = 1;
}

static void editor_restore_screen(void)
{
	unsigned long i;
	if (!editor_screen_saved) return;
	for (i = 0; i < editor_screen_saved_cells; i++)
	{
		screen_write_cell_at((unsigned short)i, editor_screen_saved_chars[i], editor_screen_saved_attrs[i], editor_screen_saved_styles[i]);
	}
	screen_set_color(editor_screen_saved_color);
	screen_set_style(editor_screen_saved_style);
	screen_set_pos(editor_screen_saved_pos);
	screen_set_hw_cursor(editor_screen_saved_pos);
	editor_screen_saved = 0;
}

static void editor_close(int saved, int save_attempted)
{
	editor_active = 0;
	editor_find_active = 0;
	editor_restore_screen();
	input_length = 0;
	cursor_pos = 0;
	input_buffer[0] = '\0';
	if (save_attempted)
	{
		if (saved) terminal_write_line("[editor] saved");
		else terminal_write_line("[editor] save failed");
	}
	else
	{
		terminal_write_line("[editor] canceled");
	}
	terminal_prompt();
}

static void editor_find_invalidate_match(void)
{
	editor_find_match_valid = 0;
	editor_find_match_start = 0;
	editor_find_match_end = 0;
}

static void editor_find_open(void)
{
	if (editor_find_query_length == 0 && editor_find_last_query_length > 0)
	{
		unsigned long i;
		editor_find_query_length = editor_find_last_query_length;
		for (i = 0; i < editor_find_query_length; i++) editor_find_query[i] = editor_find_last_query[i];
		editor_find_query[editor_find_query_length] = '\0';
	}
	editor_find_active = 1;
	editor_render();
}

static void editor_find_cancel(void)
{
	editor_find_active = 0;
	editor_render();
}

static int editor_find_match_at(unsigned long index)
{
	unsigned long i;
	if (editor_find_query_length == 0) return 0;
	if (index + editor_find_query_length > editor_length) return 0;
	for (i = 0; i < editor_find_query_length; i++)
	{
		if (ascii_upper(editor_buffer[index + i]) != ascii_upper(editor_find_query[i])) return 0;
	}
	return 1;
}

static int editor_find_next(int advance_from_current)
{
	unsigned long start;
	unsigned long i;
	unsigned long k;
	if (editor_find_query_length == 0)
	{
		editor_status_line("[editor] find: empty query");
		return 0;
	}
	editor_find_last_query_length = editor_find_query_length;
	for (k = 0; k < editor_find_query_length; k++) editor_find_last_query[k] = editor_find_query[k];
	editor_find_last_query[editor_find_last_query_length] = '\0';
	start = editor_cursor;
	if (advance_from_current && editor_find_match_valid && editor_find_match_end > start) start = editor_find_match_end;
	for (i = start; i + editor_find_query_length <= editor_length; i++)
	{
		if (editor_find_match_at(i))
		{
			editor_find_match_valid = 1;
			editor_find_match_start = i;
			editor_find_match_end = i + editor_find_query_length;
			editor_cursor = i;
			editor_render();
			return 1;
		}
	}
	for (i = 0; i < start && i + editor_find_query_length <= editor_length; i++)
	{
		if (editor_find_match_at(i))
		{
			editor_find_match_valid = 1;
			editor_find_match_start = i;
			editor_find_match_end = i + editor_find_query_length;
			editor_cursor = i;
			editor_render();
			return 1;
		}
	}
	editor_find_invalidate_match();
	editor_status_line("[editor] find: no match");
	return 0;
}

static int editor_find_prev(void)
{
	long i;
	unsigned long start;
	unsigned long k;
	if (editor_find_query_length == 0)
	{
		editor_status_line("[editor] find: empty query");
		return 0;
	}
	editor_find_last_query_length = editor_find_query_length;
	for (k = 0; k < editor_find_query_length; k++) editor_find_last_query[k] = editor_find_query[k];
	editor_find_last_query[editor_find_last_query_length] = '\0';
	if (editor_cursor == 0) start = editor_length;
	else start = editor_cursor;
	for (i = (long)start - 1; i >= 0; i--)
	{
		if (editor_find_match_at((unsigned long)i))
		{
			editor_find_match_valid = 1;
			editor_find_match_start = (unsigned long)i;
			editor_find_match_end = (unsigned long)i + editor_find_query_length;
			editor_cursor = (unsigned long)i;
			editor_render();
			return 1;
		}
	}
	for (i = (long)editor_length - 1; i >= (long)start; i--)
	{
		if (editor_find_match_at((unsigned long)i))
		{
			editor_find_match_valid = 1;
			editor_find_match_start = (unsigned long)i;
			editor_find_match_end = (unsigned long)i + editor_find_query_length;
			editor_cursor = (unsigned long)i;
			editor_render();
			return 1;
		}
	}
	editor_find_invalidate_match();
	editor_status_line("[editor] find: no match");
	return 0;
}

static int parse_display_mode_spec(const char *spec, unsigned int *width, unsigned int *height, unsigned int *bpp)
{
	char wbuf[16];
	char hbuf[16];
	char bbuf[16];
	unsigned long i = 0;
	unsigned long j = 0;
	if (spec == (void *)0 || width == (void *)0 || height == (void *)0 || bpp == (void *)0) return 0;
	while (spec[i] != '\0' && spec[i] != 'x' && spec[i] != 'X' && i + 1 < sizeof(wbuf))
	{
		wbuf[i] = spec[i];
		i++;
	}
	if (spec[i] == '\0' || i == 0 || i + 1 >= sizeof(wbuf)) return 0;
	wbuf[i] = '\0';
	i++;
	while (spec[i] != '\0' && spec[i] != 'x' && spec[i] != 'X' && j + 1 < sizeof(hbuf))
	{
		hbuf[j++] = spec[i++];
	}
	if (j == 0 || j + 1 >= sizeof(hbuf)) return 0;
	hbuf[j] = '\0';
	if (parse_dec_u32(wbuf, width) != 0 || parse_dec_u32(hbuf, height) != 0) return 0;
	*bpp = 32;
	if (spec[i] == '\0') return 1;
	i++;
	j = 0;
	while (spec[i] != '\0' && j + 1 < sizeof(bbuf)) bbuf[j++] = spec[i++];
	if (spec[i] != '\0' || j == 0) return 0;
	bbuf[j] = '\0';
	if (parse_dec_u32(bbuf, bpp) != 0) return 0;
	return (*bpp == 24 || *bpp == 32) ? 1 : 0;
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

static int terminal_try_mount_boot_fat(void)
{
	struct block_device *dev;
	if (fat32_is_mounted()) return 1;
	dev = blockdev_get_primary();
	if (dev != (void *)0 && dev->present && fat32_mount(dev) == 0) return 1;
	dev = blockdev_get_secondary();
	if (dev != (void *)0 && dev->present && fat32_mount(dev) == 0) return 1;
	return 0;
}

static void terminal_auto_fatmount(void)
{
	if (!terminal_try_mount_boot_fat()) return;
	vfs_prefer_fat_root = 1;
	fat_cwd[0] = '/';
	fat_cwd[1] = '\0';
}

static int terminal_write_fat_text(const char *path, const char *text)
{
	unsigned long len = string_length(text);
	if (fat32_write_file_path(path, (const unsigned char *)text, len) == 0) return 0;
	if (fat32_touch_file_path(path) != 0) return -1;
	return fat32_write_file_path(path, (const unsigned char *)text, len);
}

static int terminal_get_autorun_mode(void)
{
	const char *text;
	unsigned char fat_buf[32];
	unsigned long fat_size = 0;

	if (terminal_try_mount_boot_fat() &&
		fat32_read_file_path(FAT_AUTORUN_MODE_PATH, fat_buf, sizeof(fat_buf) - 1, &fat_size) == 0)
	{
		fat_buf[fat_size] = '\0';
		text = (const char *)fat_buf;
	}
	else if (fs_read_text(AUTORUN_MODE_PATH, &text) != 0)
	{
		return 1;
	}

	if (string_starts_with(text, "off")) return 0;
	if (string_starts_with(text, "once")) return 2;
	return 1;
}

static void terminal_set_autorun_mode(int mode)
{
	const char *text;
	if (mode == 0) text = "off\n";
	else if (mode == 2) text = "once\n";
	else text = "always\n";
	fs_write_text(AUTORUN_MODE_PATH, text);
	if (terminal_try_mount_boot_fat()) terminal_write_fat_text(FAT_AUTORUN_MODE_PATH, text);
}

static int terminal_autorun_once_done(void)
{
	const char *text;
	unsigned char fat_buf[8];
	unsigned long fat_size = 0;

	if (terminal_try_mount_boot_fat() &&
		fat32_read_file_path(FAT_AUTORUN_ONCE_STATE_PATH, fat_buf, sizeof(fat_buf) - 1, &fat_size) == 0)
	{
		fat_buf[fat_size] = '\0';
		text = (const char *)fat_buf;
	}
	else if (fs_read_text(AUTORUN_ONCE_STATE_PATH, &text) != 0)
	{
		return 0;
	}

	return (text[0] == '1') ? 1 : 0;
}

static void terminal_set_autorun_once_done(int done)
{
	const char *text = done ? "1\n" : "0\n";
	fs_write_text(AUTORUN_ONCE_STATE_PATH, text);
	if (terminal_try_mount_boot_fat()) terminal_write_fat_text(FAT_AUTORUN_ONCE_STATE_PATH, text);
}

static unsigned long terminal_get_autorun_delay_seconds(void)
{
	const char *text;
	char token[16];
	unsigned int seconds = (unsigned int)AUTORUN_DEFAULT_DELAY_SECONDS;
	unsigned char fat_buf[32];
	unsigned long fat_size = 0;

	if (terminal_try_mount_boot_fat() &&
		fat32_read_file_path(FAT_AUTORUN_DELAY_PATH, fat_buf, sizeof(fat_buf) - 1, &fat_size) == 0)
	{
		fat_buf[fat_size] = '\0';
		if (read_token((const char *)fat_buf, token, sizeof(token)) != (void *)0 && token[0] != '\0' &&
			parse_dec_u32(token, &seconds) == 0)
		{
			return (unsigned long)seconds;
		}
	}

	if (fs_read_text(AUTORUN_DELAY_PATH, &text) == 0 &&
		read_token(text, token, sizeof(token)) != (void *)0 && token[0] != '\0' &&
		parse_dec_u32(token, &seconds) == 0)
	{
		return (unsigned long)seconds;
	}

	return AUTORUN_DEFAULT_DELAY_SECONDS;
}

static void terminal_set_autorun_delay_seconds(unsigned long seconds)
{
	char value[16];
	uint_to_dec(seconds, value, sizeof(value));
	fs_write_text(AUTORUN_DELAY_PATH, value);
	if (terminal_try_mount_boot_fat()) terminal_write_fat_text(FAT_AUTORUN_DELAY_PATH, value);
}

static int terminal_run_autorun_script_now(void)
{
	const char *text;
	int old_fat_mode;

	if (terminal_try_mount_boot_fat())
	{
		unsigned char probe[2];
		unsigned long probe_size = 0;
		if (fat32_read_file_path(FAT_AUTORUN_PATH, probe, sizeof(probe), &probe_size) == 0)
		{
			old_fat_mode = vfs_prefer_fat_root;
			vfs_prefer_fat_root = 1;
			cmd_run(FAT_AUTORUN_PATH);
			vfs_prefer_fat_root = old_fat_mode;
			return 1;
		}
	}

	if (fs_read_text(AUTORUN_PATH, &text) != 0) return 0;
	if (text[0] == '\0') return 0;
	cmd_run(AUTORUN_PATH);
	return 1;
}

static int terminal_autorun_should_run_on_boot(void)
{
	int mode = terminal_get_autorun_mode();
	if (mode == 0) return 0;
	if (mode == 2 && terminal_autorun_once_done()) return 0;
	return 1;
}

static void terminal_schedule_boot_autorun(void)
{
	unsigned long delay_seconds;
	unsigned long delay_ticks;
	char n[16];

	if (!terminal_autorun_should_run_on_boot())
	{
		autorun_boot_pending = 0;
		return;
	}
	delay_seconds = terminal_get_autorun_delay_seconds();
	delay_ticks = delay_seconds * 100UL;
	autorun_boot_pending = 1;
	autorun_boot_deadline = timer_ticks() + delay_ticks;
	terminal_write("autorun: scheduled in ");
	uint_to_dec(delay_seconds, n, sizeof(n));
	terminal_write(n);
	terminal_write_line("s (type 'autorun stop' to skip this boot)");
}

static void terminal_poll_boot_autorun(void)
{
	int mode;
	int ran;
	if (!autorun_boot_pending) return;
	if (timer_ticks() < autorun_boot_deadline) return;
	if (editor_active || script_mode_active || terminal_capture_mode) return;
	if (input_length != 0) return;

	autorun_boot_pending = 0;
	mode = terminal_get_autorun_mode();
	ran = terminal_run_autorun_script_now();
	if (!ran)
	{
		terminal_write_line("autorun: no script found");
		return;
	}
	if (mode == 2) terminal_set_autorun_once_done(1);
}

static void terminal_print_motd(void)
{
	const char *text;
	unsigned char fat_buf[1024];
	unsigned long fat_size = 0;
	unsigned long i = 0;

	if (terminal_try_mount_boot_fat() &&
		fat32_read_file_path("/motd.txt", fat_buf, sizeof(fat_buf) - 1, &fat_size) == 0)
	{
		fat_buf[fat_size] = '\0';
		text = (const char *)fat_buf;
	}
	else
	{
	if (fs_read_text(MOTD_PATH, &text) != 0) return;
	}
	if (text[0] == '\0') return;

	while (text[i] != '\0')
	{
		char line[INPUT_BUFFER_SIZE];
		unsigned long n = 0;
		while (text[i] != '\0' && text[i] != '\n' && n + 1 < sizeof(line))
		{
			if (text[i] != '\r') line[n++] = text[i];
			i++;
		}
		line[n] = '\0';
		terminal_write_echo_text(line);
		terminal_putc('\n');
		if (text[i] == '\n') i++;
	}
}

static void terminal_run_boot_autorun(void)
{
	terminal_schedule_boot_autorun();
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
	int i;
	int any = 0;
	for (i = 0; i < 2; i++)
	{
		if (ata_is_present_drive(i))
		{
			terminal_write("ATA drive ");
			terminal_putc((char)('0' + i));
			terminal_write(" (");
			terminal_write(i == 0 ? "primary master" : "primary slave ");
			terminal_write("): sectors=");
			terminal_write_hex64((unsigned long)ata_get_sector_count_drive(i));
			terminal_putc('\n');
			any = 1;
		}
	}
	if (!any) terminal_write_line("ATA: no devices detected");
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

static void cmd_fatmount(const char *args)
{
	int drive_index = 0;
	struct block_device *dev;

	/* Optional argument: "0" = master, "1" = slave */
	if (args != (void *)0 && args[0] == '1') drive_index = 1;

	dev = blockdev_get(drive_index);
	if (dev == (void *)0 || !dev->present)
	{
		terminal_write("fatmount: drive ");
		terminal_putc((char)('0' + drive_index));
		terminal_write_line(" not present");
		return;
	}

	if (fat32_mount(dev) != 0)
	{
		terminal_write("fatmount: drive ");
		terminal_putc((char)('0' + drive_index));
		terminal_write_line(" mount failed (not FAT32?)");
		return;
	}

	vfs_prefer_fat_root = 1;
	fat_cwd[0] = '/';
	fat_cwd[1] = '\0';

	terminal_write("fatmount: drive ");
	terminal_putc((char)('0' + drive_index));
	terminal_write_line(" mounted (generic fs commands now use FAT)");
}

static void cmd_drives(void)
{
	int i;
	char num[16];
	for (i = 0; i < 2; i++)
	{
		struct block_device *dev = blockdev_get(i);
		terminal_write("  drive ");
		terminal_putc((char)('0' + i));
		terminal_write(" (");
		terminal_write(i == 0 ? "primary master" : "primary slave ");
		terminal_write("): ");
		if (dev != (void *)0 && dev->present)
		{
			uint_to_dec((unsigned long)dev->sector_count, num, sizeof(num));
			terminal_write(num);
			terminal_write_line(" sectors");
		}
		else
		{
			terminal_write_line("not detected");
		}
	}
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

	if (string_equals(what, "preview"))
	{
		unsigned char saved = terminal_text_color;
		char which[16];
		p = read_token(p, which, sizeof(which));
		if (p == (void *)0 || which[0] == '\0')
		{
			unsigned char bg;
			unsigned char fg;
			static const char hex[] = "0123456789ABCDEF";
			terminal_write_line("color preview: fg across, bg down");
			terminal_write("    ");
			for (fg = 0; fg < 16; fg++)
			{
				terminal_putc(hex[fg]);
				terminal_write("  ");
			}
			terminal_putc('\n');
			for (bg = 0; bg < 16; bg++)
			{
				screen_set_color(saved);
				terminal_putc(hex[bg]);
				terminal_write(" : ");
				for (fg = 0; fg < 16; fg++)
				{
					screen_set_color((unsigned char)((bg << 4) | fg));
					terminal_write("Aa ");
				}
				screen_set_color(saved);
				terminal_putc('\n');
			}
			screen_set_color(saved);
			return;
		}
		if (string_equals(which, "text"))
		{
			screen_set_color(terminal_text_color);
			terminal_write("text ");
			terminal_write_hex8(terminal_text_color);
			terminal_write_line(": The quick brown fox jumps over 1234567890");
			screen_set_color(saved);
			return;
		}
		if (string_equals(which, "prompt"))
		{
			screen_set_color(terminal_prompt_color);
			terminal_write("prompt ");
			terminal_write_hex8(terminal_prompt_color);
			terminal_write_line(": > preview prompt sample");
			screen_set_color(saved);
			return;
		}
		terminal_write_line("Usage: color [show|preview [text|prompt]|text <0xNN>|prompt <0xNN>]");
		return;
	}

	p = read_token(p, value, sizeof(value));
	if (p == (void *)0 || value[0] == '\0' || parse_color_token(value, &c) != 0)
	{
		terminal_write_line("Usage: color [show|preview [text|prompt]|text <0xNN>|prompt <0xNN>]");
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

	terminal_write_line("Usage: color [show|preview [text|prompt]|text <0xNN>|prompt <0xNN>]");
}

static void cmd_serial(const char *args)
{
	char op[16];
	char arg2[16];
	const char *p = read_token(args, op, sizeof(op));

	if (!serial_ready)
	{
		terminal_write_line("serial: COM1 not available");
		return;
	}

	if (p == (void *)0 || op[0] == '\0' || string_equals(op, "show"))
	{
		terminal_write("serial: mirror=");
		terminal_write(serial_mirror_enabled ? "on" : "off");
		terminal_write(" compact=");
		terminal_write(serial_compact_enabled ? "on" : "off");
		terminal_write(" rxecho=");
		terminal_write_line(serial_rxecho_enabled ? "on" : "off");
		return;
	}

	if (string_equals(op, "on"))
	{
		serial_mirror_enabled = 1;
		terminal_write_line("serial: mirror enabled");
		return;
	}
	if (string_equals(op, "off"))
	{
		serial_mirror_enabled = 0;
		terminal_write_line("serial: mirror disabled");
		return;
	}
	if (string_equals(op, "compact"))
	{
		p = read_token(p, arg2, sizeof(arg2));
		if (p == (void *)0 || arg2[0] == '\0')
		{
			terminal_write_line("Usage: serial compact <on|off>");
			return;
		}
		if (string_equals(arg2, "on"))
		{
			serial_compact_enabled = 1;
			terminal_write_line("serial: compact mode enabled");
			return;
		}
		if (string_equals(arg2, "off"))
		{
			serial_compact_enabled = 0;
			terminal_write_line("serial: compact mode disabled");
			return;
		}
		terminal_write_line("Usage: serial compact <on|off>");
		return;
	}
	if (string_equals(op, "rxecho"))
	{
		p = read_token(p, arg2, sizeof(arg2));
		if (p == (void *)0 || arg2[0] == '\0')
		{
			terminal_write_line("Usage: serial rxecho <on|off>");
			return;
		}
		if (string_equals(arg2, "on"))
		{
			serial_rxecho_enabled = 1;
			terminal_write_line("serial: rx echo enabled");
			return;
		}
		if (string_equals(arg2, "off"))
		{
			serial_rxecho_enabled = 0;
			terminal_write_line("serial: rx echo disabled");
			return;
		}
		terminal_write_line("Usage: serial rxecho <on|off>");
		return;
	}

	terminal_write_line("Usage: serial [on|off|show|compact <on|off>|rxecho <on|off>]");
}

static void cmd_display(const char *args)
{
	char op[16];
	char arg2[16];
	const char *p = read_token(args, op, sizeof(op));

	if (p == (void *)0 || op[0] == '\0' || string_equals(op, "show"))
	{
		terminal_write("display: mode=");
		if (display_mode == 0) terminal_write("vga25");
		else if (display_mode == 1) terminal_write("vga50");
		else terminal_write("fb");
		terminal_write(" term=");
		{
			char n[16];
			uint_to_dec((unsigned long)screen_get_width(), n, sizeof(n)); terminal_write(n);
			terminal_write("x");
			uint_to_dec((unsigned long)screen_get_height(), n, sizeof(n)); terminal_write(n);
		}
		if (framebuffer_available())
		{
			char n[16];
			terminal_write(" fb=");
			uint_to_dec((unsigned long)framebuffer_width(), n, sizeof(n)); terminal_write(n);
			terminal_write("x");
			uint_to_dec((unsigned long)framebuffer_height(), n, sizeof(n)); terminal_write(n);
			terminal_write("x");
			uint_to_dec((unsigned long)framebuffer_bpp(), n, sizeof(n)); terminal_write(n);
			terminal_write(" src=");
			if (framebuffer_mode_source() == 1) terminal_write("kernel-vbe");
			else terminal_write("bootloader");
		}
		else
		{
			terminal_write(" fb=none");
		}
		terminal_write(" cursor=");
		if (screen_get_cursor_style() == 1) terminal_write("block");
		else if (screen_get_cursor_style() == 2) terminal_write("bar");
		else terminal_write("underline");
		terminal_putc('\n');
		return;
	}

	if (string_equals(op, "mode"))
	{
		unsigned int w = 0, h = 0, bpp = 32;
		p = read_token(p, arg2, sizeof(arg2));
		if (p == (void *)0 || arg2[0] == '\0' || string_equals(arg2, "show"))
		{
			terminal_write_line("display mode presets: 1080p, 900p, 768p, 720p, list, or WIDTHxHEIGHT[xBPP]");
			if (framebuffer_available())
			{
				char n[16];
				terminal_write("display: current fb mode ");
				uint_to_dec((unsigned long)framebuffer_width(), n, sizeof(n)); terminal_write(n);
				terminal_write("x");
				uint_to_dec((unsigned long)framebuffer_height(), n, sizeof(n)); terminal_write(n);
				terminal_write("x");
				uint_to_dec((unsigned long)framebuffer_bpp(), n, sizeof(n)); terminal_write(n);
				terminal_putc('\n');
			}
			return;
		}
		if (string_equals(arg2, "list"))
		{
			unsigned int old_w, old_h, old_bpp;
			unsigned long i;
			struct mode_item { unsigned int w, h, bpp; const char *name; };
			static const struct mode_item candidates[] = {
				{1920,1080,32,"1080p"}, {1600,900,32,"900p"}, {1360,768,32,"768p"},
				{1280,720,32,"720p"}, {1024,768,32,"1024x768"}, {800,600,32,"800x600"}
			};
			if (!framebuffer_available())
			{
				terminal_write_line("display: framebuffer not available from bootloader");
				return;
			}
			old_w = framebuffer_width();
			old_h = framebuffer_height();
			old_bpp = framebuffer_bpp();
			terminal_write_line("display: probing common modes...");
			for (i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++)
			{
				terminal_write("  ");
				terminal_write(candidates[i].name);
				terminal_write(" : ");
				if (framebuffer_try_set_mode(candidates[i].w, candidates[i].h, candidates[i].bpp)) terminal_write_line("supported");
				else terminal_write_line("unsupported");
			}
			framebuffer_try_set_mode(old_w, old_h, old_bpp);
			if (display_mode == 2) screen_set_framebuffer_text_mode();
			return;
		}
		if (string_equals(arg2, "1080p")) { w = 1920; h = 1080; }
		else if (string_equals(arg2, "900p")) { w = 1600; h = 900; }
		else if (string_equals(arg2, "768p")) { w = 1360; h = 768; }
		else if (string_equals(arg2, "720p")) { w = 1280; h = 720; }
		else if (!parse_display_mode_spec(arg2, &w, &h, &bpp))
		{
			terminal_write_line("Usage: display mode <show|list|1080p|900p|768p|720p|WIDTHxHEIGHT[xBPP]>");
			return;
		}

		if (!framebuffer_available())
		{
			terminal_write_line("display: framebuffer not available from bootloader");
			return;
		}
		if (!framebuffer_try_set_mode(w, h, bpp))
		{
			terminal_write_line("display: requested mode not supported by current adapter");
			return;
		}
		if (!screen_set_framebuffer_text_mode())
		{
			terminal_write_line("display: mode changed but framebuffer text backend failed");
			return;
		}
		display_mode = 2;
		terminal_write("display: switched framebuffer mode to ");
		{
			char n[16];
			uint_to_dec((unsigned long)w, n, sizeof(n)); terminal_write(n);
			terminal_write("x");
			uint_to_dec((unsigned long)h, n, sizeof(n)); terminal_write(n);
			terminal_write("x");
			uint_to_dec((unsigned long)bpp, n, sizeof(n)); terminal_write_line(n);
		}
		return;
	}

	if (string_equals(op, "cursor"))
	{
		p = read_token(p, arg2, sizeof(arg2));
		if (p == (void *)0 || arg2[0] == '\0' || string_equals(arg2, "show"))
		{
			terminal_write("display: cursor=");
			if (screen_get_cursor_style() == 1) terminal_write_line("block");
			else if (screen_get_cursor_style() == 2) terminal_write_line("bar");
			else terminal_write_line("underline");
			return;
		}
		if (string_equals(arg2, "underline")) { screen_set_cursor_style(0); terminal_write_line("display: cursor set to underline"); return; }
		if (string_equals(arg2, "block")) { screen_set_cursor_style(1); terminal_write_line("display: cursor set to block"); return; }
		if (string_equals(arg2, "bar")) { screen_set_cursor_style(2); terminal_write_line("display: cursor set to bar"); return; }
		terminal_write_line("Usage: display cursor <show|underline|block|bar>");
		return;
	}

	if (string_equals(op, "vga25"))
	{
		screen_set_text_mode_80x25();
		display_mode = 0;
		terminal_write_line("display: switched to VGA text 80x25");
		return;
	}
	if (string_equals(op, "vga50"))
	{
		screen_set_text_mode_80x50();
		display_mode = 1;
		terminal_write_line("display: switched to VGA text 80x50");
		return;
	}
	if (string_equals(op, "fb"))
	{
		if (!framebuffer_available())
		{
			terminal_write_line("display: framebuffer not available from bootloader");
			return;
		}
		if (!screen_set_framebuffer_text_mode())
		{
			terminal_write_line("display: framebuffer mode unsupported by current boot framebuffer");
			return;
		}
		display_mode = 2;
		terminal_write_line("display: switched to framebuffer text mode");
		return;
	}

	terminal_write_line("Usage: display [show|vga25|vga50|fb|mode <show|list|1080p|900p|768p|720p|WIDTHxHEIGHT[xBPP]>|cursor <show|underline|block|bar>]");
}

static void cmd_fbfont(const char *args)
{
	char op[16];
	char tok[16];
	char chs[8];
	char name[32];
	char path[64];
	char out[EDITOR_BUFFER_SIZE];
	char line[128];
	char names[FS_MAX_LIST][FS_NAME_MAX + 2];
	int types[FS_MAX_LIST];
	int count = 0;
	int i_count;
	unsigned long out_len = 0;
	unsigned char rows[7];
	unsigned char v;
	unsigned long i;
	const char *text;
	const char *p = read_token(args, op, sizeof(op));

	#define APPEND_CH(C) do { if (out_len + 1 >= sizeof(out)) { terminal_write_line("fbfont: profile too large"); return; } out[out_len++] = (C); } while (0)
	#define APPEND_STR(S) do { const char *q_ = (S); while (*q_ != '\0') { APPEND_CH(*q_); q_++; } } while (0)
	#define APPEND_HEX5(B) do { unsigned char b_ = (unsigned char)((B) & 0x1F); APPEND_CH('0'); APPEND_CH('x'); APPEND_CH('0'); APPEND_CH((char)(b_ < 10 ? ('0' + b_) : ('A' + (b_ - 10)))); } while (0)

	if (p == (void *)0 || op[0] == '\0' || string_equals(op, "show"))
	{
		terminal_write("fbfont: style=");
		terminal_write(screen_fbfont_get_style() == 1 ? "blocky" : "classic");
		terminal_write(" size=");
		if (screen_fbfont_get_size() == 12) terminal_write("small");
		else if (screen_fbfont_get_size() == 16) terminal_write("large");
		else terminal_write("normal");
		terminal_write(" custom-dir=");
		terminal_write_line(FBFONT_DIR);
		return;
	}

	if (string_equals(op, "list"))
	{
		if (fs_ls(FBFONT_DIR, names, types, FS_MAX_LIST, &count) != 0)
		{
			terminal_write_line("fbfont: no profiles");
			return;
		}
		terminal_write_line("fbfont profiles:");
		for (i_count = 0; i_count < count; i_count++)
		{
			if (types[i_count] != 0) continue;
			if (!editor_path_has_ext_ci(names[i_count], ".fbf")) continue;
			terminal_write("  ");
			terminal_write_line(names[i_count]);
		}
		return;
	}

	if (string_equals(op, "style"))
	{
		p = read_token(p, tok, sizeof(tok));
		if (p == (void *)0 || tok[0] == '\0')
		{
			terminal_write_line("Usage: fbfont style <classic|blocky>");
			return;
		}
		if (string_equals(tok, "classic"))
		{
			screen_fbfont_set_style(0);
			terminal_write_line("fbfont: style set to classic");
			return;
		}
		if (string_equals(tok, "blocky"))
		{
			screen_fbfont_set_style(1);
			terminal_write_line("fbfont: style set to blocky");
			return;
		}
		terminal_write_line("Usage: fbfont style <classic|blocky>");
		return;
	}

	if (string_equals(op, "reset"))
	{
		screen_fbfont_reset_custom();
		terminal_write_line("fbfont: custom glyphs reset");
		return;
	}

	if (string_equals(op, "size"))
	{
		p = read_token(p, tok, sizeof(tok));
		if (p == (void *)0 || tok[0] == '\0')
		{
			terminal_write_line("Usage: fbfont size <small|normal|large>");
			return;
		}
		if (string_equals(tok, "small"))
		{
			screen_fbfont_set_size(12);
			terminal_write_line("fbfont: size set to small");
			return;
		}
		if (string_equals(tok, "normal"))
		{
			screen_fbfont_set_size(14);
			terminal_write_line("fbfont: size set to normal");
			return;
		}
		if (string_equals(tok, "large"))
		{
			screen_fbfont_set_size(16);
			terminal_write_line("fbfont: size set to large");
			return;
		}
		terminal_write_line("Usage: fbfont size <small|normal|large>");
		return;
	}

	if (string_equals(op, "glyph"))
	{
		p = read_token(p, chs, sizeof(chs));
		if (p == (void *)0 || chs[0] == '\0' || chs[1] != '\0')
		{
			terminal_write_line("Usage: fbfont glyph <ch> <r0> <r1> <r2> <r3> <r4> <r5> <r6>");
			terminal_write_line("Rows are 5-bit values (0x00..0x1F)");
			return;
		}
		for (i = 0; i < 7; i++)
		{
			p = read_token(p, tok, sizeof(tok));
			if (p == (void *)0 || tok[0] == '\0' || parse_color_token(tok, &v) != 0 || v > 0x1F)
			{
				terminal_write_line("Usage: fbfont glyph <ch> <r0> <r1> <r2> <r3> <r4> <r5> <r6>");
				terminal_write_line("Rows are 5-bit values (0x00..0x1F)");
				return;
			}
			rows[i] = v;
		}
		if (!screen_fbfont_set_custom_glyph(chs[0], rows))
		{
			terminal_write_line("fbfont: failed (printable ASCII only)");
			return;
		}
		terminal_write("fbfont: glyph updated for '");
		terminal_putc(chs[0]);
		terminal_write_line("'");
		return;
	}

	if (string_equals(op, "save"))
	{
		p = read_token(p, name, sizeof(name));
		if (p == (void *)0 || name[0] == '\0')
		{
			terminal_write_line("Usage: fbfont save <name>");
			return;
		}
		if (!editor_path_has_ext_ci(name, ".fbf"))
		{
			terminal_write_line("fbfont: use .fbf extension (example: myfont.fbf)");
			return;
		}
		path[0] = '\0';
		APPEND_STR("");
		{
			unsigned long j = 0;
			const char *base = FBFONT_DIR "/";
			while (base[j] != '\0' && j + 1 < sizeof(path)) { path[j] = base[j]; j++; }
			for (i = 0; name[i] != '\0' && j + 1 < sizeof(path); i++) path[j++] = name[i];
			path[j] = '\0';
		}

		out_len = 0;
		APPEND_STR("style=");
		APPEND_STR(screen_fbfont_get_style() == 1 ? "blocky" : "classic");
		APPEND_CH('\n');
		APPEND_STR("size=");
		if (screen_fbfont_get_size() == 12) APPEND_STR("small");
		else if (screen_fbfont_get_size() == 16) APPEND_STR("large");
		else APPEND_STR("normal");
		APPEND_CH('\n');
		for (i = 32; i <= 126; i++)
		{
			int is_custom = 0;
			if (!screen_fbfont_get_custom_glyph((char)i, rows, &is_custom)) continue;
			if (!is_custom) continue;
			APPEND_STR("glyph ");
			APPEND_CH((char)i);
			APPEND_CH(' ');
			APPEND_HEX5(rows[0]); APPEND_CH(' ');
			APPEND_HEX5(rows[1]); APPEND_CH(' ');
			APPEND_HEX5(rows[2]); APPEND_CH(' ');
			APPEND_HEX5(rows[3]); APPEND_CH(' ');
			APPEND_HEX5(rows[4]); APPEND_CH(' ');
			APPEND_HEX5(rows[5]); APPEND_CH(' ');
			APPEND_HEX5(rows[6]);
			APPEND_CH('\n');
		}
		APPEND_CH('\0');

		if (fs_write_text(path, out) != 0)
		{
			terminal_write_line("fbfont: save failed");
			return;
		}
		terminal_write("fbfont: saved ");
		terminal_write_line(path);
		return;
	}

	if (string_equals(op, "load"))
	{
		p = read_token(p, name, sizeof(name));
		if (p == (void *)0 || name[0] == '\0')
		{
			terminal_write_line("Usage: fbfont load <name>");
			return;
		}
		if (!editor_path_has_ext_ci(name, ".fbf"))
		{
			terminal_write_line("fbfont: use .fbf extension (example: myfont.fbf)");
			return;
		}
		{
			unsigned long j = 0;
			const char *base = FBFONT_DIR "/";
			while (base[j] != '\0' && j + 1 < sizeof(path)) { path[j] = base[j]; j++; }
			for (i = 0; name[i] != '\0' && j + 1 < sizeof(path); i++) path[j++] = name[i];
			path[j] = '\0';
		}

		if (fs_read_text(path, &text) != 0)
		{
			terminal_write("fbfont: profile not found: ");
			terminal_write_line(path);
			return;
		}

		screen_fbfont_reset_custom();
		while (*text != '\0')
		{
			unsigned long li = 0;
			while (*text == '\r' || *text == '\n') text++;
			if (*text == '\0') break;
			while (*text != '\0' && *text != '\n' && li + 1 < sizeof(line)) line[li++] = *text++;
			line[li] = '\0';

			if (line[0] == '#' || line[0] == '\0') continue;
			if (string_starts_with(line, "style="))
			{
				if (string_equals(line + 6, "classic")) screen_fbfont_set_style(0);
				else if (string_equals(line + 6, "blocky")) screen_fbfont_set_style(1);
				continue;
			}
			if (string_starts_with(line, "size="))
			{
				if (string_equals(line + 5, "small")) screen_fbfont_set_size(12);
				else if (string_equals(line + 5, "large")) screen_fbfont_set_size(16);
				else screen_fbfont_set_size(14);
				continue;
			}
			if (string_starts_with(line, "glyph "))
			{
				const char *gp = line + 6;
				char gch[8];
				char gtok[16];
				unsigned char grows[7];
				unsigned long gi;

				gp = read_token(gp, gch, sizeof(gch));
				if (gp == (void *)0 || gch[0] == '\0' || gch[1] != '\0') continue;
				for (gi = 0; gi < 7; gi++)
				{
					gp = read_token(gp, gtok, sizeof(gtok));
					if (gp == (void *)0 || gtok[0] == '\0' || parse_color_token(gtok, &v) != 0 || v > 0x1F)
					{
						gi = 99;
						break;
					}
					grows[gi] = v;
				}
				if (gi == 99) continue;
				screen_fbfont_set_custom_glyph(gch[0], grows);
			}
		}

		terminal_write("fbfont: loaded ");
		terminal_write_line(path);
		return;
	}

	terminal_write_line("Usage: fbfont [show|list|style <classic|blocky>|size <small|normal|large>|reset|glyph <ch> <r0..r6>|save <name>|load <name>]");

	#undef APPEND_CH
	#undef APPEND_STR
	#undef APPEND_HEX5
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

/* ------------------------------------------------------------------ */
/* ramfs2fat: recursively copy the entire RAM FS tree to FAT           */
/* ------------------------------------------------------------------ */

static void uint_to_dec(unsigned long v, char *buf, unsigned long buf_sz)
{
	char tmp[20];
	unsigned long i = 0;
	unsigned long j;
	if (buf_sz == 0) return;
	if (v == 0) { if (buf_sz > 1) { buf[0] = '0'; buf[1] = '\0'; } return; }
	while (v > 0 && i < sizeof(tmp) - 1)
	{
		tmp[i++] = (char)('0' + (int)(v % 10));
		v /= 10;
	}
	for (j = 0; j < i && j + 1 < buf_sz; j++)
		buf[j] = tmp[i - 1 - j];
	buf[j] = '\0';
}

static char fat83_safe_upper(char c)
{
	if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
	if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
	if (c == '_' || c == '-' || c == '$' || c == '~') return c;
	return '_';
}

static int string_equals_ci(const char *a, const char *b)
{
	unsigned long i = 0;
	while (a[i] != '\0' && b[i] != '\0')
	{
		if (fat83_safe_upper(a[i]) != fat83_safe_upper(b[i])) return 0;
		i++;
	}
	return a[i] == '\0' && b[i] == '\0';
}

static void ramfs_name_to_fat83_parts(const char *name, char *base, unsigned long base_sz, char *ext, unsigned long ext_sz)
{
	unsigned long bi = 0;
	unsigned long ei = 0;
	unsigned long i = 0;
	int in_ext = 0;

	if (base_sz > 0) base[0] = '\0';
	if (ext_sz > 0) ext[0] = '\0';

	while (name[i] != '\0')
	{
		char c = name[i++];
		if (c == '.')
		{
			if (!in_ext) in_ext = 1;
			continue;
		}

		if (!in_ext)
		{
			if (bi + 1 < base_sz && bi < 8) base[bi++] = fat83_safe_upper(c);
		}
		else
		{
			if (ei + 1 < ext_sz && ei < 3) ext[ei++] = fat83_safe_upper(c);
		}
	}

	if (bi == 0 && base_sz >= 5)
	{
		base[0] = 'F';
		base[1] = 'I';
		base[2] = 'L';
		base[3] = 'E';
		bi = 4;
	}
	if (base_sz > 0) base[bi] = '\0';
	if (ext_sz > 0) ext[ei] = '\0';
}

static void ramfs_build_fat83_name(const char *base, const char *ext, unsigned int suffix, char *out, unsigned long out_sz)
{
	char suffix_dec[12];
	unsigned long base_len = string_length(base);
	unsigned long ext_len = string_length(ext);
	unsigned long k = 0;
	unsigned long i;

	if (out_sz == 0) return;
	out[0] = '\0';

	if (suffix == 0)
	{
		for (i = 0; i < base_len && k + 1 < out_sz; i++) out[k++] = base[i];
	}
	else
	{
		unsigned long dec_len;
		unsigned long keep;
		uint_to_dec((unsigned long)suffix, suffix_dec, sizeof(suffix_dec));
		dec_len = string_length(suffix_dec);
		keep = (8 > (1 + dec_len)) ? (8 - (1 + dec_len)) : 1;
		if (base_len < keep) keep = base_len;
		for (i = 0; i < keep && k + 1 < out_sz; i++) out[k++] = base[i];
		if (k + 1 < out_sz) out[k++] = '~';
		for (i = 0; i < dec_len && k + 1 < out_sz; i++) out[k++] = suffix_dec[i];
	}

	if (ext_len > 0 && k + 2 < out_sz)
	{
		out[k++] = '.';
		for (i = 0; i < ext_len && k + 1 < out_sz; i++) out[k++] = ext[i];
	}
	out[k] = '\0';
}

static int ramfs_used_name_contains(char used[][16], int used_count, const char *name)
{
	int i;
	for (i = 0; i < used_count; i++)
	{
		if (string_equals_ci(used[i], name)) return 1;
	}
	return 0;
}

static void ramfs_used_name_add(char used[][16], int *used_count, const char *name)
{
	unsigned long i = 0;
	if (*used_count >= 128) return;
	while (name[i] != '\0' && i + 1 < 16)
	{
		used[*used_count][i] = fat83_safe_upper(name[i]);
		i++;
	}
	used[*used_count][i] = '\0';
	(*used_count)++;
}

static void ramfs_copy_to_fat_r(const char *rpath, const char *fpath, int *copied, int *errors, int depth, int map_only)
{
	char names[64][FS_NAME_MAX + 2];
	int types[64];
	char used_names[128][16];
	int used_count = 0;
	int count;
	int i;

	if (depth > 16) return;
	if (terminal_cancel_requested) return;
	count = 0;
	if (fs_ls(rpath, names, types, 64, &count) != 0) return;

	/* seed collision table with already-existing FAT entries in this directory */
	{
		char fat_names[64][40];
		int fat_count = 0;
		if (fat32_ls_path(fpath, fat_names, 64, &fat_count) == 0)
		{
			for (i = 0; i < fat_count && used_count < 128; i++)
			{
				char clean[16];
				unsigned long j = 0;
				while (fat_names[i][j] != '\0' && fat_names[i][j] != '/' && j + 1 < sizeof(clean))
				{
					clean[j] = fat83_safe_upper(fat_names[i][j]);
					j++;
				}
				clean[j] = '\0';
				if (clean[0] != '\0' && !ramfs_used_name_contains(used_names, used_count, clean))
					ramfs_used_name_add(used_names, &used_count, clean);
			}
		}
	}

	for (i = 0; i < count; i++)
	{
		terminal_poll_control_hotkeys();
		if (terminal_take_cancel_request()) return;

		char name_clean[FS_NAME_MAX + 1];
		char fat_base[9];
		char fat_ext[4];
		char fat_name[16];
		char child_rpath[256];
		char child_fpath[256];
		unsigned long rlen;
		unsigned long flen;
		unsigned long nlen;
		unsigned long fnlen;
		unsigned int suffix;
		unsigned long k;
		unsigned long j;

		/* strip trailing '/' from directory names */
		nlen = 0;
		while (names[i][nlen] != '\0' && names[i][nlen] != '/' && nlen < FS_NAME_MAX)
		{
			name_clean[nlen] = names[i][nlen];
			nlen++;
		}
		name_clean[nlen] = '\0';
		if (nlen == 0) continue;

		ramfs_name_to_fat83_parts(name_clean, fat_base, sizeof(fat_base), fat_ext, sizeof(fat_ext));
		fat_name[0] = '\0';
		for (suffix = 0; suffix < 1000; suffix++)
		{
			ramfs_build_fat83_name(fat_base, fat_ext, suffix, fat_name, sizeof(fat_name));
			if (fat_name[0] == '\0') continue;
			if (!ramfs_used_name_contains(used_names, used_count, fat_name)) break;
		}
		if (suffix >= 1000)
		{
			(*errors)++;
			continue;
		}
		ramfs_used_name_add(used_names, &used_count, fat_name);

		fnlen = string_length(fat_name);
		if (fnlen == 0) continue;

		/* build child_rpath = rpath + "/" + name_clean */
		rlen = string_length(rpath);
		flen = string_length(fpath);
		if (rlen + 1 + nlen + 1 > sizeof(child_rpath)) continue;
		if (flen + 1 + fnlen + 1 > sizeof(child_fpath)) continue;

		k = 0;
		for (j = 0; j < rlen; j++) child_rpath[k++] = rpath[j];
		if (k == 0 || child_rpath[k - 1] != '/') child_rpath[k++] = '/';
		for (j = 0; j < nlen; j++) child_rpath[k++] = name_clean[j];
		child_rpath[k] = '\0';

		k = 0;
		for (j = 0; j < flen; j++) child_fpath[k++] = fpath[j];
		if (k == 0 || child_fpath[k - 1] != '/') child_fpath[k++] = '/';
		for (j = 0; j < fnlen; j++) child_fpath[k++] = fat_name[j];
		child_fpath[k] = '\0';

		if (types[i] == 1) /* directory */
		{
			if (!map_only) fat32_mkdir_path(child_fpath); /* ignore error — may already exist */
			ramfs_copy_to_fat_r(child_rpath, child_fpath, copied, errors, depth + 1, map_only);
		}
		else /* file */
		{
			if (map_only)
			{
				terminal_write("  ");
				terminal_write(child_rpath);
				terminal_write(" -> ");
				terminal_write_line(child_fpath);
				(*copied)++;
			}
			else
			{
				unsigned char data[FS_MAX_FILE_SIZE];
				unsigned long size;
				size = 0;
				if (fs_read_file(child_rpath, data, sizeof(data), &size) == 0)
				{
					if (fat32_write_file_path(child_fpath, data, size) == 0)
						(*copied)++;
					else
						(*errors)++;
				}
				else (*errors)++;
			}
		}
	}
}

static void cmd_ramfs2fat(const char *args)
{
	int copied;
	int errors;
	int map_only = 0;
	char mode[16];
	char num[16];
	const char *p;

	args = skip_spaces(args);
	if (args[0] != '\0')
	{
		p = read_token(args, mode, sizeof(mode));
		if (p == (void *)0)
		{
			terminal_write_line("Usage: ramfs2fat [map]");
			return;
		}
		if (string_equals(mode, "map")) map_only = 1;
		else
		{
			terminal_write_line("Usage: ramfs2fat [map]");
			return;
		}
	}

	if (!fat32_is_mounted())
	{
		terminal_write_line("ramfs2fat: FAT not mounted. Use fatmount first.");
		return;
	}

	copied = 0;
	errors = 0;
	if (map_only)
	{
		terminal_write_line("ramfs2fat: map preview (RAM FS -> FAT names)");
	}
	else
	{
		terminal_write_line("ramfs2fat: copying RAM FS tree to FAT ...");
	}
	terminal_take_cancel_request(); /* clear stale request */
	ramfs_copy_to_fat_r("/", "/", &copied, &errors, 0, map_only);
	if (terminal_take_cancel_request())
	{
		terminal_write_line("ramfs2fat: canceled by user");
		return;
	}

	uint_to_dec((unsigned long)copied, num, sizeof(num));
	terminal_write("ramfs2fat: ");
	terminal_write(num);
	if (map_only)
	{
		terminal_write_line(" file mapping(s)");
		return;
	}
	if (errors > 0)
	{
		uint_to_dec((unsigned long)errors, num, sizeof(num));
		terminal_write(" file(s) copied, ");
		terminal_write(num);
		terminal_write_line(" error(s)");
	}
	else
	{
		terminal_write_line(" file(s) copied");
	}
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
	terminal_write_line("  \\dboxh \\dboxv \\dboxul \\dboxur \\dboxll \\dboxlr");
	terminal_write_line("  \\boxt \\boxb \\boxl \\boxr \\boxx");
	terminal_write_line("  \\blk \\blkup \\blkdn \\blkl \\blkr");
	terminal_write_line("  \\shade1 \\shade2 \\shade3 \\deg \\pm \\dot");
	terminal_write_line("  \\arru \\arrd \\arrl \\arrr \\tri");
	terminal_write_line("  \\xNN for raw byte (hex), example: echo \\xDB\\xDB\\xDB");
	terminal_write_line("  echo styles: §l bold, §i italic, §u underline, §s strike, uppercase disables, §r resets");
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
	unsigned char data[FS_MAX_FILE_SIZE];
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

static void cmd_fatattr(const char *args)
{
	char path[128];
	char full_path[128];
	char tok[16];
	unsigned char attr;
	unsigned char set_mask = 0;
	unsigned char clear_mask = 0;
	int modify = 0;
	const char *p = read_token(args, path, sizeof(path));

	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: fatattr <path> [+r|-r] [+h|-h] [+s|-s] [+a|-a]");
		return;
	}
	if (!fat32_is_mounted())
	{
		terminal_write_line("fatattr: not mounted (run fatmount)");
		return;
	}
	if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0)
	{
		terminal_write_line("fatattr: bad path");
		return;
	}

	for (;;)
	{
		p = read_token(p, tok, sizeof(tok));
		if (p == (void *)0)
		{
			terminal_write_line("Usage: fatattr <path> [+r|-r] [+h|-h] [+s|-s] [+a|-a]");
			return;
		}
		if (tok[0] == '\0') break;
		if ((tok[0] != '+' && tok[0] != '-') || tok[1] == '\0' || tok[2] != '\0')
		{
			terminal_write_line("fatattr: expected +r/-r +h/-h +s/-s +a/-a");
			return;
		}

		modify = 1;
		if (tok[1] == 'r' || tok[1] == 'R')
		{
			if (tok[0] == '+') set_mask |= 0x01; else clear_mask |= 0x01;
		}
		else if (tok[1] == 'h' || tok[1] == 'H')
		{
			if (tok[0] == '+') set_mask |= 0x02; else clear_mask |= 0x02;
		}
		else if (tok[1] == 's' || tok[1] == 'S')
		{
			if (tok[0] == '+') set_mask |= 0x04; else clear_mask |= 0x04;
		}
		else if (tok[1] == 'a' || tok[1] == 'A')
		{
			if (tok[0] == '+') set_mask |= 0x20; else clear_mask |= 0x20;
		}
		else
		{
			terminal_write_line("fatattr: unknown attribute (use r,h,s,a)");
			return;
		}
	}

	if (modify)
	{
		if (fat32_set_attr_path(full_path, set_mask, clear_mask, &attr) != 0)
		{
			terminal_write_line("fatattr: set failed");
			return;
		}
	}
	else
	{
		if (fat32_get_attr_path(full_path, &attr) != 0)
		{
			terminal_write_line("fatattr: read failed");
			return;
		}
	}

	terminal_write("fatattr: ");
	terminal_write(full_path);
	terminal_write(" = [");
	terminal_putc((attr & 0x01) ? 'R' : '-');
	terminal_putc((attr & 0x02) ? 'H' : '-');
	terminal_putc((attr & 0x04) ? 'S' : '-');
	terminal_putc((attr & 0x20) ? 'A' : '-');
	terminal_write("]");
	if (attr & 0x10) terminal_write(" DIR");
	if (attr & 0x08) terminal_write(" VOL");
	terminal_putc('\n');
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
	char resolved[INPUT_BUFFER_SIZE];
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
	if (resolve_command_aliases(expanded_glyphs, resolved, sizeof(resolved)) != 0)
	{
		input_length = 0;
		input_buffer[0] = '\0';
		if (!editor_active && !script_mode_active) terminal_prompt();
		return;
	}

	i = 0;
	while (resolved[i] != '\0' && i + 1 < sizeof(input_buffer))
	{
		input_buffer[i] = resolved[i];
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
		else terminal_write_line("Usage: help [basic|fs|disk|commands [page]|<command> [page]]");
	}
	else if (string_starts_with(input_buffer, "man"))
	{
		if (input_buffer[3] == ' ') cmd_man(input_buffer + 4);
		else terminal_write_line("Usage: man <topic> [page]");
	}
	else if (string_starts_with(input_buffer, "alias"))
	{
		if (input_buffer[5] == '\0') cmd_alias("");
		else if (input_buffer[5] == ' ') cmd_alias(input_buffer + 6);
		else terminal_write_line("Usage: alias <name> <command...>");
	}
	else if (string_starts_with(input_buffer, "unalias"))
	{
		if (input_buffer[7] == ' ') cmd_unalias(input_buffer + 8);
		else terminal_write_line("Usage: unalias <name>");
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
	else if (string_starts_with(input_buffer, "dir"))
	{
		if (input_buffer[3] == '\0') cmd_dir("");
		else if (input_buffer[3] == ' ') cmd_dir(input_buffer + 4);
		else terminal_write_line("Usage: dir [/b] [/w] [/s] [path]");
	}
	else if (string_starts_with(input_buffer, "tree"))
	{
		if (input_buffer[4] == '\0') cmd_tree("");
		else if (input_buffer[4] == ' ') cmd_tree(input_buffer + 5);
		else terminal_write_line("Usage: tree [/f] [path]");
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
	else if (string_starts_with(input_buffer, "type"))
	{
		if (input_buffer[4] == ' ') cmd_cat(input_buffer + 5);
		else terminal_write_line("Usage: type <path>");
	}
	else if (string_starts_with(input_buffer, "rm"))
	{
		if (input_buffer[2] == ' ') cmd_rm(input_buffer + 3);
		else terminal_write_line("Usage: rm [-r] [-f] <path>");
	}
	else if (string_starts_with(input_buffer, "del"))
	{
		if (input_buffer[3] == ' ') cmd_rm(input_buffer + 4);
		else terminal_write_line("Usage: del <path>");
	}
	else if (string_starts_with(input_buffer, "cp"))
	{
		if (input_buffer[2] == ' ') cmd_cp(input_buffer + 3);
		else terminal_write_line("Usage: cp [-r] [-n] [-i] <src> <dst>");
	}
	else if (string_starts_with(input_buffer, "copy"))
	{
		if (input_buffer[4] == ' ') cmd_cp(input_buffer + 5);
		else terminal_write_line("Usage: copy <src> <dst>");
	}
	else if (string_starts_with(input_buffer, "mv"))
	{
		if (input_buffer[2] == ' ') cmd_mv(input_buffer + 3);
		else terminal_write_line("Usage: mv [-n] [-i] <src> <dst>");
	}
	else if (string_starts_with(input_buffer, "move"))
	{
		if (input_buffer[4] == ' ') cmd_mv(input_buffer + 5);
		else terminal_write_line("Usage: move <src> <dst>");
	}
	else if (string_starts_with(input_buffer, "ren"))
	{
		if (input_buffer[3] == ' ') cmd_mv(input_buffer + 4);
		else terminal_write_line("Usage: ren <old> <new>");
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
	else if (string_equals(input_buffer, "cls"))
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
	else if (string_equals(input_buffer, "memstat"))
	{
		cmd_memstat();
	}
	else if (string_equals(input_buffer, "pagetest"))
	{
		cmd_pagetest();
	}
	else if (string_starts_with(input_buffer, "pagefault"))
	{
		if (input_buffer[9] == ' ') cmd_pagefault(input_buffer + 10);
		else terminal_write_line("Usage: pagefault <read|write|exec>");
	}
	else if (string_equals(input_buffer, "gpfault"))
	{
		cmd_gpfault("");
	}
	else if (string_equals(input_buffer, "udfault"))
	{
		cmd_udfault("");
	}
	else if (string_equals(input_buffer, "doublefault"))
	{
		cmd_doublefault("");
	}
	else if (string_equals(input_buffer, "exceptstat"))
	{
		cmd_exceptstat("");
	}
	else if (string_equals(input_buffer, "dumpstack"))
	{
		cmd_dumpstack("");
	}
	else if (string_starts_with(input_buffer, "selftest"))
	{
		if (input_buffer[8] == ' ') cmd_selftest(input_buffer + 9);
		else terminal_write_line("Usage: selftest exceptions [pf-read|pf-write|pf-exec|ud|gp|1|2|3|4|5]");
	}
	else if (string_equals(input_buffer, "elfselftest"))
	{
		cmd_elfselftest("");
	}
	else if (string_starts_with(input_buffer, "elfinfo"))
	{
		if (input_buffer[7] == ' ') cmd_elfinfo(input_buffer + 8);
		else terminal_write_line("Usage: elfinfo <path>");
	}
	else if (string_starts_with(input_buffer, "elfsym"))
	{
		if (input_buffer[6] == ' ') cmd_elfsym(input_buffer + 7);
		else terminal_write_line("Usage: elfsym <path> [filter]");
	}
	else if (string_starts_with(input_buffer, "elfaddr"))
	{
		if (input_buffer[7] == ' ') cmd_elfaddr(input_buffer + 8);
		else terminal_write_line("Usage: elfaddr <path> <hex-address>");
	}
	else if (string_starts_with(input_buffer, "execstress"))
	{
		if (input_buffer[10] == ' ') cmd_execstress(input_buffer + 11);
		else terminal_write_line("Usage: execstress <count> <path>");
	}
	else if (string_starts_with(input_buffer, "exec"))
	{
		if (input_buffer[4] == ' ') cmd_exec(input_buffer + 5);
		else terminal_write_line("Usage: exec <path>");
	}
	else if (string_equals(input_buffer, "tasks"))
	{
		cmd_tasks();
	}
	else if (string_equals(input_buffer, "tasktest"))
	{
		cmd_tasktest();
	}
	else if (string_equals(input_buffer, "taskspin"))
	{
		cmd_taskspin();
	}
	else if (string_equals(input_buffer, "shellspawn"))
	{
		cmd_shellspawn();
	}
	else if (string_starts_with(input_buffer, "shellwatch"))
	{
		if (input_buffer[10] == '\0') cmd_shellwatch("show");
		else if (input_buffer[10] == ' ') cmd_shellwatch(input_buffer + 11);
		else terminal_write_line("Usage: shellwatch [on|off|show]");
	}
	else if (string_starts_with(input_buffer, "taskprotect"))
	{
		if (input_buffer[11] == ' ') cmd_taskprotect(input_buffer + 12);
		else terminal_write_line("Usage: taskprotect <id|name> <on|off>");
	}
	else if (string_starts_with(input_buffer, "tasklog"))
	{
		if (input_buffer[7] == '\0') cmd_tasklog("show");
		else if (input_buffer[7] == ' ') cmd_tasklog(input_buffer + 8);
		else terminal_write_line("Usage: tasklog [on|off|show]");
	}
	else if (string_starts_with(input_buffer, "taskkill"))
	{
		if (input_buffer[8] == ' ') cmd_taskkill(input_buffer + 9);
		else terminal_write_line("Usage: taskkill <id>|all");
	}
	else if (string_starts_with(input_buffer, "taskstop"))
	{
		if (input_buffer[8] == ' ') cmd_taskstop(input_buffer + 9);
		else terminal_write_line("Usage: taskstop <id>");
	}
	else if (string_starts_with(input_buffer, "taskcont"))
	{
		if (input_buffer[8] == ' ') cmd_taskcont(input_buffer + 9);
		else terminal_write_line("Usage: taskcont <id>");
	}
	else if (string_equals(input_buffer, "ticks"))
	{
		cmd_ticks();
	}
	else if (string_equals(input_buffer, "motd"))
	{
		cmd_motd();
	}
	else if (string_starts_with(input_buffer, "autorun"))
	{
		if (input_buffer[7] == '\0') cmd_autorun("show");
		else if (input_buffer[7] == ' ') cmd_autorun(input_buffer + 8);
		else terminal_write_line("Usage: autorun [show|off|always|once|rearm|stop|run|delay <0..3600>]");
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
	else if (string_equals(input_buffer, "drives"))
	{
		cmd_drives();
	}
	else if (string_starts_with(input_buffer, "fatmount"))
	{
		if (input_buffer[8] == '\0') cmd_fatmount((void *)0);
		else if (input_buffer[8] == ' ') cmd_fatmount(input_buffer + 9);
		else terminal_write_line("Usage: fatmount [0|1]");
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
	else if (string_starts_with(input_buffer, "fatattr"))
	{
		if (input_buffer[7] == ' ') cmd_fatattr(input_buffer + 8);
		else terminal_write_line("Usage: fatattr <path> [+r|-r] [+h|-h] [+s|-s] [+a|-a]");
	}
	else if (string_starts_with(input_buffer, "fatrm"))
	{
		if (input_buffer[5] == ' ') cmd_fatrm(input_buffer + 6);
		else terminal_write_line("Usage: fatrm <path>");
	}
	else if (string_starts_with(input_buffer, "echo"))
	{
		if      (input_buffer[4] == '\0') terminal_putc('\n');
		else if (input_buffer[4] == ' ')
		{
			terminal_write_echo_text(&input_buffer[5]);
			terminal_putc('\n');
		}
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
		else terminal_write_line("Usage: color [show|preview [text|prompt]|text <0xNN>|prompt <0xNN>]");
	}
	else if (string_starts_with(input_buffer, "serial"))
	{
		if (input_buffer[6] == '\0') cmd_serial("show");
		else if (input_buffer[6] == ' ') cmd_serial(input_buffer + 7);
		else terminal_write_line("Usage: serial [on|off|show|compact <on|off>|rxecho <on|off>]");
	}
	else if (string_starts_with(input_buffer, "display"))
	{
		if (input_buffer[7] == '\0') cmd_display("show");
		else if (input_buffer[7] == ' ') cmd_display(input_buffer + 8);
		else terminal_write_line("Usage: display [show|vga25|vga50|fb|mode <show|list|1080p|900p|768p|720p|WIDTHxHEIGHT[xBPP]>|cursor <show|underline|block|bar>]");
	}
	else if (string_starts_with(input_buffer, "fbfont"))
	{
		if (input_buffer[6] == '\0') cmd_fbfont("show");
		else if (input_buffer[6] == ' ') cmd_fbfont(input_buffer + 7);
		else terminal_write_line("Usage: fbfont [show|list|style <classic|blocky>|size <small|normal|large>|reset|glyph <ch> <r0..r6>|save <name>|load <name>]");
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
	else if (string_starts_with(input_buffer, "ramfs2fat"))
	{
		if (input_buffer[9] == '\0') cmd_ramfs2fat((void *)0);
		else if (input_buffer[9] == ' ') cmd_ramfs2fat(input_buffer + 10);
		else terminal_write_line("Usage: ramfs2fat [map]");
	}
	else if (string_equals(input_buffer, "ramfs"))
	{
		cmd_ramfs();
	}
	else if (string_equals(input_buffer, "reboot"))
	{
		do_reboot();
	}
	else if (string_equals(input_buffer, "panic"))
	{
		terminal_write_line("[SYSTEM] deliberate panic requested");
		trigger_forced_panic();
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
	task_reap_zombies();
	if (!editor_active && !script_mode_active)
	{
		terminal_prompt();
	}
}

static void submit_current_line(void)
{
	unsigned long i;
	sync_screen_pos();
	terminal_putc('\n');
	input_buffer[input_length] = '\0';
	history_pos = -1;
	if (terminal_capture_mode)
	{
		if (terminal_capture_out != (void *)0 && terminal_capture_out_size > 0)
		{
			for (i = 0; i + 1 < terminal_capture_out_size && input_buffer[i] != '\0'; i++) terminal_capture_out[i] = input_buffer[i];
			terminal_capture_out[i] = '\0';
		}
		input_length = 0;
		cursor_pos = 0;
		input_buffer[0] = '\0';
		terminal_capture_done = 1;
		return;
	}
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
		if (editor_find_active) return;
		if (scancode == 0x1D) { ctrl_held = 1; return; }
		if (scancode == 0x9D) { ctrl_held = 0; return; }
		if (scancode == 0x38) { alt_held = 1; return; }
		if (scancode == 0xB8) { alt_held = 0; return; }
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
			editor_begin_selection_if_needed();
			if (ctrl_held) editor_cursor = editor_move_word_left(editor_cursor);
			else if (editor_cursor > 0) editor_cursor--;
			editor_finish_selection_move();
			editor_render();
			return;
		}
		if (scancode == 0x4D)
		{
			editor_begin_selection_if_needed();
			if (ctrl_held) editor_cursor = editor_move_word_right(editor_cursor);
			else if (editor_cursor < editor_length) editor_cursor++;
			editor_finish_selection_move();
			editor_render();
			return;
		}
		if (scancode == 0x47)
		{
			editor_begin_selection_if_needed();
			if (ctrl_held) editor_cursor = 0;
			else editor_cursor = editor_line_start(editor_cursor);
			editor_finish_selection_move();
			editor_render();
			return;
		}
		if (scancode == 0x4F)
		{
			editor_begin_selection_if_needed();
			if (ctrl_held) editor_cursor = editor_length;
			else editor_cursor = editor_line_end(editor_cursor);
			editor_finish_selection_move();
			editor_render();
			return;
		}
		if (scancode == 0x49)
		{
			rows = editor_visible_rows();
			if (rows > 1) rows--;
			editor_begin_selection_if_needed();
			editor_cursor = editor_move_visual_rows(editor_cursor, rows, 0);
			editor_finish_selection_move();
			editor_render();
			return;
		}
		if (scancode == 0x51)
		{
			rows = editor_visible_rows();
			if (rows > 1) rows--;
			editor_begin_selection_if_needed();
			editor_cursor = editor_move_visual_rows(editor_cursor, rows, 1);
			editor_finish_selection_move();
			editor_render();
			return;
		}
		if (scancode == 0x48)
		{
			editor_begin_selection_if_needed();
			line_start = editor_line_start(editor_cursor);
			if (line_start == 0)
			{
				editor_finish_selection_move();
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
			editor_finish_selection_move();
			editor_render();
			return;
		}
		if (scancode == 0x50)
		{
			editor_begin_selection_if_needed();
			line_start = editor_line_start(editor_cursor);
			line_end = editor_line_end(editor_cursor);
			if (line_end >= editor_length)
			{
				editor_finish_selection_move();
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
			editor_finish_selection_move();
			editor_render();
			return;
		}
		if (scancode == 0x53)
		{
			if (editor_has_selection()) editor_delete_selection();
			else if (editor_cursor < editor_length) editor_delete_range(editor_cursor, editor_cursor + 1);
			editor_render();
			return;
		}
		return;
	}

	if (scancode == 0x2A || scancode == 0x36) { shift_held = 1; return; }
	if (scancode == 0xAA || scancode == 0xB6) { shift_held = 0; return; }
	if (scancode == 0x1D) { ctrl_held = 1; return; }
	if (scancode == 0x9D) { ctrl_held = 0; return; }
	if (scancode == 0x38) { alt_held = 1; return; }
	if (scancode == 0xB8) { alt_held = 0; return; }
	if (scancode == 0x11 && ctrl_held && shift_held) { editor_close(0, 0); return; } /* Ctrl+Shift+W */
	if (scancode == 0x01) { panic_esc_held = 1; update_panic_hotkey(); return; }
	if (scancode == 0x81) { panic_esc_held = 0; update_panic_hotkey(); return; }
	if (scancode == 0x58) { panic_f12_held = 1; update_panic_hotkey(); return; }
	if (scancode == 0xD8) { panic_f12_held = 0; update_panic_hotkey(); return; }
	if (scancode == 0x3A) { caps_lock_on = !caps_lock_on; return; }
	if (scancode & 0x80) return;

	if (editor_find_active)
	{
		if (scancode == 0x01)
		{
			editor_find_cancel();
			return;
		}
		if (scancode == 0x1C)
		{
			editor_find_active = 0;
			if (shift_held) editor_find_prev();
			else editor_find_next(editor_find_match_valid ? 1 : 0);
			return;
		}
		if (scancode == 0x0E)
		{
			if (editor_find_query_length > 0)
			{
				editor_find_query_length--;
				editor_find_query[editor_find_query_length] = '\0';
			}
			editor_render();
			return;
		}
		c = translate_scancode(scancode);
		if (c >= 0x20 && c <= 0x7E && editor_find_query_length + 1 < sizeof(editor_find_query))
		{
			editor_find_query[editor_find_query_length++] = c;
			editor_find_query[editor_find_query_length] = '\0';
			editor_render();
		}
		return;
	}

	if (scancode == 0x44)
	{
		if (editor_save() == 0) editor_close(1, 1);
		else editor_status_line("[editor] save failed");
		return;
	}

	if (scancode == 0x1F && ctrl_held)
	{
		if (editor_save() == 0) editor_status_line("[editor] saved");
		else editor_status_line("[editor] save failed");
		return;
	}

	if (!editor_hex_mode && ctrl_held)
	{
		if (scancode == 0x1E)
		{
			editor_selection_active = 1;
			editor_selection_anchor = 0;
			editor_cursor = editor_length;
			editor_render();
			return;
		}
		if (scancode == 0x2E)
		{
			editor_copy_selection(0);
			return;
		}
		if (scancode == 0x2D)
		{
			editor_copy_selection(1);
			return;
		}
		if (scancode == 0x2F)
		{
			if (!editor_insert_text(editor_clipboard, editor_clipboard_length))
				editor_status_line("[editor] paste failed (buffer full)");
			else
				editor_render();
			return;
		}
		if (scancode == 0x21)
		{
			editor_find_open();
			return;
		}
	}

	if (scancode == 0x01)
	{
		editor_close(0, 0);
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
				editor_find_invalidate_match();
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
		editor_find_invalidate_match();
		editor_render();
		return;
	}

	if (scancode == 0x0E)
	{
		if (editor_has_selection())
		{
			editor_delete_selection();
			editor_render();
		}
		else if (editor_cursor > 0)
		{
			for (i = editor_cursor - 1; i < editor_length - 1; i++) editor_buffer[i] = editor_buffer[i + 1];
			editor_cursor--;
			editor_length--;
			editor_buffer[editor_length] = '\0';
			editor_dirty = 1;
			editor_find_invalidate_match();
			editor_clear_selection();
			editor_render();
		}
		return;
	}

	if (scancode == 0x1C)
	{
		if (editor_insert_text("\n", 1))
		{
			editor_render();
		}
		else editor_status_line("[editor] buffer full");
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
	if (!editor_insert_text(&c, 1)) editor_status_line("[editor] buffer full");
	else editor_render();
}

/* ================================================================== */
/* Scancode dispatcher                                                */
/* ================================================================== */

static void handle_scancode(unsigned char scancode)
{
	char c;

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
		if (scancode == 0x38) { alt_held = 1; return; }
		if (scancode == 0xB8) { alt_held = 0; return; }
		if (scancode == 0x13 && ctrl_held && shift_held) { do_reboot(); return; } /* Ctrl+Shift+R */
		if (scancode == 0x1C) { submit_current_line(); return; } /* Keypad Enter */
		if (scancode == 0x35) { c = '/'; goto insert_character; } /* Keypad / */
		if (scancode == 0x48) { handle_arrow_up(); return; }
		if (scancode == 0x50) { handle_arrow_down(); return; }
		if (scancode == 0x4B && cursor_pos > 0)  /* Left */
		{
			terminal_begin_selection_if_needed();
			if (ctrl_held)
			{
				while (cursor_pos > 0 && !char_is_word(input_buffer[cursor_pos - 1])) cursor_pos--;
				while (cursor_pos > 0 && char_is_word(input_buffer[cursor_pos - 1])) cursor_pos--;
			}
			else cursor_pos--;
			terminal_finish_selection_move();
			screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
			terminal_redraw_input_line();
			return;
		}
		if (scancode == 0x4D && cursor_pos < input_length)  /* Right */
		{
			terminal_begin_selection_if_needed();
			if (ctrl_held)
			{
				while (cursor_pos < input_length && char_is_word(input_buffer[cursor_pos])) cursor_pos++;
				while (cursor_pos < input_length && !char_is_word(input_buffer[cursor_pos])) cursor_pos++;
			}
			else cursor_pos++;
			terminal_finish_selection_move();
			screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
			terminal_redraw_input_line();
			return;
		}
		if (scancode == 0x47) /* Home */
		{ terminal_begin_selection_if_needed(); cursor_pos = 0; terminal_finish_selection_move(); terminal_redraw_input_line(); return; }
		if (scancode == 0x4F) /* End */
		{ terminal_begin_selection_if_needed(); cursor_pos = input_length; terminal_finish_selection_move(); terminal_redraw_input_line(); return; }
		if (scancode == 0x53) /* Delete */
		{
			if (terminal_has_selection()) terminal_delete_selection();
			else if (cursor_pos < input_length) terminal_delete_range(cursor_pos, cursor_pos + 1);
			return;
		}
		return;
	}

	if (scancode == 0x2A || scancode == 0x36) { shift_held = 1; return; }
	if (scancode == 0xAA || scancode == 0xB6) { shift_held = 0; return; }
	if (scancode == 0x1D) { ctrl_held = 1; return; }
	if (scancode == 0x9D) { ctrl_held = 0; return; }
	if (scancode == 0x38) { alt_held = 1; return; }
	if (scancode == 0xB8) { alt_held = 0; return; }
	if (scancode == 0x10 && ctrl_held && shift_held) { terminal_shutdown(); return; } /* Ctrl+Shift+Q */
	if (scancode == 0x01) { panic_esc_held = 1; update_panic_hotkey(); return; }
	if (scancode == 0x81) { panic_esc_held = 0; update_panic_hotkey(); return; }
	if (scancode == 0x58) { panic_f12_held = 1; update_panic_hotkey(); return; }
	if (scancode == 0xD8) { panic_f12_held = 0; update_panic_hotkey(); return; }
	if (scancode == 0x3A) { caps_lock_on = !caps_lock_on; return; }
	if (scancode == 0x0F) { handle_tab(); return; }
	if (scancode & 0x80)  return;

	if (scancode == 0x0E) { handle_backspace(0); return; }

	if (ctrl_held)
	{
		if (scancode == 0x2E)
		{
			if (input_length == 0)
			{
				terminal_request_cancel();
				terminal_write_line("^C");
			}
			else
			{
				terminal_abort_input_line();
			}
			return;
		}
		if (scancode == 0x1E)
		{
			terminal_selection_active = 1;
			terminal_selection_anchor = 0;
			cursor_pos = input_length;
			terminal_redraw_input_line();
			return;
		}
		if (scancode == 0x2E) { terminal_copy_selection(0); terminal_redraw_input_line(); return; }
		if (scancode == 0x2D) { terminal_copy_selection(1); return; }
		if (scancode == 0x2F)
		{
			terminal_insert_text(editor_clipboard, editor_clipboard_length);
			return;
		}
	}

	if (scancode == 0x1C)
	{
		submit_current_line();
		return;
	}

	c = translate_scancode(scancode);
	if (c == '\0' || input_length >= (INPUT_BUFFER_SIZE - 1)) return;

insert_character:
	if (input_length >= (INPUT_BUFFER_SIZE - 1) && !terminal_has_selection()) return;
	terminal_insert_text(&c, 1);
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
	enum boot_display_pref boot_display = boot_display_preference(mb2_info_addr);
	int boot_fb_fallback = 0;

	ensure_theme_files();
	load_current_system_theme();
	load_current_editor_theme();
	if (boot_display == BOOT_DISPLAY_FB)
	{
		if (screen_set_framebuffer_text_mode()) display_mode = 2;
		else
		{
			screen_set_text_mode_80x25();
			display_mode = 0;
			boot_fb_fallback = 1;
		}
	}
	else
	{
		screen_set_text_mode_80x25();
		display_mode = 0;
	}
	screen_set_color(terminal_text_color);
	serial_ready = serial_init();
	memmap_init(mb2_info_addr);
	terminal_auto_fatmount();
	screen_set_style(SCREEN_STYLE_BOLD);
	terminal_write_colored("TG11 OS (64-bit)", color_bold_variant(terminal_text_color));
	screen_set_style(0);
	terminal_putc('\n');
	if (TG11_OS_VERSION[0] != '\0')
	{
		char version_rest[32];
		unsigned long i = 1;
		unsigned long o = 0;
		terminal_write_colored("v", 0x02);
		while (TG11_OS_VERSION[i] != '\0' && o + 1 < sizeof(version_rest))
		{
			version_rest[o++] = TG11_OS_VERSION[i++];
		}
		version_rest[o] = '\0';
		terminal_write_line(version_rest);
	}
	else terminal_putc('\n');
	if (display_mode == 2)
	{
		terminal_write_line("display: boot framebuffer mode enabled");
		if (framebuffer_mode_source() == 1)
		{
			char n[16];
			terminal_write("display: source=kernel-vbe ");
			uint_to_dec((unsigned long)framebuffer_boot_width(), n, sizeof(n)); terminal_write(n);
			terminal_write("x");
			uint_to_dec((unsigned long)framebuffer_boot_height(), n, sizeof(n)); terminal_write(n);
			terminal_write("x");
			uint_to_dec((unsigned long)framebuffer_boot_bpp(), n, sizeof(n)); terminal_write(n);
			terminal_write(" -> ");
			uint_to_dec((unsigned long)framebuffer_width(), n, sizeof(n)); terminal_write(n);
			terminal_write("x");
			uint_to_dec((unsigned long)framebuffer_height(), n, sizeof(n)); terminal_write(n);
			terminal_write("x");
			uint_to_dec((unsigned long)framebuffer_bpp(), n, sizeof(n)); terminal_write(n);
			terminal_putc('\n');
		}
		else
		{
			terminal_write_line("display: source=bootloader");
		}
	}
	else if (boot_fb_fallback) terminal_write_line("display: boot framebuffer unavailable; using VGA text");
	terminal_print_motd();
	terminal_run_boot_autorun();
	terminal_write("Type ");
	terminal_write_colored("help", 0x06);
	terminal_write_line(" to view available commands.");
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
	terminal_poll_boot_autorun();
}

int terminal_read_line(char *out, unsigned long out_size)
{
	if (out == (void *)0 || out_size == 0) return -1;
	if (editor_active || script_mode_active || terminal_capture_mode) return -1;

	terminal_capture_mode = 1;
	terminal_capture_done = 0;
	terminal_capture_out = out;
	terminal_capture_out_size = out_size;

	input_length = 0;
	cursor_pos = 0;
	input_buffer[0] = '\0';
	prompt_vga_start = screen_get_pos();
	screen_set_hw_cursor(prompt_vga_start);

	while (!terminal_capture_done)
	{
		terminal_poll();
	}

	terminal_capture_mode = 0;
	terminal_capture_out = (void *)0;
	terminal_capture_out_size = 0;
	return 0;
}

