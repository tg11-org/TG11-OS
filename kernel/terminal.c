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
#include "syscall.h"
#include "net.h"
#include "mouse.h"
#include "usb.h"
#include "e1000.h"
#include "pci.h"

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
#define AUTORUN_SAFE_MODE_PATH "/etc/autorun.safe"
#define AUTORUN_CLEAN_PATH "/etc/autorun.clean"
#define AUTORUN_LAST_STATUS_PATH "/etc/autorun.last"
#define AUTORUN_LAST_SOURCE_PATH "/etc/autorun.src"
#define FAT_AUTORUN_PATH "/autorun.sh"
#define FAT_AUTORUN_MODE_PATH "/autorun.mod"
#define FAT_AUTORUN_ONCE_STATE_PATH "/autorun.onc"
#define FAT_AUTORUN_DELAY_PATH "/autorun.dly"
#define FAT_AUTORUN_SAFE_MODE_PATH "/autorun.saf"
#define FAT_AUTORUN_CLEAN_PATH "/autorun.cln"
#define FAT_AUTORUN_LAST_STATUS_PATH "/autorun.lst"
#define FAT_AUTORUN_LAST_SOURCE_PATH "/autorun.src"
#define AUTORUN_DEFAULT_DELAY_SECONDS 8UL
#define AUTORUN_SAFE_BOOT_WINDOW_TICKS (15UL * 100UL)
#define SCRIPT_DEFAULT_TIMEOUT_TICKS (30UL * 100UL)
#define RAMFS2FAT_BATCH_MAX 64
#define RAMFS2FAT_USED_MAX 128
#define FATSTRESS_MAX_PAYLOAD 4096
#define ELF_SHELL_IO_MAX (64UL * 1024UL)
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

/* Ctrl+R reverse search state */
static int  rsearch_active = 0;
static char rsearch_term[64];
static int  rsearch_len = 0;
static int  rsearch_match = -1; /* index into history ring, or -1 */

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

/* Output capture for pipes and redirection */
static int terminal_output_capture = 0;
static char *terminal_output_buf = (void *)0;
static unsigned long terminal_output_buf_size = 0;
static unsigned long terminal_output_buf_len = 0;

/* Pipe stdin: set when the right-side of a pipe is running */
static const char *pipe_stdin_buf = (void *)0;
static unsigned long pipe_stdin_len = 0;

static int display_mode = 0; /* 0=vga25, 1=vga50, 2=framebuffer */

/* ------------------------------------------------------------------ */
/* GUI / Desktop state                                                */
/* ------------------------------------------------------------------ */
#define GUI_MAX_WINDOWS    8
#define GUI_TITLEBAR_H    22
#define GUI_BORDER_W       2
#define GUI_TASKBAR_H     30
#define GUI_CURSOR_W      12
#define GUI_CURSOR_H      19
#define GUI_WIN_TEXT_COLS  80
#define GUI_WIN_TEXT_ROWS  25

/* Window types */
#define GUI_WTYPE_TERMINAL  0
#define GUI_WTYPE_PAINT     1
#define GUI_WTYPE_EDITOR    2
#define GUI_WTYPE_EXPLORER  3
#define GUI_WTYPE_HEXEDIT   4

/* File explorer constants */
#define GUI_EXPLORER_MAX    64
#define GUI_EXPLORER_NAME   40
#define GUI_EXPLORER_ITEM_H 16   /* pixel height per entry row */

/* Paint canvas constants */
#define GUI_PAINT_PALETTE_H  24
#define GUI_PAINT_TOOLBAR_H  20
#define GUI_PAINT_COLORS     16
#define GUI_PAINT_BUF_W     800
#define GUI_PAINT_BUF_H     600

/* Paint tools */
#define GUI_PAINT_TOOL_BRUSH  0
#define GUI_PAINT_TOOL_LINE   1
#define GUI_PAINT_TOOL_RECT   2
#define GUI_PAINT_TOOL_FILL   3
#define GUI_PAINT_TOOL_ERASER 4
#define GUI_PAINT_TOOL_COUNT  5

/* Resize handle */
#define GUI_RESIZE_HANDLE   10
#define GUI_MIN_WIN_W      120
#define GUI_MIN_WIN_H       80

/* Desktop theme system */
#define GUI_BUILTIN_THEMES  12
#define GUI_CUSTOM_THEMES    4
#define GUI_THEME_COUNT     (GUI_BUILTIN_THEMES + GUI_CUSTOM_THEMES)
struct gui_theme {
	const char *name;
	unsigned int desktop;
	unsigned int taskbar;
	unsigned int task_text;
	unsigned int titlebar;
	unsigned int title_inac;
	unsigned int title_text;
	unsigned int border;
	unsigned int winbg;
	unsigned int close_bg;
	unsigned int close_fg;
	unsigned int cursor;
	unsigned int cursor_brd;
	unsigned int start_bg;
	unsigned int start_fg;
	unsigned int clock_fg;
	unsigned int menu_bg;
	unsigned int menu_hl;
	unsigned int menu_text;
	unsigned int menu_sep;
};
static struct gui_theme gui_themes[GUI_THEME_COUNT] = {
	{ /* 0: Classic Teal (default) */
		"Classic Teal",
		0x008080, 0x1A1A2E, 0xCCCCCC, 0x2D2D7F, 0x555555, 0xFFFFFF,
		0x404040, 0x000000, 0xCC3333, 0xFFFFFF, 0xFFFFFF, 0x000000,
		0x2D5F2D, 0xFFFFFF, 0xFFFFFF, 0x2A2A3E, 0x4040A0, 0xFFFFFF, 0x555555
	},
	{ /* 1: Dark Mode */
		"Dark Mode",
		0x1E1E1E, 0x111111, 0xAAAAAA, 0x264F78, 0x3C3C3C, 0xDDDDDD,
		0x333333, 0x1E1E1E, 0xCC3333, 0xFFFFFF, 0xFFFFFF, 0x000000,
		0x264F78, 0xFFFFFF, 0xAAAAAA, 0x252526, 0x094771, 0xDDDDDD, 0x444444
	},
	{ /* 2: Ocean Blue */
		"Ocean Blue",
		0x1B4F72, 0x0B2545, 0xD4E6F1, 0x2980B9, 0x5D6D7E, 0xFFFFFF,
		0x1A5276, 0x0A1929, 0xE74C3C, 0xFFFFFF, 0xFFFFFF, 0x000000,
		0x1F618D, 0xFFFFFF, 0xD4E6F1, 0x154360, 0x2E86C1, 0xFFFFFF, 0x5D6D7E
	},
	{ /* 3: Forest Green */
		"Forest Green",
		0x1B4332, 0x081C15, 0xB7E4C7, 0x2D6A4F, 0x52796F, 0xFFFFFF,
		0x1B4332, 0x040D09, 0xCC3333, 0xFFFFFF, 0xFFFFFF, 0x000000,
		0x40916C, 0xFFFFFF, 0xB7E4C7, 0x2D6A4F, 0x52B788, 0xFFFFFF, 0x52796F
	},
	{ /* 4: Retro Amber */
		"Retro Amber",
		0x1A0F00, 0x0D0800, 0xFFA500, 0x5C3D00, 0x3A2500, 0xFFA500,
		0x2B1A00, 0x0D0800, 0xCC3333, 0xFFFFFF, 0xFFA500, 0x000000,
		0x5C3D00, 0xFFA500, 0xFFA500, 0x2B1A00, 0x7A5000, 0xFFA500, 0x5C3D00
	},
	{ /* 5: Neon Orange (cyberpunk) */
		"Neon Orange",
		0x0A0A0A, 0x141414, 0xFF6600, 0xFF4400, 0x331100, 0x000000,
		0xFF6600, 0x0A0A0A, 0xFF0000, 0x000000, 0xFF6600, 0x000000,
		0xFF4400, 0x000000, 0xFF6600, 0x1A1A1A, 0xFF6600, 0x000000, 0x662200
	},
	{ /* 6: Neon Blue (cyberpunk) */
		"Neon Blue",
		0x0A0A14, 0x0D0D1A, 0x00CCFF, 0x0066FF, 0x001A44, 0x00CCFF,
		0x0044AA, 0x0A0A14, 0xFF0044, 0xFFFFFF, 0x00CCFF, 0x000000,
		0x0066FF, 0x00CCFF, 0x00CCFF, 0x111122, 0x0066FF, 0x00CCFF, 0x003366
	},
	{ /* 7: Neon Red (cyberpunk) */
		"Neon Red",
		0x0A0A0A, 0x140A0A, 0xFF0044, 0xCC0033, 0x330011, 0xFF0044,
		0x660022, 0x0A0A0A, 0xFF0000, 0xFFFFFF, 0xFF0044, 0x000000,
		0xCC0033, 0xFF0044, 0xFF0044, 0x1A0A0A, 0xCC0033, 0xFF0044, 0x440011
	},
	{ /* 8: Neon Purple (cyberpunk) */
		"Neon Purple",
		0x0A0A14, 0x110A1A, 0xCC00FF, 0x8800CC, 0x220044, 0xCC00FF,
		0x550088, 0x0A0A14, 0xFF0066, 0xFFFFFF, 0xCC00FF, 0x000000,
		0x8800CC, 0xCC00FF, 0xCC00FF, 0x150A20, 0x8800CC, 0xCC00FF, 0x330055
	},
	{ /* 9: Neon Pink (synthwave) */
		"Neon Pink",
		0x0D0020, 0x150030, 0xFF69B4, 0xFF1493, 0x440044, 0xFF69B4,
		0x880066, 0x0D0020, 0xFF0066, 0xFFFFFF, 0xFF69B4, 0x000000,
		0xFF1493, 0x000000, 0xFF69B4, 0x1A0033, 0xFF1493, 0xFF69B4, 0x660044
	},
	{ /* 10: Cyber Yellow-Green */
		"Cyber Lime",
		0x0A0A0A, 0x0D1A0D, 0x00FF66, 0x009933, 0x002211, 0x00FF66,
		0x006633, 0x0A0A0A, 0xFF3300, 0xFFFFFF, 0x00FF66, 0x000000,
		0x009933, 0x00FF66, 0x00FF66, 0x0D1A0D, 0x009933, 0x00FF66, 0x004422
	},
	{ /* 11: Midnight Gold */
		"Midnight Gold",
		0x0D0D1A, 0x0A0A14, 0xFFD700, 0x997A00, 0x332200, 0xFFD700,
		0x665200, 0x0D0D1A, 0xCC3333, 0xFFFFFF, 0xFFD700, 0x000000,
		0x997A00, 0xFFD700, 0xFFD700, 0x111122, 0x997A00, 0xFFD700, 0x554400
	},
	/* Custom theme slots (initially empty — name NULL means unused) */
	{ 0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
	{ 0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
	{ 0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
	{ 0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 }
};
static int gui_current_theme = 0;
static int gui_theme_menu_open = 0; /* submenu state */

/* Active theme color accessors (read from current theme) */
#define GUI_COL_DESKTOP    gui_themes[gui_current_theme].desktop
#define GUI_COL_TASKBAR    gui_themes[gui_current_theme].taskbar
#define GUI_COL_TASK_TEXT  gui_themes[gui_current_theme].task_text
#define GUI_COL_TITLEBAR   gui_themes[gui_current_theme].titlebar
#define GUI_COL_TITLE_INAC gui_themes[gui_current_theme].title_inac
#define GUI_COL_TITLE_TEXT gui_themes[gui_current_theme].title_text
#define GUI_COL_BORDER     gui_themes[gui_current_theme].border
#define GUI_COL_WINBG      gui_themes[gui_current_theme].winbg
#define GUI_COL_CLOSE_BG   gui_themes[gui_current_theme].close_bg
#define GUI_COL_CLOSE_FG   gui_themes[gui_current_theme].close_fg
#define GUI_COL_CURSOR     gui_themes[gui_current_theme].cursor
#define GUI_COL_CURSOR_BRD gui_themes[gui_current_theme].cursor_brd
#define GUI_COL_START_BG   gui_themes[gui_current_theme].start_bg
#define GUI_COL_START_FG   gui_themes[gui_current_theme].start_fg
#define GUI_COL_CLOCK_FG   gui_themes[gui_current_theme].clock_fg
#define GUI_COL_MENU_BG    gui_themes[gui_current_theme].menu_bg
#define GUI_COL_MENU_HL    gui_themes[gui_current_theme].menu_hl
#define GUI_COL_MENU_TEXT  gui_themes[gui_current_theme].menu_text
#define GUI_COL_MENU_SEP   gui_themes[gui_current_theme].menu_sep

struct gui_window {
	int x, y;              /* pixel position, top-left */
	int w, h;              /* pixel size (total including decorations) */
	int visible;
	int minimized;
	int wtype;             /* GUI_WTYPE_TERMINAL, _PAINT, _EDITOR */
	char title[32];
	/* Terminal/Editor text buffer for this window */
	char text[GUI_WIN_TEXT_COLS * GUI_WIN_TEXT_ROWS];
	unsigned char attrs[GUI_WIN_TEXT_COLS * GUI_WIN_TEXT_ROWS];
	int text_col, text_row;   /* current cursor in text grid */
	int text_cols, text_rows; /* grid dimensions */
	int scroll_offset;
	/* Paint-specific state */
	unsigned int paint_color;   /* current drawing color */
	int paint_drawing;          /* mouse held down = drawing */
	int paint_tool;             /* GUI_PAINT_TOOL_* */
	int paint_brush_size;       /* brush radius: 1,2,3,5 */
	int paint_line_x0, paint_line_y0; /* line/rect start point */
	/* Editor-specific state */
	int editor_modified;        /* unsaved changes flag */
	char filepath[128];         /* editor: opened file path; explorer: cwd */
	/* Hex editor state */
	unsigned char hex_data[GUI_WIN_TEXT_COLS * GUI_WIN_TEXT_ROWS]; /* raw byte buffer */
	unsigned long hex_size;     /* number of bytes loaded */
	unsigned long hex_offset;   /* scroll offset in bytes (row-aligned) */
	unsigned long hex_cursor;   /* cursor position (byte index) */
	int hex_nibble;             /* 0=high nibble, 1=low nibble */
	/* Explorer-specific state */
	char explorer_names[GUI_EXPLORER_MAX][GUI_EXPLORER_NAME];
	int explorer_types[GUI_EXPLORER_MAX]; /* 1=dir, 0=file */
	int explorer_count;
	int explorer_selected;
	int explorer_scroll;
};

static int gui_active = 0;
static struct gui_window gui_windows[GUI_MAX_WINDOWS];
static int gui_window_count = 0;
static int gui_focused = -1;

/* Start menu state */
static int gui_start_menu_open = 0;

/* Forward declarations for GUI helpers */
static void gui_draw_start_menu(void);

/* Paint palette colors */
static const unsigned int gui_paint_palette[GUI_PAINT_COLORS] = {
	0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00,
	0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF,
	0x800000, 0x008000, 0x000080, 0x808000,
	0x800080, 0x008080, 0xC0C0C0, 0x808080
};

/* Paint pixel backing buffer (shared across all paint windows — only one paints at a time) */
static unsigned int gui_paint_pixels[GUI_PAINT_BUF_W * GUI_PAINT_BUF_H];
static int gui_paint_buf_owner = -1; /* index of the window using the buffer, or -1 */

/* Paint tool labels */
static const char *gui_paint_tool_names[GUI_PAINT_TOOL_COUNT] = {
	"Brush", "Line", "Rect", "Fill", "Eraser"
};

/* Mouse state for GUI */
static int gui_mouse_x = 0;
static int gui_mouse_y = 0;
static int gui_mouse_prev_x = 0;
static int gui_mouse_prev_y = 0;
static int gui_mouse_buttons = 0;
static int gui_mouse_prev_buttons = 0;
/* Save area under cursor */
static unsigned int gui_cursor_save[GUI_CURSOR_W * GUI_CURSOR_H];
static int gui_cursor_saved = 0;

/* Dragging state */
static int gui_drag_window = -1;
static int gui_drag_ox = 0;
static int gui_drag_oy = 0;

/* Resize state */
static int gui_resize_window = -1;
static int gui_resize_start_w = 0;
static int gui_resize_start_h = 0;
static int gui_resize_start_mx = 0;
static int gui_resize_start_my = 0;

static int vfs_prefer_fat_root = 0;
static char fat_cwd[128] = "/";
static int fat_mounted_drive_index = -1;
static int fat_active_drive_index = -1;  /* user's context drive; changes only via cd/fatmount */
static unsigned int fat_registered_drive_mask = 0;
static char fat_drive_cwd[BLOCKDEV_MAX_DRIVES][128] = {"/", "/", "/", "/"};
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

static void terminal_write_fat_failure(const char *prefix);
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
static int suppress_prompt = 0;
static int last_exit_code = 0;
static int run_source_mode = 0;
static char hostname_buf[32] = "tg11";
static char ps1_buf[64] = "> ";
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
static unsigned long autorun_boot_started_at = 0;
static int autorun_boot_clean_marked = 0;
static int autorun_safe_latched = 0;
static int script_last_error = 0;
static unsigned long script_last_error_line = 0;
static char script_last_error_text[96];
static int exec_trace_enabled = 0;
static unsigned char ramfs2fat_copy_buf[FS_MAX_FILE_SIZE];
static unsigned char fatstress_payload_buf[FATSTRESS_MAX_PAYLOAD];
static unsigned char fatstress_verify_buf[FATSTRESS_MAX_PAYLOAD];
static unsigned char elf_shell_io_buf[ELF_SHELL_IO_MAX];

/* ------------------------------------------------------------------ */
/* Background jobs                                                    */
/* ------------------------------------------------------------------ */
#define BG_JOB_MAX 4
struct bg_job {
	char cmd[INPUT_BUFFER_SIZE];
	int  task_id;
	int  active;
};
static struct bg_job bg_jobs[BG_JOB_MAX];
static int bg_job_count = 0;

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
static int fat_mount_drive_now(int drive_index, int enable_generic_mode);
static int parse_mount_target_drive(const char *token, int *out_drive_index);
static int parse_drive_prefixed_path(const char *input, int *out_drive_index, const char **out_rest, int *out_explicit);
static int parse_mountpoint_token(const char *token, int drive_index);
static void editor_handle_scancode(unsigned char scancode);
static void run_command(void);
static void run_command_single(void);
static void run_command_dispatch(void);
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
static int parse_fat_drive_index_arg(const char *args, int *out_drive_index, int *out_used_default);
static void cmd_fatwhere(void);
static void cmd_fatunmount(const char *args);
static void cmd_fatstress(const char *args);
static void cmd_fatperf(const char *args);
static const char *ata_drive_label(int drive_index);
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
static int confirm_dangerous_action(const char *args, const char *rerun_hint);
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
static void cmd_pause(const char *args);
static void cmd_wait(const char *args);
static void cmd_elfinfo(const char *args);
static void cmd_elfsegs(const char *args);
static void cmd_elfsects(const char *args);
static void cmd_elfsym(const char *args);
static void cmd_elfaddr(const char *args);
static void cmd_elfcheck(const char *args);
static void cmd_execstress(const char *args);
static void cmd_exectrace(const char *args);
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
static int terminal_read_config_text(const char *fat_path, const char *ram_path, const char **ram_text, unsigned char *fat_buf, unsigned long fat_buf_size, const char **out_text);
static void terminal_write_config_text(const char *fat_path, const char *ram_path, const char *text);
static int terminal_get_autorun_mode(void);
static void terminal_set_autorun_mode(int mode);
static int terminal_autorun_once_done(void);
static void terminal_set_autorun_once_done(int done);
static unsigned long terminal_get_autorun_delay_seconds(void);
static void terminal_set_autorun_delay_seconds(unsigned long seconds);
static int terminal_get_autorun_safe_mode(void);
static void terminal_set_autorun_safe_mode(int enabled);
static int terminal_get_autorun_clean_flag(void);
static void terminal_set_autorun_clean_flag(int clean);
static void terminal_get_autorun_last_status(char *out, unsigned long out_size);
static void terminal_set_autorun_last_status(const char *status);
static void terminal_get_autorun_last_source(char *out, unsigned long out_size);
static void terminal_set_autorun_last_source(const char *source);
static void terminal_autorun_boot_begin(void);
static void terminal_autorun_boot_heartbeat(void);
static void terminal_set_script_error(unsigned long line_no, const char *msg);
static int terminal_script_command_known(const char *line);
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
	terminal_write_line("  panic [yes]          - Deliberately trigger kernel panic");
	terminal_write_line("  shutdown/exit/quit   - Shut down");
	terminal_write_line("  pause                - Wait for one keypress to continue");
	terminal_write_line("  wait <seconds>       - Delay command flow for N seconds");
	terminal_write_line("  memmap               - Physical memory map");
	terminal_write_line("  memstat              - Allocator and paging summary");
	terminal_write_line("  pagetest             - Paging allocator self-test");
	terminal_write_line("  pagefault <mode> [yes] - Trigger PF: read|write|exec");
	terminal_write_line("  gpfault [yes]        - Trigger #GP using non-canonical address");
	terminal_write_line("  udfault [yes]        - Trigger #UD via UD2 instruction");
	terminal_write_line("  doublefault [yes]    - Simulate double-fault recovery");
	terminal_write_line("  exceptstat           - Show exception statistics");
	terminal_write_line("  dumpstack            - Dump current kernel call stack");
	terminal_write_line("  selftest exceptions [step] - Guided exception test harness");
	terminal_write_line("  elfinfo <path>       - Inspect ELF headers and symbol count");
	terminal_write_line("  elfsegs <path>       - Inspect ELF program headers only");
	terminal_write_line("  elfsects <path>      - Inspect ELF section headers only");
	terminal_write_line("  elfsym <path> [f]    - List ELF symbols, optional name filter");
	terminal_write_line("  elfaddr <p> <addr>   - Resolve address in file, or active image with 1 arg");
	terminal_write_line("  exec <path> [args...] - Load/call one ELF64 binary");
	terminal_write_line("  exectrace [on|off|show] - Toggle exec stack mapping diagnostics");
	terminal_write_line("  execstress <n> <path> - Repeat ELF run and show free-page delta");
	terminal_write_line("  elfselftest          - Run built-in ELF test matrix");
	terminal_write_line("  hexdump <a> [n]      - Hex dump memory");
}

static void print_help_fs(void)
{
	terminal_write_line("Filesystem commands:");
	terminal_write_line("  pwd                  - Print current directory");
	terminal_write_line("  ls [path]            - List entries");
	terminal_write_line("  dir [/b] [/w] [/s] [/rN] [path] - Directory listing (/rN sets FAT read batch)");
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
	terminal_write_line("  run [-x] [-t <sec>] <path> - Run script with optional timeout");
	terminal_write_line("  basic <path>         - Run Tiny BASIC program");
	terminal_write_line("  motd                 - Print MOTD with inline color/style tokens");
	terminal_write_line("  pause                - Wait for one keypress to continue");
	terminal_write_line("  wait <seconds>       - Delay command flow for N seconds");
	terminal_write_line("  autorun [show|log|off|always|once|rearm|stop|run|safe <on|off>|delay <sec>] - Boot script control");
	terminal_write_line("  elfinfo <path>       - Inspect ELF headers and symbol tables");
	terminal_write_line("  elfsegs <path>       - Inspect ELF program headers only");
	terminal_write_line("  elfsects <path>      - Inspect ELF section headers only");
	terminal_write_line("  elfsym <path> [f]    - List symbols from an ELF image");
	terminal_write_line("  elfaddr <p> <addr>   - Resolve address in file, or active image with 1 arg");
	terminal_write_line("  elfcheck <path>      - Validate ELF loadability without executing");
	terminal_write_line("  exec <path> [args...] - Load and run ELF64 kernel binary");
	terminal_write_line("  exectrace [on|off|show] - Toggle exec stack mapping diagnostics");
	terminal_write_line("  elfselftest          - Validate built-in ELF fixtures");
	terminal_write_line("  script: foreach i in a,b do echo $(i)");
	terminal_write_line("  fatmount <hd#> [/hd#|A:] - Register/mount FAT drive with root alias");
	terminal_write_line("  mtn|mnt|mount <hd#> [/hd#|A:] - Linux-style mount aliases");
	terminal_write_line("  fatwhere             - Show active + registered FAT mount roots");
	terminal_write_line("  drives               - List detected ATA drives");
	terminal_write_line("  ramfs                - Switch generic fs commands to RAM FS");
	terminal_write_line("  ramfs2fat [map]      - Copy RAM FS tree to FAT (or show name map)");
	terminal_write_line("  fatunmount [hd#|A:]  - Remove one mount root or unmount all");
	terminal_write_line("  umtn|umnt|umount [hd#|A:] - Linux-style unmount aliases");
	terminal_write_line("  fatls                - List FAT32 cwd");
	terminal_write_line("  fatcat <path>        - Read FAT32 file");
	terminal_write_line("  fattouch <path>      - Create FAT32 file");
	terminal_write_line("  fatwrite <p> <txt>   - Write FAT32 file");
	terminal_write_line("  fatstress [n] [-v] [-p <n>] [-s <bytes>] [-k <slots>] [-d <n>]");
	terminal_write_line("  fatperf [show|batch <1..16>|cache <on|off>|cache data|fat <on|off>|flush|dirbench [path] [list]]");
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
	{"pause", "wait for one keypress", "pause", "Prints a short prompt and blocks until a key is pressed. Useful in scripts and step-by-step demos, similar to Windows pause.", "pause", "wait run", "pause keypress script batch wait"},
	{"wait", "delay execution for N seconds", "wait <seconds>", "Sleeps for the requested number of seconds while still honoring control hotkeys such as Ctrl+C cancellation. Useful in scripts and autorun flows.", "wait 1\nwait 5", "pause run autorun", "sleep delay wait seconds script"},
	{"reboot", "restart the machine", "reboot", "Requests an immediate reboot.", "reboot", "shutdown", "restart reset power"},
	{"panic", "deliberately trigger a kernel panic", "panic [yes]", "Triggers an invalid opcode exception on purpose. Requires confirmation using the yes argument.", "panic yes", "reboot memstat", "crash panic test debug"},
	{"shutdown", "shut down the machine", "shutdown\nexit\nquit", "Stops the VM or machine using the supported power-off path.", "shutdown", "reboot", "power off quit exit"},
	{"pwd", "print the current working directory", "pwd", "Shows the shell's current working directory.", "pwd", "cd ls", "filesystem cwd path"},
	{"ls", "list directory entries", "ls [path]", "Lists files and directories from the current working directory or the given path.", "ls\nls /scripts", "cd pwd dir", "filesystem list directory"},
	{"dir", "show a Windows-style directory listing", "dir [/b] [/w] [/s] [/rN] [path]", "Prints one entry per line with a type column and file sizes, followed by summary totals. /b is bare names, /w is wide names, /s recursively lists each directory with its own totals plus a grand total, and /rN sets FAT multi-sector read batch size (for example /r8).", "dir\ndir /w\ndir /r8\ndir /s scripts", "ls tree cd pwd fatperf", "filesystem directory size windows dos perf"},
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
	{"grep", "search for a pattern in a file", "grep [-i] [-n] [-v] [-c] <pattern> <file>", "Searches for lines matching a text pattern. -i ignores case, -n shows line numbers, -v inverts match, -c prints only a count of matching lines. Reads from pipe if no file given.", "grep hello /notes.txt\nls | grep -c txt", "find cat wc", "search text filter match pattern"},
	{"find", "search for files by name", "find <pattern> [path]", "Searches directory trees for file names matching a pattern.", "find *.txt\nfind *.bas /scripts", "grep ls", "search file name glob wildcard"},
	{"wc", "count lines, words, and bytes", "wc [-l] [-w] [-c] <file>", "Counts lines, words, and bytes in a file or pipe. -l shows only line count, -w only word count, -c only byte count.", "wc /notes.txt\nls | wc -l", "cat grep head", "count lines words bytes size"},
	{"head", "show the first N lines", "head [-N] <file>", "Prints the first N lines of a file (default 10). Reads from pipe if no file given.", "head /notes.txt\nhead -5 /notes.txt\nls | head -3", "tail cat", "first lines top preview"},
	{"tail", "show the last N lines", "tail [-N] <file>", "Prints the last N lines of a file (default 10). Reads from pipe if no file given.", "tail /log.txt\ntail -5 /log.txt", "head cat", "last lines bottom end"},
	{"xxd", "hex dump a file", "xxd <file>", "Displays the contents of a file as a hex dump with ASCII sidebar. Reads from pipe if no file given.", "xxd /kernel.bin", "hexdump hexedit", "hex dump binary file bytes"},
	{"run", "execute a shell script file", "run [-x] [-t <1..3600>] <path>", "Runs one script file through the shell. -x echoes each line before executing it, and -t sets a per-script timeout in seconds. Script errors now report line numbers for control-flow mistakes, cancel, timeout, and unknown commands.", "run boot.scr\nrun -x test.scr\nrun -t 20 boot.scr", "basic edit autorun", "script shell batch timeout diagnostics"},
	{"motd", "print the message of the day", "motd", "Prints the boot message text from /motd.txt on FAT when present, otherwise from /etc/motd.txt in RAM FS. MOTD rendering supports the same inline color and style tokens as echo, including &NN and &r.", "motd", "echo autorun", "motd boot banner colors"},
	{"autorun", "configure the delayed boot script runner", "autorun [show|log|off|always|once|rearm|stop|run|safe <on|off|show>|delay <0..3600>]", "Controls whether /autorun.sh on FAT, or /etc/autorun.sh in RAM FS, executes after boot. show reports current mode, safe mode, delay, pending state, last status, and source. log shows the last execution status/source. safe enables the crash guard that skips autorun after an unclean reboot until the system survives the clean-boot window.", "autorun show\nautorun always\nautorun delay 10\nautorun safe on\nautorun log", "run motd fatmount", "autorun boot script startup safe mode delay"},
	{"basic", "run a Tiny BASIC program", "basic <path>", "Loads and runs a Tiny BASIC program from the filesystem.", "basic scripts/tic-tac-toe.bas", "run edit", "basic interpreter program"},
	{"elfinfo", "inspect ELF64 headers and symbol metadata", "elfinfo <path>", "Parses an ELF64 file without executing it and prints header fields, PT_LOAD range, and how many named defined symbols were found.", "elfinfo /app.elf", "elfsym elfaddr exec", "elf symbols debug headers metadata"},
	{"elfsegs", "list ELF64 program headers", "elfsegs <path>", "Prints each program header with vaddr, offset, file size, memory size, alignment, and load-validation hints.", "elfsegs /app.elf", "elfinfo elfsym exec", "elf program headers segments load layout"},
	{"elfsects", "list ELF64 section headers", "elfsects <path>", "Prints section headers with name, type, address, offset, size, and links to help inspect ELF layout and symbols.", "elfsects /app.elf", "elfinfo elfsegs elfsym", "elf sections shdr layout debug"},
	{"elfsym", "list named ELF64 symbols", "elfsym <path> [filter]", "Lists named defined symbols from SHT_SYMTAB or SHT_DYNSYM. Supply an optional filter substring to narrow the output.", "elfsym /app.elf\nelfsym /kernel/app.elf start", "elfinfo elfaddr exec", "elf symbols symtab debug lookup"},
	{"elfaddr", "resolve an address to the nearest ELF64 symbol", "elfaddr <path> <hex-address> | elfaddr <hex-address>", "Looks up the nearest named symbol at or before the given virtual address. With one argument, searches all currently active loaded ELF images.", "elfaddr /app.elf 0xFFFF900003000000\nelfaddr 0xFFFF900003000000", "elfinfo elfsym exec", "elf address symbol resolve debug"},
	{"elfcheck", "validate ELF64 loadability", "elfcheck <path>", "Loads and immediately unloads an ELF image to verify loader compatibility and report detailed load error names without executing entry code.", "elfcheck /app.elf", "elfinfo exec execstress", "elf validation loader sanity check"},
	{"exec", "load and call an ELF64 kernel binary", "exec <path> [args...]", "Loads a small ELF64 image into kernel memory, maps its PT_LOAD segments, builds argc/argv, and calls its entry point directly.", "exec app.elf\nexec app.elf --mode test", "memstat pagetest", "elf executable loader"},
	{"exectrace", "toggle exec mapping diagnostics", "exectrace [on|off|show]", "Controls extra trace output from the exec stack mapper and argv stack setup path. Keep off for normal use, turn on while debugging user stack setup failures.", "exectrace show\nexectrace on\nexec /hello.elf\nexectrace off", "exec elfcheck memstat", "exec trace debug diagnostics stack mapping"},
	{"execstress", "run one ELF repeatedly and check memory delta", "execstress <count> <path>", "Loads, executes, and unloads the same ELF image count times, then reports free-page delta to help detect leaks while iterating on ELF/memory changes.", "execstress 100 app.elf", "exec memstat pagetest", "elf stress test memory leak"},
	{"elfselftest", "run built-in ELF functional and leak tests", "elfselftest", "Runs /app.elf, /appw.elf, and /app2p.elf checks (return values + load/unload stability) and performs a short stress loop with free-page delta reporting.", "elfselftest", "exec execstress memstat", "elf selftest memory loader"},
	{"hexdump", "dump memory in hex", "hexdump <address> [count]", "Reads memory starting from a hex address and shows a hex dump.", "hexdump 0x100000 64", "hexedit memmap", "memory dump debug"},
	{"memmap", "show the physical memory map", "memmap", "Displays the multiboot-provided physical memory map.", "memmap", "memstat pagetest", "memory multiboot map"},
	{"memstat", "show allocator and paging state", "memstat", "Displays total and free pages, the virtual allocation window, and the active CR3 value.", "memstat", "memmap pagetest exec", "memory paging allocator cr3"},
	{"pagetest", "exercise the paging allocator", "pagetest", "Allocates pages, writes a pattern, verifies it, unmaps it, and confirms the mapping is gone.", "pagetest", "memstat exec", "paging allocator self test"},
	{"pagefault", "trigger a controlled page fault", "pagefault <read|write|exec> [yes]", "Intentionally faults using an unmapped address. read dereferences it, write stores through it, and exec calls it as code so you can verify decoded #PF bits on the panic screen. Requires confirmation using yes.", "pagefault read yes\npagefault write yes\npagefault exec yes", "panic pagetest memstat", "page fault pf cr2 panic test"},
	{"gpfault", "trigger a controlled general-protection fault", "gpfault [yes]", "Triggers #GP by dereferencing a non-canonical address in long mode. Requires confirmation using yes.", "gpfault yes", "udfault pagefault panic", "general protection fault gp exception test"},
	{"udfault", "trigger a controlled invalid-opcode fault", "udfault [yes]", "Executes UD2 to intentionally trigger #UD. Requires confirmation using yes.", "udfault yes", "gpfault pagefault panic", "invalid opcode ud exception test"},
	{"doublefault", "simulate double-fault recovery", "doublefault [yes]", "Simulates a double fault by directly invoking the recovery handler. A double fault occurs when an exception happens while handling another exception. Requires confirmation using yes.", "doublefault yes", "udfault gpfault panic", "double fault df exception recovery test"},
	{"exceptstat", "display exception statistics", "exceptstat", "Shows the count of each exception type that has occurred since system boot. Useful for detecting repeated faults under load.", "exceptstat", "panic udfault gpfault", "exception statistics counter tracking diagnostics"},
	{"dumpstack", "dump current kernel call stack", "dumpstack", "Walks the RBP chain to display the current function call stack. Useful for runtime diagnostics before intentional faults.", "dumpstack", "exceptstat panic", "stack dump backtrace call chain"},
	{"selftest", "run guided built-in test suites", "selftest exceptions [pf-read|pf-write|pf-exec|ud|gp|1|2|3|4|5]", "Prints a reboot-friendly exception test plan, or runs one selected step and triggers the expected fault immediately.", "selftest exceptions\nselftest exceptions pf-exec\nselftest exceptions 4", "pagefault gpfault udfault panic", "selftest test harness exceptions diagnostics"},
	{"ataid", "show ATA identity information", "ataid", "Detects ATA drives and shows the available sector count.", "ataid", "drives readsec writesec", "ata disk identity"},
	{"readsec", "dump one 512-byte sector", "readsec <lba-hex>", "Reads and dumps one sector from the selected ATA device.", "readsec 0x20", "writesec ataid", "ata sector read"},
	{"writesec", "write text into one sector", "writesec <lba> <text>", "Writes marker text into a single sector for low-level disk testing.", "writesec 32 hello", "readsec ataid", "ata sector write"},
	{"drives", "list detected ATA drives", "drives", "Enumerates the ATA drives seen by the kernel.", "drives", "ataid fatmount", "ata disk list"},
	{"fatmount", "register/mount a FAT32 drive with aliases", "fatmount <0..3|hd0..hd3> [/hd#|A:]", "Registers a FAT drive so it can be addressed by /hdN/... or A:/... style prefixes. The command mounts the selected drive immediately, but previously registered drives remain available by prefix.", "fatmount hd1\nfatmount hd2 /hd2\nfatmount hd0 A:", "fatunmount fatwhere drives ramfs", "fat32 mount roots hd0 hd1 hd2 hd3 A:"},
	{"mtn", "Linux-style FAT mount alias", "mtn <hd0|hd1|hd2|hd3> [/hd#|A:]", "Alias for fatmount using Linux-like drive names.", "mtn hd1\nmtn hd3 /hd3", "fatmount mnt mount fatwhere drives", "mount mtn fat32 hd0 hd1 hd2 hd3"},
	{"mnt", "short Linux-style FAT mount alias", "mnt <hd0|hd1|hd2|hd3> [/hd#|A:]", "Short alias for fatmount. Equivalent to mtn.", "mnt hd1", "mtn mount fatmount fatwhere", "mount mnt fat32 hd0 hd1 hd2 hd3"},
	{"mount", "long Linux-style FAT mount alias", "mount <hd0|hd1|hd2|hd3> [/hd#|A:]", "Long alias for fatmount. Equivalent to mtn/mnt.", "mount hd2 /hd2", "mtn mnt fatmount fatwhere", "mount fat32 hd0 hd1 hd2 hd3"},
	{"fatwhere", "show active and registered FAT mount roots", "fatwhere", "Reports the currently active FAT drive and all registered drive prefixes (/hdN and A: style roots).", "fatwhere", "fatmount fatunmount drives", "fat32 mount target diagnostics roots"},
	{"fatunmount", "remove FAT mount roots", "fatunmount [hd#|A:]", "Without an argument, unmounts FAT and clears all registered roots. With hd#/A:, removes only that drive root and keeps others registered.", "fatunmount\nfatunmount hd2\nfatunmount A:", "fatmount ramfs", "fat32 unmount disk roots"},
	{"umtn", "Linux-style FAT unmount alias", "umtn [hd#|A:]", "Alias for fatunmount.", "umtn\numtn hd2", "umnt umount fatunmount mtn fatmount", "unmount umount fat32"},
	{"umnt", "short Linux-style FAT unmount alias", "umnt [hd#|A:]", "Short alias for fatunmount. Equivalent to umtn.", "umnt A:", "umtn umount fatunmount", "unmount umnt fat32"},
	{"umount", "long Linux-style FAT unmount alias", "umount [hd#|A:]", "Long alias for fatunmount. Equivalent to umtn/umnt.", "umount hd1", "umtn umnt fatunmount", "unmount umount fat32"},
	{"ramfs", "switch generic file commands back to RAM FS", "ramfs", "Makes generic file commands operate on the RAM filesystem again instead of the FAT volume.", "ramfs", "fatmount ramfs2fat", "ramfs filesystem switch"},
	{"ramfs2fat", "copy the RAM FS tree to FAT32", "ramfs2fat [map]", "Copies the RAM filesystem tree onto the mounted FAT32 volume or prints the filename mapping when map is supplied.", "ramfs2fat\nramfs2fat map", "ramfs fatmount", "ramfs fat32 copy sync"},
	{"fatls", "list FAT32 directory entries", "fatls", "Lists the contents of the current FAT32 working directory.", "fatls", "fatcat fattouch fatwrite", "fat32 list directory"},
	{"fatcat", "print one FAT32 file", "fatcat <path>", "Reads and prints a file from the mounted FAT32 volume.", "fatcat /notes.txt", "fatwrite fattouch", "fat32 file read"},
	{"fattouch", "create an empty FAT32 file", "fattouch <path>", "Creates a zero-length file on the mounted FAT32 volume.", "fattouch /new.txt", "fatwrite fatcat", "fat32 file create"},
	{"fatwrite", "replace a FAT32 file with text", "fatwrite <path> <text>", "Writes text into a FAT32 file, replacing the previous contents.", "fatwrite /note.txt hello", "fattouch fatcat", "fat32 file write"},
	{"fatstress", "stress-test FAT read/write integrity", "fatstress [rounds] [-v] [-p <n>] [-s <bytes>] [-k <slots>] [-d <n>]", "Runs repeated write-read-verify cycles on rotating FAT files and periodic deletes to exercise allocation/reuse paths. Optional rounds range: 1..100000. -v enables trace logs; -p sets progress interval; -s sets payload bytes (1..4096); -k sets rotating slot count (1..9999); -d sets delete interval in rounds (1..100000).", "fatstress\nfatstress 2000 -v\nfatstress 5000 -v -p 250 -s 512 -k 256 -d 16", "fatwrite fatcat fatrm", "fat32 stress test integrity fsck debug trace payload"},
	{"fatperf", "tune FAT32 read batching and caches", "fatperf [show|batch <1..16>|cache <on|off>|cache data|fat <on|off>|flush|dirbench [path] [list]]", "Shows or changes FAT32 read-performance settings. batch sets ATA multi-sector read size used by directory and file reads. cache toggles both caches together, or data/fat cache separately. flush clears caches, and dirbench runs dir /b repeatedly with multiple batch sizes and prints tick deltas.", "fatperf show\nfatperf batch 8\nfatperf cache fat off\nfatperf flush\nfatperf dirbench / 1,2,4,8,16", "dir fatls fatmount", "fat32 performance benchmark cache batch tuning"},
	{"fatattr", "inspect or modify FAT32 attributes", "fatattr <path> [mods]", "Shows or changes FAT attribute bits like read-only, hidden, system, and archive.", "fatattr /note.txt\nfatattr /note.txt +r", "fatls fatrm", "fat32 attributes readonly hidden"},
	{"fatrm", "remove a FAT32 file or directory", "fatrm <path>", "Deletes a file or directory from the mounted FAT32 volume.", "fatrm /note.txt", "fatls fatattr", "fat32 remove delete"},
	{"netinfo", "show network configuration", "netinfo", "Displays the current IP address, gateway, netmask, DNS server, MAC address, and link status.", "netinfo", "netreinit dhcp ping", "network ip mac gateway dns link status"},
	{"netreinit", "reinitialise the network stack", "netreinit", "Re-scans the PCI bus, reinitialises the E1000 NIC driver, and resets the IP configuration to defaults.", "netreinit", "netinfo dhcp", "network driver pci e1000 reset"},
	{"ping", "send ICMP echo requests", "ping [-c count] <ip>", "Sends ICMP echo requests to the specified IP address. Use -c to set the number of packets (default 4, max 100).", "ping 10.0.2.2\nping -c 10 10.0.2.2", "netinfo arp", "network icmp echo rtt latency"},
	{"arp", "display the ARP cache", "arp", "Shows known IP-to-MAC address mappings from the ARP cache.", "arp", "ping netinfo", "network arp mac address cache"},
	{"udpsend", "send a UDP datagram", "udpsend <ip> <port> <message>", "Sends a text message as a UDP datagram to the specified IP and port.", "udpsend 10.0.2.2 9999 hello", "ping netinfo", "network udp send packet datagram"},
	{"dhcp", "obtain an IP lease via DHCP", "dhcp", "Broadcasts a DHCP Discover, waits for an Offer, sends a Request, and applies the Ack. Updates IP, gateway, netmask, and DNS server.", "dhcp", "netinfo nslookup", "network dhcp ip lease address automatic"},
	{"nslookup", "resolve a hostname via DNS", "nslookup <hostname>", "Sends a DNS A-record query to the configured DNS server and displays the resolved IPv4 address.", "nslookup example.com", "dhcp wget", "network dns resolve hostname address"},
	{"wget", "make an HTTP GET request via TCP", "wget <ip> <port> [path]", "Opens a TCP connection to ip:port, sends an HTTP/1.0 GET request for the specified path, receives and displays the response.", "wget 10.0.2.2 80 /\nwget 93.184.216.34 80 /index.html", "nslookup dhcp", "network tcp http get request web"},
	{"date", "show the current date and time", "date", "Reads the CMOS real-time clock and displays the current date in a human-readable format.", "date", "uptime ticks", "rtc clock time calendar cmos"},
	{"uptime", "show system uptime", "uptime", "Displays how long the system has been running since boot, based on the kernel tick counter.", "uptime", "date ticks", "time boot running duration"},
	{"jobs", "list background jobs", "jobs", "Displays all currently active background jobs launched with the & operator.", "jobs", "taskkill tasks", "background process list running"},
	{"sleep", "delay execution for N seconds", "sleep <seconds>", "Alias for 'wait'. Sleeps for the requested number of seconds.", "sleep 3", "wait pause", "delay timer seconds"},
	{"env", "list environment variables", "env", "Displays all defined shell variables. Alias for 'set' with no arguments.", "env", "set", "variables environment print list"},
	{"sort", "sort lines of text", "sort [-r] [file]", "Sorts lines alphabetically. Use -r for reverse order. Reads from pipe if no file given.", "sort /notes.txt\nls | sort -r", "uniq head tail", "text lines alphabetical order"},
	{"uniq", "filter duplicate adjacent lines", "uniq [file]", "Removes consecutive duplicate lines. Reads from pipe if no file given.", "sort /data.txt | uniq", "sort grep", "duplicate filter unique lines"},
	{"tee", "copy pipe input to a file and stdout", "tee <file>", "Writes pipe input to both the terminal and the specified file.", "ls | tee /listing.txt", "cat sort", "pipe split output file copy"},
	{"tr", "translate characters", "tr <set1> <set2>", "Replaces each character in set1 with the corresponding character in set2. Reads from pipe.", "echo hello | tr elo ELO", "sed grep", "character replace translate"},
	{"seq", "print a sequence of numbers", "seq [start] <end> [step]", "Prints numbers from start to end. Default start is 1, default step is 1.", "seq 5\nseq 2 10\nseq 1 20 3", "calc", "numbers range count sequence"},
	{"diff", "compare two files line by line", "diff <file1> <file2>", "Shows lines that differ between the two files with line numbers.", "diff /a.txt /b.txt", "cmp cat", "compare difference text files"},
	{"cmp", "compare two files byte by byte", "cmp <file1> <file2>", "Reports the first byte where the files differ, or if one file is shorter.", "cmp /a.bin /b.bin", "diff xxd", "compare binary byte files"},
	{"calc", "evaluate arithmetic expression", "calc <expression>", "Evaluates an integer arithmetic expression with +, -, *, /, %, and parentheses. Supports variables.", "calc 2+3*4\ncalc (100-30)/7", "set seq", "math arithmetic expression evaluate"},
	{"whoami", "print current user name", "whoami", "Displays the current user (always root on TG11-OS).", "whoami", "hostname", "user identity name"},
	{"hostname", "show or set the system hostname", "hostname [name]", "With no arguments, prints the hostname. With an argument, sets it.", "hostname\nhostname mypc", "whoami", "system name host machine"},
	{"history", "show command history", "history", "Displays the numbered list of recently entered commands. Use !! to repeat the last command, or !N to repeat command number N.", "history\n!!\n!3", "set env", "history recall repeat bang expansion"},
	{"unset", "remove a shell variable", "unset <variable>", "Removes the named variable from the shell environment.", "unset myvar", "set env", "variable remove delete environment"},
	{"source", "execute a script in the current shell", "source <file>", "Runs script commands in the current shell, preserving variable changes. Alias: . <file>", "source /init.sh\n. /config.sh", "run set", "script execute dot include"},
	{"read", "read user input into a variable", "read <variable>", "Prompts for a line of input and stores it in the named shell variable.", "read name", "set echo", "input prompt variable"},
	{"df", "show filesystem free space", "df", "Displays the free space on the mounted FAT32 volume in kilobytes.", "df", "stat mount", "disk free space filesystem"},
	{"stat", "show file information", "stat <path>", "Displays file type and size for the given path.", "stat /notes.txt", "ls df", "file information size type"},
	{"cut", "extract fields from lines", "cut -d<delim> -f<field> [file]", "Splits each input line on a delimiter and prints the requested field number. Reads from pipe if no file given.", "cut -d: -f2 /etc/passwd\nls | cut -d. -f1", "tr grep", "field column delimiter split extract"},
	{"rev", "reverse lines of text", "rev [file]", "Reverses each line character by character. Reads from pipe if no file given.", "rev /notes.txt\necho hello | rev", "tr sort", "reverse mirror text characters"},
	{"printf", "formatted output", "printf <format> [args...]", "Prints formatted text. Supports %s, %d, \\n, \\t escapes.", "printf \"Hello %s\\n\" world\nprintf \"%d + %d = %d\\n\" 1 2 3", "echo", "format print string output"},
	{"true", "return success", "true", "Does nothing and returns exit code 0 (success). Useful in shell conditionals and loops.", "true && echo yes\nwhile true; do echo loop; done", "false test", "success zero exit"},
	{"false", "return failure", "false", "Does nothing and returns exit code 1 (failure). Useful in shell conditionals.", "false || echo failed\nfalse && echo never", "true test", "failure nonzero exit"},
	{"test", "evaluate conditional expression", "test <expr>  or  [ <expr> ]", "Evaluates a conditional expression and sets exit code to 0 (true) or 1 (false). Supports: -e/-f/-d (file tests), -z/-n (string tests), =/!= (string compare), -eq/-ne/-lt/-gt/-le/-ge (numeric compare), ! (negation).", "test -f /hello.txt && echo exists\n[ 5 -gt 3 ] && echo yes\ntest -z \"\" && echo empty", "true false", "if condition compare check bracket"},
	{"which", "show command location", "which <command>", "Shows whether a command is a shell builtin or alias. Reports 'not found' for unknown commands.", "which ls\nwhich help", "type alias", "find command builtin lookup"},
	{"type", "describe command type", "type <command>", "Describes how a command name would be interpreted. Shows if it is a builtin, alias (with its expansion), or unknown.", "type echo\ntype ls", "which alias", "identify command builtin alias"},
	{"basename", "strip directory from path", "basename <path>", "Prints the filename portion of a path, stripping any leading directory components.", "basename /home/user/file.txt\nbasename /etc/", "dirname", "path filename strip directory"},
	{"dirname", "strip filename from path", "dirname <path>", "Prints the directory portion of a path, stripping the final component.", "dirname /home/user/file.txt\ndirname ./script.sh", "basename", "path directory parent strip"},
	{"yes", "output repeated string", "yes [string]", "Outputs a string repeatedly (default 'y'). Limited to 100 lines to prevent hangs. Useful with pipes.", "yes | head -5\nyes hello | head -3", "echo seq", "repeat loop string"},
	{"nl", "number lines", "nl [file]", "Adds line numbers to each line of input. Numbers are right-justified in a 6-character field. Reads from pipe if no file given.", "nl /hello.txt\nls | nl", "cat wc head", "number lines count prefix"},
	{"factor", "prime factorization", "factor <number>", "Prints the prime factorization of a positive integer.", "factor 120\nfactor 97", "calc seq", "prime factor math number decompose"},
	{"du", "disk usage", "du [path]", "Reports disk usage for files in a directory. Shows size in KB for each file and a total.", "du\ndu /mydir", "df stat ls", "disk usage size space files"},
	{"xargs", "build commands from pipe input", "xargs <command>", "Reads lines from pipe input and runs the specified command once per line, appending the line as an argument.", "ls | xargs cat\nseq 3 | xargs echo", "grep find", "pipe input lines execute command"},
	{"less", "page through file", "less <file>", "Interactive pager. Shows a file one page at a time. Keys: j/Down=scroll down, k/Up=scroll up, Space=page down, b=page up, g/Home=top, G/End=bottom, q/Esc=quit.", "less /hello.txt", "more cat head tail", "pager scroll view page"},
	{"more", "page through file (alias for less)", "more <file>", "Alias for less. Interactive pager for viewing files one page at a time.", "more /hello.txt", "less cat", "pager scroll view page"},
	{"tac", "reverse lines of file", "tac <file>", "Prints lines of a file in reverse order (last line first). Reads from pipe input if no file given.", "tac /hello.txt\nseq 5 | tac", "rev cat", "reverse lines backward"},
	{"expr", "evaluate expression", "expr <expression>", "Evaluates an arithmetic expression and prints the result. Supports +, -, *, /, %, parentheses, and nested expressions.", "expr 2+3*4\nexpr (10-3)/2", "calc", "math arithmetic evaluate calculate"},
	{"watch", "run command repeatedly", "watch [-n <sec>] <command>", "Runs a command every N seconds (default 2), clearing the screen each time. Press q or Esc to stop.", "watch date\nwatch -n 5 uptime", "time sleep", "repeat loop monitor periodic"},
	{"paste", "merge lines from files", "paste <file1> <file2> [-d <delim>]", "Merges lines from two files side by side, separated by a delimiter (default: two spaces).", "paste a.txt b.txt\npaste -d , a.txt b.txt", "cut column", "merge join lines side combine"},
	{"column", "columnate output", "column [file]", "Formats input into columns based on terminal width. Words are aligned in evenly-spaced columns.", "ls | column\ncolumn names.txt", "paste cut", "format align table columns layout"},
	{"strings", "extract printable strings", "strings [-n <len>] <file>", "Finds and prints sequences of printable characters (min 4 by default) in a file. Useful for inspecting binary files.", "strings /kernel.elf\nstrings -n 8 data.bin", "xxd hexdump", "binary printable text extract"},
	{"rmdir", "remove empty directory", "rmdir <directory>", "Removes an empty directory. Fails if the directory contains any files or subdirectories.", "rmdir /mydir\nrmdir /tmp/empty", "mkdir rm", "directory remove delete empty folder"},
	{"time", "time a command", "time <command>", "Measures how long a command takes to execute and displays elapsed wall-clock time.", "time ls\ntime sort -n data.txt", "watch ticks", "benchmark duration elapsed measure performance"},
	{"gui", "start desktop GUI", "gui", "Launches a graphical desktop environment with a terminal window. Requires framebuffer mode. Use 'display fb' first if in text mode. Esc exits back to text mode.", "gui", "display", "desktop window mouse graphical interface"}
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
	if (terminal_output_capture && terminal_output_buf != (void *)0)
	{
		if (terminal_output_buf_len + 1 < terminal_output_buf_size)
		{
			terminal_output_buf[terminal_output_buf_len++] = c;
			terminal_output_buf[terminal_output_buf_len] = '\0';
		}
		return;
	}

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
	if (suppress_prompt) return;
	screen_set_color(terminal_prompt_color);
	/* Expand PS1 tokens: \u=user, \h=hostname, \w=cwd, \$=$ */
	{
		const char *s = ps1_buf;
		while (*s)
		{
			if (*s == '\\' && s[1] == 'u') { terminal_write("root"); s += 2; }
			else if (*s == '\\' && s[1] == 'h') { terminal_write(hostname_buf); s += 2; }
			else if (*s == '\\' && s[1] == 'w')
			{
				if (fat_mode_active())
					terminal_write(fat_cwd);
				else
				{
					char pwd_buf[256];
					fs_get_pwd(pwd_buf, sizeof(pwd_buf));
					terminal_write(pwd_buf);
				}
				s += 2;
			}
			else if (*s == '\\' && s[1] == '$') { terminal_putc('$'); s += 2; }
			else { terminal_putc(*s); s++; }
		}
	}
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

void *memcpy(void *dst, const void *src, unsigned long n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	while (n--) *d++ = *s++;
	return dst;
}

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

static int fat_mount_drive_now(int drive_index, int enable_generic_mode)
{
	struct block_device *dev;
	int old_drive;

	if (drive_index < 0 || drive_index >= BLOCKDEV_MAX_DRIVES) return -1;
	dev = blockdev_get(drive_index);
	if (dev == (void *)0 || !dev->present) return -1;

	old_drive = fat_mounted_drive_index;
	if (old_drive == drive_index && fat32_is_mounted())
	{
		if (enable_generic_mode) vfs_prefer_fat_root = 1;
		return 0;
	}

	if (old_drive >= 0 && old_drive < BLOCKDEV_MAX_DRIVES && fat32_is_mounted())
	{
		unsigned long i = 0;
		while (fat_cwd[i] != '\0' && i + 1 < sizeof(fat_drive_cwd[0])) { fat_drive_cwd[old_drive][i] = fat_cwd[i]; i++; }
		fat_drive_cwd[old_drive][i] = '\0';
	}

	if (fat32_mount(dev) != 0) return -1;
	fat_mounted_drive_index = drive_index;
	if (enable_generic_mode) vfs_prefer_fat_root = 1;

	if (fat_drive_cwd[drive_index][0] == '\0')
	{
		fat_drive_cwd[drive_index][0] = '/';
		fat_drive_cwd[drive_index][1] = '\0';
	}
	{
		unsigned long i = 0;
		while (fat_drive_cwd[drive_index][i] != '\0' && i + 1 < sizeof(fat_cwd)) { fat_cwd[i] = fat_drive_cwd[drive_index][i]; i++; }
		fat_cwd[i] = '\0';
	}
	return 0;
}

static int parse_mount_target_drive(const char *token, int *out_drive_index)
{
	char tok[16];
	unsigned long i = 0;

	if (token == (void *)0 || out_drive_index == (void *)0) return -1;
	while (token[i] != '\0' && i + 1 < sizeof(tok))
	{
		tok[i] = ascii_lower(token[i]);
		i++;
	}
	if (token[i] != '\0') return -1;
	tok[i] = '\0';

	if ((tok[0] == '/' && tok[1] == 'h' && tok[2] == 'd' && tok[3] >= '0' && tok[3] <= '3' && tok[4] == '\0') ||
		(tok[0] == 'h' && tok[1] == 'd' && tok[2] >= '0' && tok[2] <= '3' && tok[3] == '\0'))
	{
		*out_drive_index = (tok[0] == '/') ? (tok[3] - '0') : (tok[2] - '0');
		return 0;
	}
	if (tok[0] >= '0' && tok[0] <= '3' && tok[1] == '\0')
	{
		*out_drive_index = tok[0] - '0';
		return 0;
	}
	if (tok[1] == ':' && tok[2] == '\0')
	{
		char c = tok[0];
		if (c >= 'a' && c <= 'd')
		{
			*out_drive_index = c - 'a';
			return 0;
		}
	}

	return -1;
}

static int parse_mountpoint_token(const char *token, int drive_index)
{
	int mapped_drive = -1;
	if (parse_mount_target_drive(token, &mapped_drive) != 0) return -1;
	if (drive_index >= 0 && mapped_drive != drive_index) return -1;
	return 0;
}

static int parse_drive_prefixed_path(const char *input, int *out_drive_index, const char **out_rest, int *out_explicit)
{
	const char *p;
	char token[16];
	unsigned long n = 0;
	int drive_index;

	if (out_drive_index == (void *)0 || out_rest == (void *)0 || out_explicit == (void *)0) return -1;
	*out_drive_index = -1;
	*out_rest = input;
	*out_explicit = 0;

	if (input == (void *)0 || input[0] == '\0') return 0;

	if (input[1] == ':' && input[0] != '\0')
	{
		token[0] = input[0];
		token[1] = ':';
		token[2] = '\0';
		if (parse_mount_target_drive(token, &drive_index) != 0) return -1;
		p = input + 2;
		if (*p == '/' || *p == '\\') p++;
		*out_drive_index = drive_index;
		*out_rest = p;
		*out_explicit = 1;
		return 0;
	}

	if (input[0] == '/' || input[0] == '\\')
	{
		p = input + 1;
		while (*p != '\0' && *p != '/' && *p != '\\')
		{
			if (n + 1 >= sizeof(token)) return -1;
			token[n++] = *p++;
		}
		token[n] = '\0';
		if (token[0] == '\0') return 0;
		if (parse_mount_target_drive(token, &drive_index) != 0) return 0;
		if (*p == '/' || *p == '\\') p++;
		*out_drive_index = drive_index;
		*out_rest = p;
		*out_explicit = 1;
		return 0;
	}

	return 0;
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
	int explicit_drive = -1;
	int explicit_prefix = 0;

	if (input == (void *)0 || out == (void *)0) return -1;

	if (parse_drive_prefixed_path(input, &explicit_drive, &p, &explicit_prefix) != 0) return -1;
	if (explicit_prefix)
	{
		if ((fat_registered_drive_mask & (1U << (unsigned int)explicit_drive)) == 0U) return -1;
		if (fat_mount_drive_now(explicit_drive, 1) != 0) return -1;
	}
	else
	{
		/* No drive prefix — resolve on the user's active drive, not whatever is physically mounted. */
		int active = (fat_active_drive_index >= 0) ? fat_active_drive_index : fat_mounted_drive_index;
		if (active >= 0 && (fat_mounted_drive_index != active || !fat32_is_mounted()) &&
			(fat_registered_drive_mask & (1U << (unsigned int)active)) != 0U)
		{
			if (fat_mount_drive_now(active, 1) != 0) return -1;
		}
		else if (!fat32_is_mounted())
		{
			int i;
			int fallback = -1;
			for (i = 0; i < BLOCKDEV_MAX_DRIVES; i++)
			{
				if ((fat_registered_drive_mask & (1U << (unsigned int)i)) != 0U)
				{
					fallback = i;
					break;
				}
			}
			if (fallback < 0 || fat_mount_drive_now(fallback, 1) != 0) return -1;
		}
	}

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

	if (!explicit_prefix && p[0] != '/' && p[0] != '\\')
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
	if (fs_read_text(AUTORUN_SAFE_MODE_PATH, &existing) != 0)
	{
		fs_write_text(AUTORUN_SAFE_MODE_PATH, "on\n");
	}
	if (fs_read_text(AUTORUN_CLEAN_PATH, &existing) != 0)
	{
		fs_write_text(AUTORUN_CLEAN_PATH, "1\n");
	}
	if (fs_read_text(AUTORUN_LAST_STATUS_PATH, &existing) != 0)
	{
		fs_write_text(AUTORUN_LAST_STATUS_PATH, "idle\n");
	}
	if (fs_read_text(AUTORUN_LAST_SOURCE_PATH, &existing) != 0)
	{
		fs_write_text(AUTORUN_LAST_SOURCE_PATH, "none\n");
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
			const char *active_cwd;
			int active = (fat_active_drive_index >= 0) ? fat_active_drive_index : fat_mounted_drive_index;
			active_cwd = (active >= 0 && active != fat_mounted_drive_index) ? fat_drive_cwd[active] : fat_cwd;
			i = 0;
			while (active_cwd[i] != '\0' && i + 1 < out_size)
			{
				out[i] = active_cwd[i];
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

	/* Mirror PS1 into ps1_buf */
	if (string_equals(name, "PS1"))
	{
		j = 0;
		while (value[j] != '\0' && j + 1 < sizeof(ps1_buf))
		{
			ps1_buf[j] = value[j];
			j++;
		}
		ps1_buf[j] = '\0';
	}
}

static void cmd_setvar(const char *args)
{
	char name[16];
	const char *p;
	unsigned long i;

	p = read_token(args, name, sizeof(name));
	if (p == (void *)0 || name[0] == '\0')
	{
		if (script_var_count == 0)
		{
			terminal_write_line("set: no variables defined");
			return;
		}
		for (i = 0; i < (unsigned long)script_var_count; i++)
		{
			terminal_write(script_var_names[i]);
			terminal_write("=");
			terminal_write_line(script_var_values[i]);
		}
		return;
	}

	p = skip_spaces(p);
	if (*p == '\0')
	{
		terminal_write_line("Usage: set [<name> <value>]");
		return;
	}
	{
		char value[96];
		i = 0;
		while (p[i] != '\0' && i + 1 < sizeof(value))
		{
			value[i] = p[i];
			i++;
		}
		value[i] = '\0';
		script_set_var(name, value);
	}
}

static void cmd_unset(const char *args)
{
	char name[16];
	int i, j;
	read_token(args, name, sizeof(name));
	if (name[0] == '\0') { terminal_write_line("Usage: unset <variable>"); return; }
	for (i = 0; i < script_var_count; i++)
	{
		if (string_equals(script_var_names[i], name))
		{
			for (j = i; j + 1 < script_var_count; j++)
			{
				unsigned long k;
				for (k = 0; k < sizeof(script_var_names[0]); k++) script_var_names[j][k] = script_var_names[j + 1][k];
				for (k = 0; k < sizeof(script_var_values[0]); k++) script_var_values[j][k] = script_var_values[j + 1][k];
			}
			script_var_count--;
			if (string_equals(name, "PS1")) { ps1_buf[0] = '>'; ps1_buf[1] = ' '; ps1_buf[2] = '\0'; }
			return;
		}
	}
	terminal_write("unset: "); terminal_write(name); terminal_write_line(": not found");
	last_exit_code = 1;
}

static void cmd_run(const char *args);

static void cmd_source(const char *args)
{
	run_source_mode = 1;
	cmd_run(args);
	run_source_mode = 0;
}

static void cmd_read(const char *args)
{
	char var_name[16];
	char line[INPUT_BUFFER_SIZE];
	read_token(args, var_name, sizeof(var_name));
	if (var_name[0] == '\0') { terminal_write_line("Usage: read <variable>"); return; }
	terminal_read_line(line, sizeof(line));
	script_set_var(var_name, line);
}

static long eval_arith_factor(const char **pp)
{
	long val = 0;
	int neg = 0;
	const char *p = *pp;
	while (*p == ' ') p++;
	if (*p == '-') { neg = 1; p++; while (*p == ' ') p++; }
	else if (*p == '+') { p++; while (*p == ' ') p++; }
	if (*p == '(')
	{
		long eval_arith_expr_inner(const char **);
		p++;
		val = eval_arith_expr_inner(&p);
		while (*p == ' ') p++;
		if (*p == ')') p++;
	}
	else if (*p >= '0' && *p <= '9')
	{
		while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
	}
	else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')
	{
		char vname[16];
		unsigned long vi = 0;
		int si;
		while (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') && vi + 1 < sizeof(vname))
		{
			vname[vi++] = *p; p++;
		}
		vname[vi] = '\0';
		for (si = 0; si < script_var_count; si++)
		{
			if (string_equals(script_var_names[si], vname))
			{
				const char *sv = script_var_values[si];
				int sn = 0;
				if (*sv == '-') { sn = 1; sv++; }
				while (*sv >= '0' && *sv <= '9') { val = val * 10 + (*sv - '0'); sv++; }
				if (sn) val = -val;
				break;
			}
		}
	}
	*pp = p;
	return neg ? -val : val;
}

static long eval_arith_term(const char **pp)
{
	long val = eval_arith_factor(pp);
	const char *p = *pp;
	for (;;)
	{
		while (*p == ' ') p++;
		if (*p == '*') { p++; *pp = p; val *= eval_arith_factor(pp); p = *pp; }
		else if (*p == '/') { p++; *pp = p; { long d = eval_arith_factor(pp); p = *pp; val = (d != 0) ? val / d : 0; } }
		else if (*p == '%') { p++; *pp = p; { long d = eval_arith_factor(pp); p = *pp; val = (d != 0) ? val % d : 0; } }
		else break;
	}
	*pp = p;
	return val;
}

long eval_arith_expr_inner(const char **pp)
{
	long val = eval_arith_term(pp);
	const char *p = *pp;
	for (;;)
	{
		while (*p == ' ') p++;
		if (*p == '+') { p++; *pp = p; val += eval_arith_term(pp); p = *pp; }
		else if (*p == '-') { p++; *pp = p; val -= eval_arith_term(pp); p = *pp; }
		else break;
	}
	*pp = p;
	return val;
}

static int eval_arithmetic(const char *expr, char *out, unsigned long out_size)
{
	const char *p = expr;
	long val = eval_arith_expr_inner(&p);
	long v = val;
	unsigned long pos = 0;
	char tmp[24];
	int ti = 0;

	if (val < 0) { if (pos + 1 < out_size) out[pos++] = '-'; v = -v; }
	if (v == 0) { if (pos + 1 < out_size) out[pos++] = '0'; }
	else
	{
		while (v > 0) { tmp[ti++] = (char)('0' + (v % 10)); v /= 10; }
		while (ti > 0) { if (pos + 1 < out_size) out[pos++] = tmp[--ti]; }
	}
	out[pos] = '\0';
	return 0;
}

static int expand_command_substitutions(const char *in, char *out, unsigned long out_size)
{
	unsigned long i = 0;
	unsigned long o = 0;

	if (in == (void *)0 || out == (void *)0 || out_size == 0) return -1;

	while (in[i] != '\0')
	{
		/* $? — last exit code */
		if (in[i] == '$' && in[i + 1] == '?')
		{
			char tmp[16];
			int v = last_exit_code, ti = 0;
			if (v < 0) { if (o + 1 >= out_size) return -1; out[o++] = '-'; v = -v; }
			if (v == 0) { tmp[ti++] = '0'; }
			else { while (v > 0) { tmp[ti++] = (char)('0' + v % 10); v /= 10; } }
			while (ti > 0) { if (o + 1 >= out_size) return -1; out[o++] = tmp[--ti]; }
			i += 2;
			continue;
		}
		/* ${#var} — length of variable, ${var:-default}, ${var} */
		if (in[i] == '$' && in[i + 1] == '{')
		{
			unsigned long j = i + 2;
			int is_len = 0;
			if (in[j] == '#') { is_len = 1; j++; }
			/* read variable name */
			char vname[16];
			unsigned long vi = 0;
			while ((in[j] >= 'a' && in[j] <= 'z') || (in[j] >= 'A' && in[j] <= 'Z') ||
			       (in[j] >= '0' && in[j] <= '9') || in[j] == '_')
			{
				if (vi + 1 < sizeof(vname)) vname[vi++] = in[j];
				j++;
			}
			vname[vi] = '\0';
			/* look up value */
			const char *val = "";
			int found = 0;
			for (int si = 0; si < script_var_count; si++)
			{
				if (string_equals(script_var_names[si], vname))
				{ val = script_var_values[si]; found = 1; break; }
			}
			if (is_len)
			{
				/* ${#var} — emit length */
				if (in[j] != '}') return -1;
				j++;
				unsigned long len = 0;
				if (found) { const char *p = val; while (*p) { len++; p++; } }
				char tmp[16]; uint_to_dec(len, tmp, sizeof(tmp));
				unsigned long k = 0;
				while (tmp[k]) { if (o + 1 >= out_size) return -1; out[o++] = tmp[k++]; }
				i = j;
				continue;
			}
			/* check for :- (default value) */
			if (in[j] == ':' && in[j + 1] == '-')
			{
				j += 2;
				/* read default text until } */
				char def[64];
				unsigned long di = 0;
				while (in[j] != '\0' && in[j] != '}')
				{
					if (di + 1 < sizeof(def)) def[di++] = in[j];
					j++;
				}
				def[di] = '\0';
				if (in[j] != '}') return -1;
				j++;
				const char *use = (found && val[0] != '\0') ? val : def;
				while (*use) { if (o + 1 >= out_size) return -1; out[o++] = *use++; }
				i = j;
				continue;
			}
			/* plain ${var} */
			if (in[j] != '}') return -1;
			j++;
			while (*val) { if (o + 1 >= out_size) return -1; out[o++] = *val++; }
			i = j;
			continue;
		}
		/* $VAR — script variable expansion */
		if (in[i] == '$' && in[i + 1] != '(' && in[i + 1] != '\0' && in[i + 1] != ' ' && in[i + 1] != '"' && in[i + 1] != '\'')
		{
			char vname[16];
			unsigned long vi = 0, j;
			int si, found = 0;
			j = i + 1;
			while ((in[j] >= 'a' && in[j] <= 'z') || (in[j] >= 'A' && in[j] <= 'Z') ||
			       (in[j] >= '0' && in[j] <= '9') || in[j] == '_')
			{
				if (vi + 1 < sizeof(vname)) vname[vi++] = in[j];
				j++;
			}
			vname[vi] = '\0';
			if (vi > 0)
			{
				for (si = 0; si < script_var_count; si++)
				{
					if (string_equals(script_var_names[si], vname))
					{
						unsigned long k = 0;
						while (script_var_values[si][k] != '\0')
						{
							if (o + 1 >= out_size) return -1;
							out[o++] = script_var_values[si][k++];
						}
						found = 1;
						break;
					}
				}
				i = j;
				(void)found; /* unset vars expand to empty */
				continue;
			}
		}
		if (in[i] == '$' && in[i + 1] == '(' && in[i + 2] == '(')
		{
			char inner[128];
			char value[64];
			unsigned long j = i + 3;
			unsigned long k = 0;
			int depth = 1;

			while (in[j] != '\0' && depth > 0)
			{
				if (in[j] == '(' && in[j + 1] == '(') { depth++; }
				if (in[j] == ')' && in[j + 1] == ')') { depth--; if (depth == 0) break; }
				if (k + 1 >= sizeof(inner)) return -1;
				inner[k++] = in[j++];
			}
			if (depth != 0 || in[j] != ')' || in[j + 1] != ')') return -1;
			inner[k] = '\0';

			if (eval_arithmetic(inner, value, sizeof(value)) != 0) return -1;

			k = 0;
			while (value[k] != '\0')
			{
				if (o + 1 >= out_size) return -1;
				out[o++] = value[k++];
			}
			i = j + 2;
			continue;
		}
		else if (in[i] == '$' && in[i + 1] == '(')
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

		/* Tilde expansion: ~ at start or after space/= → / */
		if (in[i] == '~' && (in[i + 1] == '/' || in[i + 1] == '\0' || in[i + 1] == ' '))
		{
			if (i == 0 || in[i - 1] == ' ' || in[i - 1] == '=')
			{
				if (o + 1 >= out_size) return -1;
				out[o++] = '/';
				i++;
				continue;
			}
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

/* Reverse-search: find match starting from 'from' going backward.
   Returns history ring index or -1. */
static int rsearch_find(int from)
{
	int i;
	if (rsearch_len == 0 || history_count == 0) return -1;
	rsearch_term[rsearch_len] = '\0';
	for (i = from; i >= 0; i--)
	{
		int idx = (history_start + i) % HISTORY_SIZE;
		if (string_contains_ci(history[idx], rsearch_term))
			return i;
	}
	return -1;
}

/* Redraw reverse-search prompt line */
static void rsearch_redraw(void)
{
	unsigned long pos = prompt_vga_start - 2;
	unsigned long width = (unsigned long)screen_get_width();
	unsigned long row = pos / width;
	unsigned long start = row * width;
	unsigned long end = start + width;
	unsigned long p = start;
	const char *prefix = "(reverse-i-search)`";
	const char *suffix = "': ";
	const char *match = "";
	if (rsearch_match >= 0)
	{
		int idx = (history_start + rsearch_match) % HISTORY_SIZE;
		match = history[idx];
	}
	screen_set_color(terminal_prompt_color);
	while (*prefix && p < end) { screen_write_char_at((unsigned short)p, *prefix); p++; prefix++; }
	screen_set_color(terminal_text_color);
	{ int k = 0; while (k < rsearch_len && p < end) { screen_write_char_at((unsigned short)p, rsearch_term[k]); p++; k++; } }
	screen_set_color(terminal_prompt_color);
	while (*suffix && p < end) { screen_write_char_at((unsigned short)p, *suffix); p++; suffix++; }
	screen_set_color(terminal_text_color);
	while (*match && p < end) { screen_write_char_at((unsigned short)p, *match); p++; match++; }
	while (p < end) { screen_write_char_at((unsigned short)p, ' '); p++; }
	screen_set_hw_cursor((unsigned short)p);
}

/* Accept the current rsearch match into input_buffer */
static void rsearch_accept(void)
{
	rsearch_active = 0;
	if (rsearch_match >= 0)
	{
		int idx = (history_start + rsearch_match) % HISTORY_SIZE;
		unsigned long k = 0;
		while (history[idx][k]) { input_buffer[k] = history[idx][k]; k++; }
		input_buffer[k] = '\0';
		input_length = k;
		cursor_pos = k;
	}
	terminal_draw_prompt_prefix();
	terminal_redraw_input_line();
}

/* Cancel rsearch and restore original line */
static void rsearch_cancel(void)
{
	rsearch_active = 0;
	terminal_draw_prompt_prefix();
	terminal_redraw_input_line();
}

static void cmd_history(const char *args)
{
	int i;
	char num_buf[16];
	(void)args;
	for (i = 0; i < history_count; i++)
	{
		int idx = (history_start + i) % HISTORY_SIZE;
		uint_to_dec((unsigned long)(i + 1), num_buf, sizeof(num_buf));
		terminal_write("  ");
		terminal_write(num_buf);
		terminal_write("  ");
		terminal_write_line(history[idx]);
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
	"help", "man", "alias", "unalias", "version", "echo", "glyph", "charmap", "color", "serial", "display", "themes", "theme", "ethemes", "etheme", "clear", "pause", "wait", "reboot",
	"panic",
	"quit", "exit", "shutdown",
	"set",
	"pwd", "ls", "dirname", "dir", "tree", "cd", "mkdir", "touch", "write", "cat", "type", "rm", "del", "cp", "copy", "mv", "move", "ren", "edit", "hexedit", "run", "basic", "cls",
	"grep", "find", "wc", "head", "tail", "xxd", "sort", "uniq", "tee", "tr", "seq", "diff", "cmp", "calc",
	"hexdump", "memmap", "memstat", "pagetest", "pagefault", "gpfault", "udfault", "doublefault", "exceptstat", "dumpstack", "selftest", "elfinfo", "elfsegs", "elfsects", "elfsym", "elfaddr", "elfcheck", "exec", "exectrace", "execstress", "elfselftest", "tasks", "tasktest", "taskspin", "shellspawn", "shellwatch", "taskprotect", "tasklog", "taskkill", "taskstop", "taskcont", "ticks", "motd", "autorun", "ataid", "readsec", "writesec", "drives", "fatmount", "mtn", "mnt", "mount", "fatwhere", "fatstress", "fatperf", "ramfs", "ramfs2fat", "fatunmount", "umtn", "umnt", "umount", "fatls", "fatcat", "fattouch", "fatwrite", "fatattr", "fatrm",
	"netinfo", "netreinit", "ping", "arp", "udpsend", "dhcp", "nslookup", "wget",
	"date", "uptime", "jobs", "sleep", "env", "whoami", "hostname", "history", "unset", "source", "read",
	"df", "stat", "cut", "rev", "printf",
	"true", "false", "test", "which", "type", "basename", "yes", "nl", "factor", "du", "xargs",
	"less", "more", "tac", "expr",
	"watch", "paste", "column", "strings", "rmdir", "time",
	"gui",
	(void *)0
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
	unsigned long file_size = 0;
	int is_dir = 0;
	const char *source_name = fat_mode_active() ? "fat" : "ramfs";

	if (path == (void *)0 || path[0] == '\0' || buf == (void *)0 || out_size == (void *)0) return -1;
	if (fat_mode_active())
	{
		if (fat_resolve_path(path, resolved, sizeof(resolved)) != 0)
		{
			terminal_write(op);
			terminal_write_line(": invalid FAT path");
			return -1;
		}
		if (fat32_stat_path(resolved, &is_dir, &file_size) != 0 || is_dir)
		{
			terminal_write(op);
			terminal_write(": read failed (");
			terminal_write(source_name);
			terminal_write_line(")");
			return -1;
		}
		if (file_size > capacity)
		{
			terminal_write(op);
			terminal_write_line(": file too large for buffer");
			return -1;
		}
		if (fat32_read_file_path(resolved, buf, capacity, out_size) != 0)
		{
			terminal_write(op);
			terminal_write(": read failed (");
			terminal_write(source_name);
			terminal_write_line(")");
			return -1;
		}
	}
	else
	{
		if (fs_stat(path, &is_dir, &file_size) != 0 || is_dir)
		{
			terminal_write(op);
			terminal_write(": read failed (");
			terminal_write(source_name);
			terminal_write_line(")");
			return -1;
		}
		if (file_size > capacity)
		{
			terminal_write(op);
			terminal_write_line(": file too large for buffer");
			return -1;
		}
		if (fs_read_file(path, buf, capacity, out_size) != 0)
		{
			terminal_write(op);
			terminal_write(": read failed (");
			terminal_write(source_name);
			terminal_write_line(")");
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
	unsigned long entry;
	unsigned long load_starts[32];
	unsigned long load_ends[32];
	unsigned long load_count;
	int entry_in_load;
	int overlap_detected;
	int align_mismatch;
	int range_overflow;
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
	unsigned long seg_end = 0;
	unsigned long page_start = 0;
	unsigned long page_end = 0;
	unsigned long i;
	const unsigned long page_size = 4096UL;

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
	terminal_write(n);
	terminal_write(" align=");
	terminal_write_hex64(ph->align);

	if (ph->type == 1u && ph->memsz != 0)
	{
		seg_end = ph->vaddr + ph->memsz;
		if (seg_end < ph->vaddr)
		{
			print_ctx->range_overflow = 1;
		}
		else
		{
			page_start = ph->vaddr & ~(page_size - 1UL);
			page_end = (seg_end + page_size - 1UL) & ~(page_size - 1UL);

			if (print_ctx->entry >= ph->vaddr && print_ctx->entry < seg_end)
			{
				print_ctx->entry_in_load = 1;
				terminal_write(" <entry>");
			}

			if (ph->align > 1UL)
			{
				if ((ph->align & (ph->align - 1UL)) != 0 || ((ph->vaddr & (ph->align - 1UL)) != (ph->offset & (ph->align - 1UL))))
				{
					print_ctx->align_mismatch = 1;
					terminal_write(" <align?>");
				}
			}

			for (i = 0; i < print_ctx->load_count; i++)
			{
				if (page_start < print_ctx->load_ends[i] && print_ctx->load_starts[i] < page_end)
				{
					print_ctx->overlap_detected = 1;
					terminal_write(" <overlap?>");
					break;
				}
			}

			if (print_ctx->load_count < (sizeof(print_ctx->load_starts) / sizeof(print_ctx->load_starts[0])))
			{
				print_ctx->load_starts[print_ctx->load_count] = page_start;
				print_ctx->load_ends[print_ctx->load_count] = page_end;
				print_ctx->load_count++;
			}
		}
	}

	terminal_putc('\n');
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
	int has_space = 0;

	/* Check if input has a space → path completion mode */
	for (j = 0; j < input_length; j++)
	{
		if (input_buffer[j] == ' ') { has_space = 1; break; }
	}

	if (!has_space)
	{
		/* Command name completion */
		for (i = 0; cmd_list[i]; i++)
			if (string_starts_with(cmd_list[i], input_buffer)) { match_count++; last = cmd_list[i]; }
		for (alias_index = 0; alias_index < command_alias_count; alias_index++)
			if (string_starts_with(command_alias_names[alias_index], input_buffer)) { match_count++; last = command_alias_names[alias_index]; }

		if (match_count == 1)
		{
			clear_input_line();
			for (j = 0; last[j]; j++) { input_buffer[j] = last[j]; screen_write_char_at((unsigned short)(prompt_vga_start + j), last[j]); }
			input_buffer[j] = ' '; screen_write_char_at((unsigned short)(prompt_vga_start + j), ' '); j++;
			input_length = j; cursor_pos = j; input_buffer[j] = '\0';
			sync_screen_pos();
			screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
		}
		else if (match_count > 1)
		{
			/* Find longest common prefix */
			char prefix[INPUT_BUFFER_SIZE];
			unsigned long prefix_len = 0;
			int first = 1;
			for (i = 0; cmd_list[i]; i++)
			{
				if (string_starts_with(cmd_list[i], input_buffer))
				{
					if (first) { for (prefix_len = 0; cmd_list[i][prefix_len]; prefix_len++) prefix[prefix_len] = cmd_list[i][prefix_len]; prefix[prefix_len] = '\0'; first = 0; }
					else { unsigned long k; for (k = 0; k < prefix_len && cmd_list[i][k] && cmd_list[i][k] == prefix[k]; k++); prefix_len = k; prefix[k] = '\0'; }
				}
			}
			for (alias_index = 0; alias_index < command_alias_count; alias_index++)
			{
				if (string_starts_with(command_alias_names[alias_index], input_buffer))
				{
					if (first) { for (prefix_len = 0; command_alias_names[alias_index][prefix_len]; prefix_len++) prefix[prefix_len] = command_alias_names[alias_index][prefix_len]; prefix[prefix_len] = '\0'; first = 0; }
					else { unsigned long k; for (k = 0; k < prefix_len && command_alias_names[alias_index][k] && command_alias_names[alias_index][k] == prefix[k]; k++); prefix_len = k; prefix[k] = '\0'; }
				}
			}
			if (prefix_len > input_length)
			{
				clear_input_line();
				for (j = 0; j < prefix_len; j++) { input_buffer[j] = prefix[j]; screen_write_char_at((unsigned short)(prompt_vga_start + j), prefix[j]); }
				input_length = j; cursor_pos = j; input_buffer[j] = '\0';
				sync_screen_pos();
				screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
			}
			else
			{
				sync_screen_pos(); terminal_putc('\n');
				for (i = 0; cmd_list[i]; i++)
					if (string_starts_with(cmd_list[i], input_buffer)) { terminal_write(cmd_list[i]); terminal_putc(' '); }
				for (alias_index = 0; alias_index < command_alias_count; alias_index++)
					if (string_starts_with(command_alias_names[alias_index], input_buffer)) { terminal_write(command_alias_names[alias_index]); terminal_putc(' '); }
				terminal_putc('\n');
				terminal_prompt();
				for (j = 0; j < input_length; j++) screen_write_char_at((unsigned short)(prompt_vga_start + j), input_buffer[j]);
				cursor_pos = input_length; sync_screen_pos();
				screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
			}
		}
	}
	else
	{
		/* Path/filename completion */
		char partial[128];
		char dir_path[128];
		char prefix_part[128];
		unsigned long partial_start = 0, pi = 0, di = 0;
		char names[FS_MAX_LIST][FS_NAME_MAX + 2];
		int types[FS_MAX_LIST];
		int count = 0;

		/* Extract the last word (the path being typed) */
		{
			unsigned long last_space = 0;
			for (j = 0; j < input_length; j++)
				if (input_buffer[j] == ' ') last_space = j + 1;
			partial_start = last_space;
		}
		for (j = partial_start; j < input_length && pi + 1 < sizeof(partial); j++)
			partial[pi++] = input_buffer[j];
		partial[pi] = '\0';

		/* Split partial into dir_path and prefix_part */
		{
			int last_slash = -1;
			for (j = 0; j < pi; j++)
				if (partial[j] == '/') last_slash = (int)j;
			if (last_slash >= 0)
			{
				unsigned long k;
				for (k = 0; k <= (unsigned long)last_slash && k + 1 < sizeof(dir_path); k++)
					dir_path[k] = partial[k];
				dir_path[k] = '\0';
				di = 0;
				for (k = (unsigned long)last_slash + 1; k < pi && di + 1 < sizeof(prefix_part); k++)
					prefix_part[di++] = partial[k];
				prefix_part[di] = '\0';
			}
			else
			{
				dir_path[0] = '.';
				dir_path[1] = '\0';
				for (j = 0; j < pi && j + 1 < sizeof(prefix_part); j++)
					prefix_part[j] = partial[j];
				prefix_part[j] = '\0';
				di = j;
			}
		}

		if (fat_mode_active())
		{
			char fat_names[64][40];
			char fat_full[128];
			int fat_count = 0;
			if (fat_resolve_path(dir_path, fat_full, sizeof(fat_full)) == 0 &&
			    fat32_ls_path(fat_full, fat_names, 64, &fat_count) == 0)
			{
				count = 0;
				for (i = 0; i < fat_count && count < FS_MAX_LIST; i++)
				{
					unsigned long k;
					for (k = 0; fat_names[i][k] && k + 1 < FS_NAME_MAX + 2; k++)
						names[count][k] = fat_names[i][k];
					names[count][k] = '\0';
					/* Determine type via stat */
					{
						char child_path[128];
						unsigned long ci2 = 0, k2;
						int cis_dir = 0;
						unsigned long csz = 0;
						for (k2 = 0; fat_full[k2] && ci2 + 1 < sizeof(child_path); k2++) child_path[ci2++] = fat_full[k2];
						if (ci2 > 0 && child_path[ci2 - 1] != '/') child_path[ci2++] = '/';
						for (k2 = 0; names[count][k2] && ci2 + 1 < sizeof(child_path); k2++) child_path[ci2++] = names[count][k2];
						child_path[ci2] = '\0';
						fat32_stat_path(child_path, &cis_dir, &csz);
						types[count] = cis_dir;
					}
					count++;
				}
			}
		}
		else
		{
			if (fs_ls(dir_path, names, types, FS_MAX_LIST, &count) != 0)
				return;
		}

		match_count = 0;
		last = (void *)0;
		for (i = 0; i < count; i++)
		{
			if (string_starts_with(names[i], prefix_part))
			{
				match_count++;
				last = names[i];
			}
		}

		if (match_count == 1)
		{
			/* Build completed path */
			char completed[INPUT_BUFFER_SIZE];
			unsigned long ci = 0;
			int is_dir = 0;

			for (j = 0; j < partial_start && ci + 1 < sizeof(completed); j++)
				completed[ci++] = input_buffer[j];
			/* Add dir_path prefix if it wasn't "." */
			if (!(dir_path[0] == '.' && dir_path[1] == '\0'))
			{
				for (j = 0; dir_path[j] && ci + 1 < sizeof(completed); j++)
					completed[ci++] = dir_path[j];
				if (ci > 0 && completed[ci - 1] != '/') completed[ci++] = '/';
			}
			for (j = 0; last[j] && ci + 1 < sizeof(completed); j++)
				completed[ci++] = last[j];

			/* Check if it's a directory - if so, append / */
			for (i = 0; i < count; i++)
			{
				if (string_starts_with(names[i], prefix_part) && names[i] == last)
				{
					is_dir = types[i];
					break;
				}
			}
			if (is_dir) { if (ci + 1 < sizeof(completed)) completed[ci++] = '/'; }
			else { if (ci + 1 < sizeof(completed)) completed[ci++] = ' '; }

			completed[ci] = '\0';
			clear_input_line();
			for (j = 0; j < ci; j++) { input_buffer[j] = completed[j]; screen_write_char_at((unsigned short)(prompt_vga_start + j), completed[j]); }
			input_length = ci; cursor_pos = ci; input_buffer[ci] = '\0';
			sync_screen_pos();
			screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
		}
		else if (match_count > 1)
		{
			/* Find longest common prefix among matches */
			char common[128];
			unsigned long common_len = 0;
			int first = 1;
			for (i = 0; i < count; i++)
			{
				if (string_starts_with(names[i], prefix_part))
				{
					if (first)
					{
						for (common_len = 0; names[i][common_len]; common_len++) common[common_len] = names[i][common_len];
						common[common_len] = '\0';
						first = 0;
					}
					else
					{
						unsigned long k;
						for (k = 0; k < common_len && names[i][k] && names[i][k] == common[k]; k++);
						common_len = k;
						common[k] = '\0';
					}
				}
			}
			if (common_len > di)
			{
				/* Extend to common prefix */
				char completed[INPUT_BUFFER_SIZE];
				unsigned long ci = 0;
				for (j = 0; j < partial_start && ci + 1 < sizeof(completed); j++)
					completed[ci++] = input_buffer[j];
				if (!(dir_path[0] == '.' && dir_path[1] == '\0'))
				{
					for (j = 0; dir_path[j] && ci + 1 < sizeof(completed); j++)
						completed[ci++] = dir_path[j];
					if (ci > 0 && completed[ci - 1] != '/') completed[ci++] = '/';
				}
				for (j = 0; j < common_len && ci + 1 < sizeof(completed); j++)
					completed[ci++] = common[j];
				completed[ci] = '\0';
				clear_input_line();
				for (j = 0; j < ci; j++) { input_buffer[j] = completed[j]; screen_write_char_at((unsigned short)(prompt_vga_start + j), completed[j]); }
				input_length = ci; cursor_pos = ci; input_buffer[ci] = '\0';
				sync_screen_pos();
				screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
			}
			else
			{
				sync_screen_pos(); terminal_putc('\n');
				for (i = 0; i < count; i++)
				{
					if (string_starts_with(names[i], prefix_part))
					{
						terminal_write(names[i]);
						if (types[i]) terminal_putc('/');
						terminal_putc(' ');
					}
				}
				terminal_putc('\n');
				terminal_prompt();
				for (j = 0; j < input_length; j++) screen_write_char_at((unsigned short)(prompt_vga_start + j), input_buffer[j]);
				cursor_pos = input_length; sync_screen_pos();
				screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
			}
		}
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

static int terminal_wait_for_keypress(void)
{
	int saw_extended = 0;
	for (;;)
	{
		while (scancode_queue_tail != scancode_queue_head)
		{
			unsigned char sc = scancode_queue[scancode_queue_tail];
			scancode_queue_tail = (scancode_queue_tail + 1) % SCANCODE_QUEUE_SIZE;

			if (sc == 0xE0)
			{
				saw_extended = 1;
				continue;
			}
			if ((sc & 0x80u) != 0)
			{
				saw_extended = 0;
				continue;
			}
			if (!saw_extended && (sc == 0x2A || sc == 0x36 || sc == 0x1D || sc == 0x38)) continue;
			return 0;
		}
		task_yield();
	}
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

static void cmd_pause(const char *args)
{
	if (skip_spaces(args)[0] != '\0')
	{
		terminal_write_line("Usage: pause");
		return;
	}
	terminal_write("Press any key to continue . . .");
	(void)terminal_wait_for_keypress();
	terminal_putc('\n');
}

static void cmd_wait(const char *args)
{
	char tok[16];
	unsigned int seconds;
	unsigned long deadline;
	const char *rest = read_token(args, tok, sizeof(tok));

	if (rest == (void *)0 || tok[0] == '\0' || parse_dec_u32(tok, &seconds) != 0 || skip_spaces(rest)[0] != '\0')
	{
		terminal_write_line("Usage: wait <seconds>");
		return;
	}

	deadline = timer_ticks() + (unsigned long)seconds * 100UL;
	while (timer_ticks() < deadline)
	{
		terminal_poll_control_hotkeys();
		if (terminal_take_cancel_request())
		{
			terminal_write_line("wait: canceled");
			return;
		}
		task_yield();
	}
}

static void trigger_forced_panic(void)
{
	/* Intentional page fault for panic-path testing. */
	*((volatile unsigned long *)0) = 0xDEADBEEFUL;
	for (;;) arch_halt();
}

static int confirm_dangerous_action(const char *args, const char *rerun_hint)
{
	char tok[8];
	const char *p = read_token(args, tok, sizeof(tok));

	if (p != (void *)0 && string_equals(tok, "yes") && skip_spaces(p)[0] == '\0') return 1;

	terminal_write("Are you sure? Re-run: ");
	terminal_write(rerun_hint);
	terminal_write_line(" yes");
	return 0;
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
	char rerun_hint[48];
	unsigned long i = 0;
	const char *p = read_token(args, mode, sizeof(mode));
	unsigned long fault_addr;

	if (p == (void *)0 || mode[0] == '\0')
	{
		terminal_write_line("Usage: pagefault <read|write|exec> [yes]");
		return;
	}

	if (!(string_equals(mode, "read") || string_equals(mode, "write") || string_equals(mode, "exec")))
	{
		terminal_write_line("Usage: pagefault <read|write|exec> [yes]");
		return;
	}

	{
		const char *prefix = "pagefault ";
		while (prefix[i] != '\0' && i + 1 < sizeof(rerun_hint)) { rerun_hint[i] = prefix[i]; i++; }
		{
			unsigned long j = 0;
			while (mode[j] != '\0' && i + 1 < sizeof(rerun_hint)) { rerun_hint[i++] = mode[j++]; }
		}
		rerun_hint[i] = '\0';
	}

	if (!confirm_dangerous_action(p, rerun_hint)) return;

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
		terminal_write_line("Usage: pagefault <read|write|exec> [yes]");
		return;
	}

	for (;;) arch_halt();
}

static void cmd_gpfault(const char *args)
{
	if (!confirm_dangerous_action(args, "gpfault"))
	{
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
	if (!confirm_dangerous_action(args, "udfault"))
	{
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

	if (!confirm_dangerous_action(args, "doublefault"))
	{
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
		cmd_pagefault("read yes");
		return;
	}
	if (string_equals(step, "pf-write") || string_equals(step, "2"))
	{
		terminal_write_line("[selftest] step 2/5: #PF write expected (vec=0x0E, P=0 W=1 I=0)");
		cmd_pagefault("write yes");
		return;
	}
	if (string_equals(step, "pf-exec") || string_equals(step, "3"))
	{
		terminal_write_line("[selftest] step 3/5: #PF exec expected (vec=0x0E, P=0 W=0 I=1)");
		cmd_pagefault("exec yes");
		return;
	}
	if (string_equals(step, "ud") || string_equals(step, "4"))
	{
		terminal_write_line("[selftest] step 4/5: #UD expected (vec=0x06)");
		cmd_udfault("yes");
		return;
	}
	if (string_equals(step, "gp") || string_equals(step, "5"))
	{
		terminal_write_line("[selftest] step 5/5: #GP expected (vec=0x0D)");
		cmd_gpfault("yes");
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

static const char *elf_load_error_name(int rc)
{
	switch (rc)
	{
		case ELF_ERR_MAGIC: return "bad ELF magic";
		case ELF_ERR_CLASS: return "not ELF64";
		case ELF_ERR_TYPE: return "not executable or shared";
		case ELF_ERR_ARCH: return "not x86-64";
		case ELF_ERR_PHDR: return "bad program headers";
		case ELF_ERR_MAP: return "page mapping failed";
		case ELF_ERR_RANGE: return "segment exceeds file";
		case ELF_ERR_OVERLAP: return "overlapping PT_LOAD page windows";
		case ELF_ERR_ENTRY: return "entry point not in PT_LOAD segment";
		case ELF_ERR_NULL: return "null input";
		case ELF_ERR_INTERP: return "dynamic linking not supported";
		default: return "unknown error";
	}
}

static void cmd_exec(const char *args)
{
	char path[128];
	char exec_args[7][64];
	unsigned long exec_arg_count = 0;
	unsigned long size = 0;
	elf_exec_t prog;
	int rc;
	const char *p = read_token(args, path, sizeof(path));

	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: exec <path> [args...]");
		return;
	}
	for (;;)
	{
		char tok[64];
		p = read_token(p, tok, sizeof(tok));
		if (p == (void *)0)
		{
			terminal_write_line("exec: argument token too long");
			return;
		}
		if (tok[0] == '\0') break;
		if (exec_arg_count >= 7)
		{
			terminal_write_line("exec: too many args (max 7)");
			return;
		}
		{
			unsigned long i = 0;
			while (tok[i] != '\0' && i + 1 < sizeof(exec_args[exec_arg_count]))
			{
				exec_args[exec_arg_count][i] = tok[i];
				i++;
			}
			exec_args[exec_arg_count][i] = '\0';
		}
		exec_arg_count++;
	}

	if (read_shell_file_bytes("exec", path, elf_shell_io_buf, ELF_SHELL_IO_MAX, &size) != 0) return;

	terminal_write("[exec] loading ");
	terminal_write(path);
	terminal_write_line(" ...");

	rc = elf_load(elf_shell_io_buf, size, &prog);
	if (rc != 0)
	{
		char n[16];
		terminal_write("[exec] load error: ");
		terminal_write(elf_load_error_name(rc));
		terminal_write(" (rc=");
		if (rc < 0)
		{
			terminal_putc('-');
			uint_to_dec((unsigned long)(-rc), n, sizeof(n));
		}
		else
		{
			uint_to_dec((unsigned long)rc, n, sizeof(n));
		}
		terminal_write(n);
		terminal_write_line(")");
		return;
	}

	terminal_write("[exec] entry: ");
	terminal_write_hex64(prog.entry);
	terminal_putc('\n');
	terminal_write_line("[exec] calling...");

	{
#define USER_STACK_PAGES 8
		const unsigned long user_stack_span = (unsigned long)(USER_STACK_PAGES) * 4096UL;
		const unsigned long user_stack_base = 0x0000002000000000UL;
		const unsigned long user_stack_top = user_stack_base + user_stack_span;
		const unsigned long user_heap_gap = 0x10000UL;
		const unsigned long user_heap_base = 0x0000001800000000UL;
		const unsigned long user_heap_limit = user_stack_base - user_heap_gap;
		unsigned long user_rsp;
		unsigned long argc = 1 + exec_arg_count;
		unsigned char *stack_alias_base;
		unsigned long page;
		unsigned long phys_pages[USER_STACK_PAGES];
		int map_ok = 1;
		int i;
		char trace_n[16];
		unsigned long flags = PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_USER | PAGE_FLAG_NO_EXECUTE;

		/* Ensure fixed stack window is currently unmapped. */
		for (i = 0; i < USER_STACK_PAGES; i++)
		{
			unsigned long probe = user_stack_base + (unsigned long)i * 4096UL;
			unsigned long existing = paging_get_phys(probe);
			if (existing != 0)
			{
				if (exec_trace_enabled)
				{
					terminal_write("[exec][trace] stack collision va=");
					terminal_write_hex64(probe);
					terminal_write(" pa=");
					terminal_write_hex64(existing);
					terminal_putc('\n');
				}
				terminal_write_line("[exec] fixed user stack window not free");
				elf_unload(elf_shell_io_buf, size);
				return;
			}
		}

		user_rsp = user_stack_top;
		if (exec_trace_enabled)
		{
			terminal_write("[exec] user stack ");
			terminal_write_hex64(user_stack_base);
			terminal_write("-");
			terminal_write_hex64(user_stack_top);
			terminal_putc('\n');
			terminal_write("[exec][trace] stack range ");
			terminal_write_hex64(user_stack_base);
			terminal_write("-");
			terminal_write_hex64(user_stack_top);
			terminal_write(" pages=");
			uint_to_dec(USER_STACK_PAGES, trace_n, sizeof(trace_n));
			terminal_write(trace_n);
			terminal_write(" argc=");
			uint_to_dec(argc, trace_n, sizeof(trace_n));
			terminal_write(trace_n);
			terminal_putc('\n');
		}

		/* Map user stack pages */
		stack_alias_base = (unsigned char *)virt_alloc_pages(USER_STACK_PAGES);
		for (i = 0; i < USER_STACK_PAGES; i++) phys_pages[i] = 0;
		if (stack_alias_base == (void *)0)
		{
			terminal_write_line("[exec] failed to allocate stack backing");
			elf_unload(elf_shell_io_buf, size);
			return;
		}
		for (i = 0; i < USER_STACK_PAGES && map_ok; i++)
		{
			int map_rc;
			page = user_stack_base + (unsigned long)i * 4096UL;
			phys_pages[i] = paging_get_phys((unsigned long)stack_alias_base + (unsigned long)i * 4096UL);
			if (phys_pages[i] == 0)
			{
				if (exec_trace_enabled)
				{
					terminal_write_line("[exec][trace] phys_alloc_page failed for user stack");
				}
				map_ok = 0;
				break;
			}
			map_rc = paging_map_page(page, phys_pages[i], flags);
			if (map_rc != 0)
			{
				if (exec_trace_enabled)
				{
					terminal_write_line("[exec][trace] paging_map_page failed for user stack");
				}
				phys_pages[i] = 0;
				map_ok = 0;
			}
		}

		if (exec_trace_enabled && map_ok)
		{
			terminal_write_line("[exec][trace] user stack pages mapped");
		}

		if (!map_ok)
		{
			/* Unwind any pages already mapped */
			for (i = 0; i < USER_STACK_PAGES; i++)
			{
				if (phys_pages[i] != 0)
				{
					paging_unmap_page(user_stack_base + (unsigned long)i * 4096UL);
				}
			}
			virt_free_pages(stack_alias_base, USER_STACK_PAGES);
			terminal_write_line("[exec] failed to map user stack");
			elf_unload(elf_shell_io_buf, size);
			return;
		}

		if (user_heap_limit <= user_heap_base ||
			task_user_heap_config(user_heap_base, user_heap_limit) != 0)
		{
			terminal_write_line("[exec] failed to configure user heap");
			for (i = 0; i < USER_STACK_PAGES; i++)
			{
				page = user_stack_base + (unsigned long)i * 4096UL;
				paging_unmap_page(page);
			}
			virt_free_pages(stack_alias_base, USER_STACK_PAGES);
			elf_unload(elf_shell_io_buf, size);
			return;
		}

		if (exec_trace_enabled)
		{
			terminal_write("[exec][trace] heap range ");
			terminal_write_hex64(user_heap_base);
			terminal_write("-");
			terminal_write_hex64(user_heap_limit);
			terminal_putc('\n');
		}

		unsigned long argv1_ptr = 0UL;

		/* Build SysV AMD64 startup stack frame.
		 * Layout from RSP upward at process entry:
		 *   argc, argv[0..argc-1], NULL, envp[0]=NULL,
		 *   auxv (AT_PAGESZ=6, AT_ENTRY=9, AT_NULL=0), string data
		 */
		{
			unsigned long j;
			unsigned long str_cursor = user_stack_top;
			unsigned long argv_user_ptrs[8]; /* max 1 + 7 */
			/* frame: argc(1) + argv ptrs(argc) + argv_null(1) +
			 *        envp_null(1) + 3 auxv pairs(6) = argc + 9 qwords */
			unsigned long frame_qwords = argc + 9UL;

			/* Pack argv strings from the top of the stack downward */
			for (j = argc; j-- > 0; )
			{
				const char *s = (j == 0) ? path : exec_args[j - 1];
				unsigned long slen = string_length(s) + 1UL;
				unsigned long k;
				if (str_cursor < user_stack_base + slen)
				{
					terminal_write_line("[exec] argv strings overflow stack");
					syscall_mmap_cleanup();
					task_user_heap_reset();
					for (i = 0; i < USER_STACK_PAGES; i++)
						paging_unmap_page(user_stack_base + (unsigned long)i * 4096UL);
					virt_free_pages(stack_alias_base, USER_STACK_PAGES);
					elf_unload(elf_shell_io_buf, size);
					return;
				}
				str_cursor -= slen;
				argv_user_ptrs[j] = str_cursor;
				{
					unsigned long off = str_cursor - user_stack_base;
					for (k = 0; k < slen; k++)
						((unsigned char *)stack_alias_base)[off + k] = (unsigned char)s[k];
				}
			}

			/* 8-byte align, then adjust so RSP at argc is 16-byte aligned */
			str_cursor &= ~7UL;
			if ((str_cursor & 15UL) != (frame_qwords * 8UL & 15UL))
				str_cursor -= 8UL;
			user_rsp = str_cursor;

			if (user_rsp < user_stack_base + frame_qwords * 8UL)
			{
				terminal_write_line("[exec] insufficient stack for startup frame");
				syscall_mmap_cleanup();
				task_user_heap_reset();
				for (i = 0; i < USER_STACK_PAGES; i++)
					paging_unmap_page(user_stack_base + (unsigned long)i * 4096UL);
				virt_free_pages(stack_alias_base, USER_STACK_PAGES);
				elf_unload(elf_shell_io_buf, size);
				return;
			}

			/* Push startup frame top-down: auxv, envp null, argv ptrs, argc */
			/* AT_NULL */
			user_rsp -= 8UL; *((volatile unsigned long *)(stack_alias_base + (user_rsp - user_stack_base))) = 0UL;
			user_rsp -= 8UL; *((volatile unsigned long *)(stack_alias_base + (user_rsp - user_stack_base))) = 0UL;
			/* AT_ENTRY */
			user_rsp -= 8UL; *((volatile unsigned long *)(stack_alias_base + (user_rsp - user_stack_base))) = prog.entry;
			user_rsp -= 8UL; *((volatile unsigned long *)(stack_alias_base + (user_rsp - user_stack_base))) = 9UL;
			/* AT_PAGESZ */
			user_rsp -= 8UL; *((volatile unsigned long *)(stack_alias_base + (user_rsp - user_stack_base))) = 4096UL;
			user_rsp -= 8UL; *((volatile unsigned long *)(stack_alias_base + (user_rsp - user_stack_base))) = 6UL;
			/* envp[0] = NULL */
			user_rsp -= 8UL; *((volatile unsigned long *)(stack_alias_base + (user_rsp - user_stack_base))) = 0UL;
			/* argv[argc] = NULL */
			user_rsp -= 8UL; *((volatile unsigned long *)(stack_alias_base + (user_rsp - user_stack_base))) = 0UL;
			/* argv[argc-1] .. argv[0] */
			for (j = argc; j-- > 0; )
			{
				user_rsp -= 8UL;
				*((volatile unsigned long *)(stack_alias_base + (user_rsp - user_stack_base))) = argv_user_ptrs[j];
			}
			/* argc */
			user_rsp -= 8UL; *((volatile unsigned long *)(stack_alias_base + (user_rsp - user_stack_base))) = argc;
			if (argc >= 2) argv1_ptr = argv_user_ptrs[1];
		}

		task_exec_user(prog.entry, user_rsp, argc, argv1_ptr);

		/* Clean up user-mode anonymous mappings and open file descriptors */
		syscall_mmap_cleanup();
		syscall_fd_cleanup();

		/* Unmap user stack pages */
		for (i = 0; i < USER_STACK_PAGES; i++)
		{
			page = user_stack_base + (unsigned long)i * 4096UL;
			paging_unmap_page(page);
		}
		virt_free_pages(stack_alias_base, USER_STACK_PAGES);
	}

	elf_unload(elf_shell_io_buf, size);
	terminal_write_line("[exec] done");
}

static void cmd_exectrace(const char *args)
{
	char tok[8];
	const char *rest = read_token(args, tok, sizeof(tok));

	if (rest == (void *)0 || tok[0] == '\0' || string_equals(tok, "show"))
	{
		terminal_write("exectrace: ");
		terminal_write_line(exec_trace_enabled ? "on" : "off");
		return;
	}

	if (skip_spaces(rest)[0] != '\0')
	{
		terminal_write_line("Usage: exectrace [on|off|show]");
		return;
	}

	if (string_equals(tok, "on"))
	{
		exec_trace_enabled = 1;
		terminal_write_line("exectrace: on");
		return;
	}
	if (string_equals(tok, "off"))
	{
		exec_trace_enabled = 0;
		terminal_write_line("exectrace: off");
		return;
	}

	terminal_write_line("Usage: exectrace [on|off|show]");
}

static void cmd_elfcheck(const char *args)
{
	char path[128];
	unsigned long size = 0;
	elf_exec_t prog;
	int rc;
	const char *p = read_token(args, path, sizeof(path));

	if (p == (void *)0 || path[0] == '\0' || skip_spaces(p)[0] != '\0')
	{
		terminal_write_line("Usage: elfcheck <path>");
		return;
	}

	if (read_shell_file_bytes("elfcheck", path, elf_shell_io_buf, ELF_SHELL_IO_MAX, &size) != 0) return;

	rc = elf_load(elf_shell_io_buf, size, &prog);
	if (rc != 0)
	{
		char n[16];
		terminal_write("elfcheck: FAIL ");
		terminal_write(path);
		terminal_write(" : ");
		terminal_write(elf_load_error_name(rc));
		terminal_write(" (rc=");
		if (rc < 0)
		{
			terminal_putc('-');
			uint_to_dec((unsigned long)(-rc), n, sizeof(n));
		}
		else
		{
			uint_to_dec((unsigned long)rc, n, sizeof(n));
		}
		terminal_write(n);
		terminal_write_line(")");
		return;
	}

	terminal_write("elfcheck: PASS ");
	terminal_write(path);
	terminal_write(" entry=");
	terminal_write_hex64(prog.entry);
	terminal_putc('\n');
	elf_unload(elf_shell_io_buf, size);
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

static void cmd_preempt(const char *args)
{
	char tok[8];
	read_token(args, tok, sizeof(tok));
	if (string_equals(tok, "on"))
	{
		task_set_preemption(1);
		terminal_write_line("preemption enabled");
	}
	else if (string_equals(tok, "off"))
	{
		task_set_preemption(0);
		terminal_write_line("preemption disabled (cooperative only)");
	}
	else
	{
		terminal_write("preemption: ");
		terminal_write_line(task_get_preemption() ? "on" : "off");
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

/* ---- Background job support ---- */
static void bg_task_entry(void *arg)
{
	struct bg_job *job = (struct bg_job *)arg;
	unsigned long i = 0;

	/* Copy our saved command into input_buffer for dispatch.
	 * The shell task is blocked in the keyboard loop at this point,
	 * so input_buffer is safe to reuse. */
	while (job->cmd[i] && i + 1 < sizeof(input_buffer))
	{
		input_buffer[i] = job->cmd[i];
		i++;
	}
	input_buffer[i] = '\0';
	input_length = i;

	run_command_dispatch();

	job->active = 0;
	terminal_write("\n[bg] Done: ");
	terminal_write_line(job->cmd);
	if (!editor_active && !script_mode_active) terminal_prompt();
	task_exit();
}

static void cmd_jobs(void)
{
	int i, found = 0;
	char n[16];
	for (i = 0; i < BG_JOB_MAX; i++)
	{
		if (bg_jobs[i].active)
		{
			terminal_write("[");
			uint_to_dec((unsigned long)(i + 1), n, sizeof(n));
			terminal_write(n);
			terminal_write("] tid ");
			uint_to_dec((unsigned long)bg_jobs[i].task_id, n, sizeof(n));
			terminal_write(n);
			terminal_write("  ");
			terminal_write_line(bg_jobs[i].cmd);
			found = 1;
		}
	}
	if (!found)
		terminal_write_line("No background jobs.");
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
		int safe_enabled;
		char last_status[48];
		char last_source[48];
		unsigned long delay_seconds = terminal_get_autorun_delay_seconds();
		mode = terminal_get_autorun_mode();
		safe_enabled = terminal_get_autorun_safe_mode();
		terminal_get_autorun_last_status(last_status, sizeof(last_status));
		terminal_get_autorun_last_source(last_source, sizeof(last_source));
		terminal_write("autorun: mode=");
		if (mode == 0) terminal_write("off");
		else if (mode == 2) terminal_write("once");
		else terminal_write("always");
		terminal_write(" safe=");
		terminal_write(safe_enabled ? "on" : "off");
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
		terminal_write(" last=");
		terminal_write(last_status);
		terminal_write(" src=");
		terminal_write(last_source);
		terminal_putc('\n');
		return;
	}
	if (string_equals(tok, "log"))
	{
		char last_status[48];
		char last_source[48];
		terminal_get_autorun_last_status(last_status, sizeof(last_status));
		terminal_get_autorun_last_source(last_source, sizeof(last_source));
		terminal_write("autorun log: status=");
		terminal_write(last_status);
		terminal_write(" source=");
		terminal_write_line(last_source);
		return;
	}
	if (string_equals(tok, "safe"))
	{
		char mode_tok[8];
		int enabled;
		if (read_token(rest, mode_tok, sizeof(mode_tok)) == (void *)0 || mode_tok[0] == '\0' || string_equals(mode_tok, "show"))
		{
			terminal_write("autorun: safe=");
			terminal_write_line(terminal_get_autorun_safe_mode() ? "on" : "off");
			return;
		}
		if (!string_equals(mode_tok, "on") && !string_equals(mode_tok, "off"))
		{
			terminal_write_line("Usage: autorun safe [on|off|show]");
			return;
		}
		enabled = string_equals(mode_tok, "on") ? 1 : 0;
		terminal_set_autorun_safe_mode(enabled);
		autorun_safe_latched = 0;
		terminal_write("autorun: safe mode ");
		terminal_write_line(enabled ? "on" : "off");
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
		terminal_set_autorun_last_status("canceled\n");
		terminal_set_autorun_last_source("none\n");
		terminal_write_line("autorun: canceled for this boot");
		return;
	}
	if (string_equals(tok, "run"))
	{
		int rc = terminal_run_autorun_script_now();
		if (rc > 0)
		{
			terminal_set_autorun_last_status("executed\n");
			terminal_write_line("autorun: executed");
		}
		else if (rc == 0)
		{
			terminal_set_autorun_last_status("no-script\n");
			terminal_set_autorun_last_source("none\n");
			terminal_write_line("autorun: no script found");
		}
		else
		{
			terminal_set_autorun_last_status("error\n");
			terminal_write_line("autorun: script failed");
		}
		return;
	}

	terminal_write_line("Usage: autorun [show|log|off|always|once|rearm|stop|run|safe <on|off|show>|delay <0..3600>]");
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

/* ── RTC (CMOS Real-Time Clock) ────────────────────────────────── */

static unsigned char rtc_read(unsigned char reg)
{
	arch_outb(0x70, reg);
	return arch_inb(0x71);
}

static int bcd_to_bin(unsigned char bcd)
{
	return ((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F);
}

static void cmd_date(const char *args)
{
	unsigned char sec, min, hr, day, mon, yr, century, status_b;
	int year;
	char buf[32];
	int i;
	static const char *months[] = {
		"Jan","Feb","Mar","Apr","May","Jun",
		"Jul","Aug","Sep","Oct","Nov","Dec"
	};
	static const char *full_months[] = {
		"January","February","March","April","May","June",
		"July","August","September","October","November","December"
	};
	static const char *days[] = {
		"Sun","Mon","Tue","Wed","Thu","Fri","Sat"
	};
	static const char *full_days[] = {
		"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
	};

	/* Wait for RTC update-in-progress to clear */
	while (rtc_read(0x0A) & 0x80) {}

	sec     = rtc_read(0x00);
	min     = rtc_read(0x02);
	hr      = rtc_read(0x04);
	day     = rtc_read(0x07);
	mon     = rtc_read(0x08);
	yr      = rtc_read(0x09);
	century = rtc_read(0x32); /* century register (may be 0) */

	status_b = rtc_read(0x0B);

	/* Convert BCD to binary if needed */
	if (!(status_b & 0x04))
	{
		sec = (unsigned char)bcd_to_bin(sec);
		min = (unsigned char)bcd_to_bin(min);
		hr  = (unsigned char)bcd_to_bin(hr & 0x7F);
		day = (unsigned char)bcd_to_bin(day);
		mon = (unsigned char)bcd_to_bin(mon);
		yr  = (unsigned char)bcd_to_bin(yr);
		century = (unsigned char)bcd_to_bin(century);
	}

	/* Handle 12-hour mode */
	if (!(status_b & 0x02) && (rtc_read(0x04) & 0x80))
	{
		hr = (unsigned char)((hr + 12) % 24);
	}

	year = (century > 0) ? (int)century * 100 + (int)yr : 2000 + (int)yr;

	/* Day of week: Tomohiko Sakamoto's algorithm */
	{
		static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
		int y = year;
		int m = (int)mon;
		int d = (int)day;
		if (m < 3) y--;
		i = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
	}

	/* If +FORMAT given, use custom format */
	if (args && args[0] == '+')
	{
		const char *f = args + 1;
		char out[256];
		int oi = 0;
		while (*f && oi < 254)
		{
			if (*f == '%' && *(f+1))
			{
				f++;
				char tmp[16];
				const char *ins = tmp;
				switch (*f)
				{
					case 'Y': uint_to_dec((unsigned long)year, tmp, sizeof(tmp)); break;
					case 'm': tmp[0]='0'+mon/10; tmp[1]='0'+mon%10; tmp[2]=0; break;
					case 'd': tmp[0]='0'+day/10; tmp[1]='0'+day%10; tmp[2]=0; break;
					case 'H': tmp[0]='0'+hr/10;  tmp[1]='0'+hr%10;  tmp[2]=0; break;
					case 'M': tmp[0]='0'+min/10; tmp[1]='0'+min%10; tmp[2]=0; break;
					case 'S': tmp[0]='0'+sec/10; tmp[1]='0'+sec%10; tmp[2]=0; break;
					case 'a': ins = days[i]; break;
					case 'A': ins = full_days[i]; break;
					case 'b': case 'h': ins = (mon>=1&&mon<=12) ? months[mon-1] : "???"; break;
					case 'B': ins = (mon>=1&&mon<=12) ? full_months[mon-1] : "???"; break;
					case 'p': ins = (hr < 12) ? "AM" : "PM"; break;
					case 'I': { unsigned char h12 = (hr==0)?12:((hr>12)?(hr-12):hr); tmp[0]='0'+h12/10; tmp[1]='0'+h12%10; tmp[2]=0; break; }
					case 'n': tmp[0]='\n'; tmp[1]=0; break;
					case '%': tmp[0]='%'; tmp[1]=0; break;
					default: tmp[0]='%'; tmp[1]=*f; tmp[2]=0; break;
				}
				while (*ins && oi < 254) out[oi++] = *ins++;
				f++;
			}
			else
				out[oi++] = *f++;
		}
		out[oi] = '\0';
		terminal_write_line(out);
		return;
	}

	/* Default format: "Wed Apr 07 14:30:25 2026" */
	terminal_write(days[i]);
	terminal_write(" ");
	if (mon >= 1 && mon <= 12)
		terminal_write(months[mon - 1]);
	else
		terminal_write("???");
	terminal_write(" ");
	buf[0] = (char)('0' + day / 10);
	buf[1] = (char)('0' + day % 10);
	buf[2] = '\0';
	terminal_write(buf);
	terminal_write(" ");
	buf[0] = (char)('0' + hr / 10);
	buf[1] = (char)('0' + hr % 10);
	buf[2] = ':';
	buf[3] = (char)('0' + min / 10);
	buf[4] = (char)('0' + min % 10);
	buf[5] = ':';
	buf[6] = (char)('0' + sec / 10);
	buf[7] = (char)('0' + sec % 10);
	buf[8] = '\0';
	terminal_write(buf);
	terminal_write(" ");
	uint_to_dec((unsigned long)year, buf, sizeof(buf));
	terminal_write_line(buf);
}

static void cmd_uptime(void)
{
	unsigned long t = timer_ticks();
	unsigned long total_secs = t / 100;
	unsigned long days_val   = total_secs / 86400;
	unsigned long hours_val  = (total_secs % 86400) / 3600;
	unsigned long mins_val   = (total_secs % 3600) / 60;
	unsigned long secs_val   = total_secs % 60;
	char n[16];

	terminal_write("up ");
	if (days_val > 0)
	{
		uint_to_dec(days_val, n, sizeof(n));
		terminal_write(n);
		terminal_write("d ");
	}
	uint_to_dec(hours_val, n, sizeof(n));
	terminal_write(n);
	terminal_write("h ");
	uint_to_dec(mins_val, n, sizeof(n));
	terminal_write(n);
	terminal_write("m ");
	uint_to_dec(secs_val, n, sizeof(n));
	terminal_write(n);
	terminal_write_line("s");
}

static void cmd_elfsegs(const char *args)
{
	char path[128];
	unsigned long size = 0;
	struct elf_phdr_print_ctx phdr_ctx;
	int rc;
	const char *p = read_token(args, path, sizeof(path));

	if (p == (void *)0 || path[0] == '\0' || skip_spaces(p)[0] != '\0')
	{
		terminal_write_line("Usage: elfsegs <path>");
		return;
	}
	if (read_shell_file_bytes("elfsegs", path, elf_shell_io_buf, ELF_SHELL_IO_MAX, &size) != 0) return;

	terminal_write("elfsegs: ");
	terminal_write_line(path);
	terminal_write_line("  program headers:");
	phdr_ctx.count = 0;
	phdr_ctx.entry = 0;
	phdr_ctx.load_count = 0;
	phdr_ctx.entry_in_load = 0;
	phdr_ctx.overlap_detected = 0;
	phdr_ctx.align_mismatch = 0;
	phdr_ctx.range_overflow = 0;
	rc = elf_visit_program_headers(elf_shell_io_buf, size, print_elf_phdr, &phdr_ctx);
	if (rc == ELF_ERR_PHDR)
	{
		terminal_write_line("    none");
		return;
	}
	if (rc != ELF_OK)
	{
		terminal_write_line("    <parse failed>");
		return;
	}
	if (phdr_ctx.count == 0)
	{
		terminal_write_line("    none");
		return;
	}
	if (phdr_ctx.overlap_detected) terminal_write_line("  warning: PT_LOAD page overlap detected");
	if (phdr_ctx.align_mismatch) terminal_write_line("  warning: PT_LOAD alignment mismatch detected");
	if (phdr_ctx.range_overflow) terminal_write_line("  warning: PT_LOAD range overflow detected");
}

static void cmd_elfsects(const char *args)
{
	char path[128];
	unsigned long size = 0;
	struct elf_section_print_ctx section_ctx;
	int rc;
	const char *p = read_token(args, path, sizeof(path));

	if (p == (void *)0 || path[0] == '\0' || skip_spaces(p)[0] != '\0')
	{
		terminal_write_line("Usage: elfsects <path>");
		return;
	}
	if (read_shell_file_bytes("elfsects", path, elf_shell_io_buf, ELF_SHELL_IO_MAX, &size) != 0) return;

	terminal_write("elfsects: ");
	terminal_write_line(path);
	terminal_write_line("  sections:");
	section_ctx.count = 0;
	rc = elf_visit_sections(elf_shell_io_buf, size, print_elf_section, &section_ctx);
	if (rc == ELF_ERR_SHDR)
	{
		terminal_write_line("    none");
		return;
	}
	if (rc != ELF_OK)
	{
		terminal_write_line("    <parse failed>");
		return;
	}
	if (section_ctx.count == 0) terminal_write_line("    none");
}

static void cmd_elfinfo(const char *args)
{
	char path[128];
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
	if (read_shell_file_bytes("elfinfo", path, elf_shell_io_buf, ELF_SHELL_IO_MAX, &size) != 0) return;

	rc = elf_get_info(elf_shell_io_buf, size, &info);
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
	phdr_ctx.entry = info.entry;
	phdr_ctx.load_count = 0;
	phdr_ctx.entry_in_load = 0;
	phdr_ctx.overlap_detected = 0;
	phdr_ctx.align_mismatch = 0;
	phdr_ctx.range_overflow = 0;
	rc = elf_visit_program_headers(elf_shell_io_buf, size, print_elf_phdr, &phdr_ctx);
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
	if (rc == ELF_OK && phdr_ctx.count != 0)
	{
		terminal_write("  entry in PT_LOAD: ");
		terminal_write_line(phdr_ctx.entry_in_load ? "yes" : "no");
		if (phdr_ctx.overlap_detected) terminal_write_line("  warning: PT_LOAD page overlap detected");
		if (phdr_ctx.align_mismatch) terminal_write_line("  warning: PT_LOAD alignment mismatch detected");
		if (phdr_ctx.range_overflow) terminal_write_line("  warning: PT_LOAD range overflow detected");
	}

	terminal_write_line("  sections:");
	section_ctx.count = 0;
	rc = elf_visit_sections(elf_shell_io_buf, size, print_elf_section, &section_ctx);
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
	if (read_shell_file_bytes("elfsym", path, elf_shell_io_buf, ELF_SHELL_IO_MAX, &size) != 0) return;

	ctx.filter = filter[0] == '\0' ? (void *)0 : filter;
	ctx.total = 0;
	ctx.matched = 0;

	terminal_write("elfsym: ");
	terminal_write_line(path);
	rc = elf_visit_symbols(elf_shell_io_buf, size, print_elf_symbol, &ctx);
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
	unsigned long size = 0;
	unsigned long addr;
	unsigned long offset;
	unsigned long image_base = 0;
	unsigned long image_end = 0;
	elf_symbol_t sym;
	const char *end;
	const char *p = read_token(args, path, sizeof(path));
	int rc;

	if (p == (void *)0 || path[0] == '\0')
	{
		terminal_write_line("Usage: elfaddr <path> <hex-address> | elfaddr <hex-address>");
		return;
	}
	p = read_token(p, addr_tok, sizeof(addr_tok));
	if (p == (void *)0)
	{
		terminal_write_line("Usage: elfaddr <path> <hex-address> | elfaddr <hex-address>");
		return;
	}

	/* One-argument mode: resolve address against currently active ELF images. */
	if (addr_tok[0] == '\0')
	{
		if (skip_spaces(p)[0] != '\0')
		{
			terminal_write_line("Usage: elfaddr <path> <hex-address> | elfaddr <hex-address>");
			return;
		}
		addr = parse_hex(path, &end);
		if (*end != '\0')
		{
			terminal_write_line("elfaddr: bad hex address");
			return;
		}
		rc = elf_symbolize_active_addr(addr, &sym, &offset, &image_base, &image_end);
		if (rc != ELF_OK)
		{
			terminal_write_line("elfaddr: no matching active symbol");
			return;
		}
		terminal_write("elfaddr: ");
		terminal_write_hex64(addr);
		terminal_write(" -> ");
		terminal_write(sym.name);
		terminal_write("+");
		terminal_write_hex64(offset);
		terminal_write(" (active image ");
		terminal_write_hex64(image_base);
		terminal_write("-");
		terminal_write_hex64(image_end);
		terminal_write(")");
		terminal_putc('\n');
		return;
	}

	if (skip_spaces(p)[0] != '\0')
	{
		terminal_write_line("Usage: elfaddr <path> <hex-address> | elfaddr <hex-address>");
		return;
	}
	addr = parse_hex(addr_tok, &end);
	if (*end != '\0')
	{
		terminal_write_line("elfaddr: bad hex address");
		return;
	}
	if (read_shell_file_bytes("elfaddr", path, elf_shell_io_buf, ELF_SHELL_IO_MAX, &size) != 0) return;

	rc = elf_find_symbol_by_addr(elf_shell_io_buf, size, addr, &sym, &offset);
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

	if (read_shell_file_bytes("execstress", path, elf_shell_io_buf, ELF_SHELL_IO_MAX, &size) != 0) return;
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
		last_rc = elf_load(elf_shell_io_buf, size, &prog);
		if (last_rc != 0) break;
		last_ret = elf_call(&prog);
		elf_unload(elf_shell_io_buf, size);
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
		{
			char n[16];
			terminal_write(elf_load_error_name(last_rc));
			terminal_write(" (");
			if (last_rc < 0)
			{
				terminal_putc('-');
				uint_to_dec((unsigned long)(-last_rc), n, sizeof(n));
			}
			else
			{
				uint_to_dec((unsigned long)last_rc, n, sizeof(n));
			}
			terminal_write(n);
			terminal_write_line(")");
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
		{"/app2p.elf", 99},
		{"/bss.elf", 13},
		{"/pie.elf", 123}
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

		if (fs_read_file(tests[i].path, elf_shell_io_buf, ELF_SHELL_IO_MAX, &size) != 0)
		{
			int st_dir = 0;
			unsigned long st_size = 0;
			terminal_write("  FAIL "); terminal_write(tests[i].path); terminal_write_line(" : read failed");
			if (fs_stat(tests[i].path, &st_dir, &st_size) != 0)
			{
				terminal_write("    note: missing path "); terminal_write_line(tests[i].path);
			}
			else
			{
				char n[16];
				terminal_write("    note: stat ok type=");
				terminal_write(st_dir ? "dir" : "file");
				terminal_write(" size=");
				uint_to_dec(st_size, n, sizeof(n));
				terminal_write_line(n);
			}
			failures++;
			continue;
		}
		rc = elf_load(elf_shell_io_buf, size, &prog);
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
		elf_unload(elf_shell_io_buf, size);
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
		unsigned long size = 0;
		elf_info_t info;
		elf_symbol_t sym;
		unsigned long offset = 0;
		if (fs_read_file("/app.elf", elf_shell_io_buf, ELF_SHELL_IO_MAX, &size) != 0)
		{
			terminal_write_line("  FAIL symbols /app.elf : read failed");
			failures++;
		}
		else if (elf_get_info(elf_shell_io_buf, size, &info) != ELF_OK || info.symbol_count == 0)
		{
			terminal_write_line("  FAIL symbols /app.elf : symbol table missing");
			failures++;
		}
		else if (elf_find_symbol_by_addr(elf_shell_io_buf, size, info.entry, &sym, &offset) != ELF_OK || offset != 0 || !string_equals(sym.name, "_start"))
		{
			terminal_write_line("  FAIL symbols /app.elf : entry lookup mismatch");
			failures++;
		}
		else
		{
			terminal_write_line("  PASS symbols /app.elf");
		}
	}

	{
		unsigned long size = 0;
		elf_exec_t prog;
		elf_symbol_t sym;
		unsigned long offset = 0;
		unsigned long image_base = 0;
		unsigned long image_end = 0;
		int rc;
		if (fs_read_file("/pie.elf", elf_shell_io_buf, ELF_SHELL_IO_MAX, &size) != 0)
		{
			terminal_write_line("  FAIL active-symbol /pie.elf : read failed");
			failures++;
		}
		else if ((rc = elf_load(elf_shell_io_buf, size, &prog)) != ELF_OK)
		{
			(void)rc;
			terminal_write_line("  FAIL active-symbol /pie.elf : load failed");
			failures++;
		}
		else if (elf_symbolize_active_addr(prog.entry, &sym, &offset, &image_base, &image_end) != ELF_OK ||
			offset != 0 || !string_equals(sym.name, "_start") || image_end <= image_base)
		{
			terminal_write_line("  FAIL active-symbol /pie.elf : symbol lookup mismatch");
			elf_unload(elf_shell_io_buf, size);
			failures++;
		}
		else
		{
			terminal_write_line("  PASS active-symbol /pie.elf");
			elf_unload(elf_shell_io_buf, size);
		}
	}

	/* Short stress pass on multi-page image to catch regressions quickly. */
	before_free = memory_free_pages();
	{
		unsigned long size = 0;
		unsigned long ok_runs = 0;
		int rc = 0;
		if (fs_read_file("/app2p.elf", elf_shell_io_buf, ELF_SHELL_IO_MAX, &size) != 0)
		{
			terminal_write_line("  FAIL stress /app2p.elf : read failed");
			failures++;
		}
		else
		{
			for (i = 0; i < 256UL; i++)
			{
				elf_exec_t prog;
				rc = elf_load(elf_shell_io_buf, size, &prog);
				if (rc != 0) break;
				(void)elf_call(&prog);
				elf_unload(elf_shell_io_buf, size);
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
	terminal_write_line("  note: exec /hello.elf to validate SYS_WRITE + SYS_EXIT in user mode");
	terminal_write_line("  note: exec /argc.elf a b c to validate argc/argv stack setup");
	terminal_write_line("  note: exec /brk.elf to validate SYS_BRK heap growth/shrink");
}

static void cmd_pwd(void)
{
	if (fat_mode_active())
	{
		/* Show the active drive's CWD, not the temporarily mounted drive's CWD */
		int active = (fat_active_drive_index >= 0) ? fat_active_drive_index : fat_mounted_drive_index;
		if (active >= 0 && active != fat_mounted_drive_index)
			terminal_write_line(fat_drive_cwd[active]);
		else
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
		for (i = 0; i < fat_count; i++)
		{
			/* Color directories bright blue */
			char child_path[128];
			unsigned long ci2 = 0, k;
			int cis_dir = 0;
			unsigned long csz = 0;
			for (k = 0; full_path[k] && ci2 + 1 < sizeof(child_path); k++) child_path[ci2++] = full_path[k];
			if (ci2 > 0 && child_path[ci2 - 1] != '/') child_path[ci2++] = '/';
			for (k = 0; fat_names[i][k] && ci2 + 1 < sizeof(child_path); k++) child_path[ci2++] = fat_names[i][k];
			child_path[ci2] = '\0';
			if (fat32_stat_path(child_path, &cis_dir, &csz) == 0 && cis_dir)
				screen_set_color(0x09);
			terminal_write_line(fat_names[i]);
			screen_set_color(terminal_text_color);
		}
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
		if (types[i]) screen_set_color(0x09); /* bright blue for dirs */
		terminal_write_line(names[i]);
		if (types[i]) screen_set_color(terminal_text_color);
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
	unsigned int batch_override = 0;
	int batch_override_set = 0;
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
			if (ascii_upper(tok[1]) == 'R' && tok[2] != '\0')
			{
				if (parse_dec_u32(tok + 2, &batch_override) != 0 || fat32_set_io_batch_sectors(batch_override) != 0)
				{
					terminal_write_line("dir: invalid /rN (N=1..16)");
					return;
				}
				batch_override_set = 1;
				continue;
			}
			unsigned long k = 1;
			while (tok[k] != '\0')
			{
				char c = ascii_upper(tok[k]);
				if (c == 'B') opt_bare = 1;
				else if (c == 'W') opt_wide = 1;
				else if (c == 'S') opt_recursive = 1;
				else { terminal_write_line("dir: unknown option (use /b /w /s /rN)"); return; }
				k++;
			}
			continue;
		}
		if (target[0] != '\0')
		{
			terminal_write_line("Usage: dir [/b] [/w] [/s] [/rN] [path]");
			return;
		}
		{
			unsigned long k = 0;
			while (tok[k] != '\0' && k + 1 < sizeof(target)) { target[k] = tok[k]; k++; }
			target[k] = '\0';
		}
	}

	if (batch_override_set && !fat_mode_active())
	{
		terminal_write_line("dir: /rN applied for next FAT operations");
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
		if (fat_mounted_drive_index >= 0 && fat_mounted_drive_index < BLOCKDEV_MAX_DRIVES)
		{
			unsigned long i = 0;
			while (fat_cwd[i] != '\0' && i + 1 < sizeof(fat_drive_cwd[0])) { fat_drive_cwd[fat_mounted_drive_index][i] = fat_cwd[i]; i++; }
			fat_drive_cwd[fat_mounted_drive_index][i] = '\0';
		}
		/* cd is an explicit navigation — the drive we landed on becomes the active drive */
		fat_active_drive_index = fat_mounted_drive_index;
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
		if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0)
		{
			terminal_write_line("mkdir: failed (bad path)");
			return;
		}
		if (fat32_mkdir_path(full_path) != 0)
		{
			terminal_write_fat_failure("mkdir: failed");
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
		if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0)
		{
			terminal_write_line("touch: failed (bad path)");
			return;
		}
		if (fat32_touch_file_path(full_path) != 0)
		{
			terminal_write_fat_failure("touch: failed");
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
		if (pipe_stdin_buf && pipe_stdin_len > 0)
		{
			unsigned long pi2;
			for (pi2 = 0; pi2 < pipe_stdin_len; pi2++)
				terminal_putc(pipe_stdin_buf[pi2]);
			if (pipe_stdin_len > 0 && pipe_stdin_buf[pipe_stdin_len - 1] != '\n')
				terminal_putc('\n');
			return;
		}
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
		terminal_write_fat_failure("cat: failed");
		return;
	}

	if (fs_read_text(path, &text) != 0)
	{
		terminal_write_line("cat: failed");
		return;
	}
	terminal_write_line(text);
}

static int simple_pattern_match(const char *text, const char *pattern)
{
	/* Simple substring search (case-insensitive) */
	unsigned long tlen = 0, plen = 0, i, j;
	while (text[tlen]) tlen++;
	while (pattern[plen]) plen++;
	if (plen == 0) return 1;
	if (plen > tlen) return 0;
	for (i = 0; i <= tlen - plen; i++)
	{
		int match = 1;
		for (j = 0; j < plen; j++)
		{
			char tc = text[i + j], pc = pattern[j];
			if (tc >= 'A' && tc <= 'Z') tc += 32;
			if (pc >= 'A' && pc <= 'Z') pc += 32;
			if (tc != pc) { match = 0; break; }
		}
		if (match) return 1;
	}
	return 0;
}

static void cmd_grep(const char *args)
{
	char pattern[128], path[128], full_path[128];
	const char *p;
	int case_sensitive = 1, line_numbers = 0, invert = 0, count_only = 0;

	p = args;
	while (*p == '-')
	{
		if (p[1] == 'i' && (p[2] == ' ' || p[2] == '\0')) { case_sensitive = 0; p += 2; }
		else if (p[1] == 'n' && (p[2] == ' ' || p[2] == '\0')) { line_numbers = 1; p += 2; }
		else if (p[1] == 'v' && (p[2] == ' ' || p[2] == '\0')) { invert = 1; p += 2; }
		else if (p[1] == 'c' && (p[2] == ' ' || p[2] == '\0')) { count_only = 1; p += 2; }
		else break;
		while (*p == ' ') p++;
	}
	p = read_token(p, pattern, sizeof(pattern));
	if (p == (void *)0 || pattern[0] == '\0')
	{
		terminal_write_line("Usage: grep [-i] [-n] [-v] [-c] <pattern> <file>");
		return;
	}
	p = read_token(p, path, sizeof(path));
	if (p == (void *)0 || path[0] == '\0')
	{
		if (pipe_stdin_buf && pipe_stdin_len > 0)
		{
			/* Use pipe stdin as data source */
			unsigned long size = pipe_stdin_len;
			unsigned long si, line_start2;
			int line_no2, match_count = 0;
			line_start2 = 0;
			line_no2 = 1;
			for (si = 0; si <= size; si++)
			{
				if (si == size || pipe_stdin_buf[si] == '\n')
				{
					char line_buf[512];
					unsigned long ll = si - line_start2;
					int matched;
					if (ll >= sizeof(line_buf)) ll = sizeof(line_buf) - 1;
					{
						unsigned long k;
						for (k = 0; k < ll; k++) line_buf[k] = pipe_stdin_buf[line_start2 + k];
					}
					if (ll > 0 && line_buf[ll - 1] == '\r') ll--;
					line_buf[ll] = '\0';
					if (case_sensitive)
					{
						unsigned long pi2, patlen = 0;
						while (pattern[patlen]) patlen++;
						matched = 0;
						if (patlen == 0) matched = 1;
						else if (patlen <= ll)
						{
							for (pi2 = 0; pi2 <= ll - patlen; pi2++)
							{
								int ok = 1;
								unsigned long li2;
								for (li2 = 0; li2 < patlen; li2++)
								{
									if (line_buf[pi2 + li2] != pattern[li2]) { ok = 0; break; }
								}
								if (ok) { matched = 1; break; }
							}
						}
					}
					else
					{
						matched = simple_pattern_match(line_buf, pattern);
					}
					if (invert) matched = !matched;
					if (matched)
					{
						match_count++;
						if (!count_only)
						{
							if (line_numbers)
							{
								char num_buf[16];
								uint_to_dec((unsigned long)line_no2, num_buf, sizeof(num_buf));
								terminal_write(num_buf);
								terminal_write(":");
							}
							terminal_write_line(line_buf);
						}
					}
					line_no2++;
					line_start2 = si + 1;
				}
			}
			if (count_only)
			{
				char num_buf[16];
				uint_to_dec((unsigned long)match_count, num_buf, sizeof(num_buf));
				terminal_write_line(num_buf);
			}
			return;
		}
		terminal_write_line("Usage: grep [-i] [-n] [-v] [-c] <pattern> <file>");
		return;
	}
	{
		unsigned char data[FS_MAX_FILE_SIZE];
		unsigned long size = 0;
		unsigned long i, line_start;
		int line_no;
		if (fat_mode_active())
		{
			if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0 ||
				fat32_read_file_path(full_path, data, sizeof(data) - 1, &size) != 0)
			{
				terminal_write_line("grep: cannot read file");
				return;
			}
		}
		else
		{
			if (fs_read_file(path, data, sizeof(data) - 1, &size) != 0)
			{
				terminal_write_line("grep: cannot read file");
				return;
			}
		}
		data[size] = '\0';
		line_start = 0;
		line_no = 1;
		{
		int match_count2 = 0;
		for (i = 0; i <= size; i++)
		{
			if (i == size || data[i] == '\n')
			{
				char line_buf[512];
				unsigned long ll = i - line_start;
				int matched;
				if (ll >= sizeof(line_buf)) ll = sizeof(line_buf) - 1;
				{
					unsigned long k;
					for (k = 0; k < ll; k++) line_buf[k] = (char)data[line_start + k];
				}
				if (ll > 0 && line_buf[ll - 1] == '\r') ll--;
				line_buf[ll] = '\0';
				if (case_sensitive)
				{
					unsigned long pi, li2;
					unsigned long patlen = 0;
					while (pattern[patlen]) patlen++;
					matched = 0;
					if (patlen == 0) matched = 1;
					else if (patlen <= ll)
					{
						for (pi = 0; pi <= ll - patlen; pi++)
						{
							int ok = 1;
							for (li2 = 0; li2 < patlen; li2++)
							{
								if (line_buf[pi + li2] != pattern[li2]) { ok = 0; break; }
							}
							if (ok) { matched = 1; break; }
						}
					}
				}
				else
				{
					matched = simple_pattern_match(line_buf, pattern);
				}
				if (invert) matched = !matched;
				if (matched)
				{
					match_count2++;
					if (!count_only)
					{
						if (line_numbers)
						{
							char num_buf[16];
							uint_to_dec((unsigned long)line_no, num_buf, sizeof(num_buf));
							terminal_write(num_buf);
							terminal_write(":");
						}
						terminal_write_line(line_buf);
					}
				}
				line_no++;
				line_start = i + 1;
			}
		}
		if (count_only)
		{
			char num_buf[16];
			uint_to_dec((unsigned long)match_count2, num_buf, sizeof(num_buf));
			terminal_write_line(num_buf);
		}
		}
	}
}

static void cmd_find(const char *args)
{
	char pattern[128];
	char start_path[128];
	char names[FS_MAX_LIST][FS_NAME_MAX + 2];
	int types[FS_MAX_LIST];
	int count, i;
	const char *p;

	p = read_token(args, pattern, sizeof(pattern));
	if (p == (void *)0 || pattern[0] == '\0')
	{
		terminal_write_line("Usage: find <pattern> [path]");
		return;
	}
	p = read_token(p, start_path, sizeof(start_path));
	if (start_path[0] == '\0')
	{
		start_path[0] = '/';
		start_path[1] = '\0';
	}
	if (fs_ls(start_path, names, types, FS_MAX_LIST, &count) != 0)
	{
		terminal_write_line("find: cannot list directory");
		return;
	}
	for (i = 0; i < count; i++)
	{
		if (simple_pattern_match(names[i], pattern))
		{
			if (start_path[0] != '/' || start_path[1] != '\0')
			{
				terminal_write(start_path);
				terminal_write("/");
			}
			else
			{
				terminal_write("/");
			}
			terminal_write_line(names[i]);
		}
	}
}

static void cmd_wc(const char *args)
{
	char path[128], full_path[128];
	const char *p = args;
	int show_l = 0, show_w = 0, show_c = 0;
	/* Parse flags */
	while (*p == '-')
	{
		if (p[1] == 'l' && (p[2] == ' ' || p[2] == '\0')) { show_l = 1; p += 2; }
		else if (p[1] == 'w' && (p[2] == ' ' || p[2] == '\0')) { show_w = 1; p += 2; }
		else if (p[1] == 'c' && (p[2] == ' ' || p[2] == '\0')) { show_c = 1; p += 2; }
		else break;
		while (*p == ' ') p++;
	}
	if (!show_l && !show_w && !show_c) { show_l = 1; show_w = 1; show_c = 1; }
	p = read_token(p, path, sizeof(path));
	if (p == (void *)0 || path[0] == '\0')
	{
		if (pipe_stdin_buf && pipe_stdin_len > 0)
		{
			unsigned long si;
			int lines2 = 0, words2 = 0, in_w = 0;
			char num_buf[16];
			for (si = 0; si < pipe_stdin_len; si++)
			{
				char c = pipe_stdin_buf[si];
				if (c == '\n') lines2++;
				if (c == ' ' || c == '\t' || c == '\n' || c == '\r') in_w = 0;
				else { if (!in_w) words2++; in_w = 1; }
			}
			if (show_l) { uint_to_dec((unsigned long)lines2, num_buf, sizeof(num_buf)); terminal_write("  "); terminal_write(num_buf); }
			if (show_w) { uint_to_dec((unsigned long)words2, num_buf, sizeof(num_buf)); terminal_write("  "); terminal_write(num_buf); }
			if (show_c) { uint_to_dec(pipe_stdin_len, num_buf, sizeof(num_buf)); terminal_write("  "); terminal_write(num_buf); }
			terminal_write_line(" (stdin)");
			return;
		}
		terminal_write_line("Usage: wc [-l] [-w] [-c] <file>");
		return;
	}
	{
		unsigned char data[FS_MAX_FILE_SIZE];
		unsigned long size = 0, i;
		int lines = 0, words = 0;
		int in_word = 0;
		char num_buf[16];
		if (fat_mode_active())
		{
			if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0 ||
				fat32_read_file_path(full_path, data, sizeof(data) - 1, &size) != 0)
			{
				terminal_write_line("wc: cannot read file");
				return;
			}
		}
		else
		{
			if (fs_read_file(path, data, sizeof(data) - 1, &size) != 0)
			{
				terminal_write_line("wc: cannot read file");
				return;
			}
		}
		for (i = 0; i < size; i++)
		{
			if (data[i] == '\n') lines++;
			if (data[i] == ' ' || data[i] == '\t' || data[i] == '\n' || data[i] == '\r')
			{
				in_word = 0;
			}
			else
			{
				if (!in_word) words++;
				in_word = 1;
			}
		}
		if (show_l) { uint_to_dec((unsigned long)lines, num_buf, sizeof(num_buf)); terminal_write("  "); terminal_write(num_buf); }
		if (show_w) { uint_to_dec((unsigned long)words, num_buf, sizeof(num_buf)); terminal_write("  "); terminal_write(num_buf); }
		if (show_c) { uint_to_dec(size, num_buf, sizeof(num_buf)); terminal_write("  "); terminal_write(num_buf); }
		terminal_write(" ");
		terminal_write_line(path);
	}
}

static void cmd_head(const char *args)
{
	char path[128], full_path[128];
	int n = 10;
	const char *p = args;

	if (*p == '-')
	{
		p++;
		n = 0;
		while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
		while (*p == ' ') p++;
	}
	{
		char ppath[128];
		p = read_token(p, ppath, sizeof(ppath));
		if (p == (void *)0 || ppath[0] == '\0')
		{
			if (pipe_stdin_buf && pipe_stdin_len > 0)
			{
				unsigned long si, ls2 = 0;
				int ln2 = 0;
				for (si = 0; si <= pipe_stdin_len && ln2 < n; si++)
				{
					if (si == pipe_stdin_len || pipe_stdin_buf[si] == '\n')
					{
						char lb[512];
						unsigned long ll = si - ls2;
						unsigned long k2;
						if (ll >= sizeof(lb)) ll = sizeof(lb) - 1;
						for (k2 = 0; k2 < ll; k2++) lb[k2] = pipe_stdin_buf[ls2 + k2];
						if (ll > 0 && lb[ll - 1] == '\r') ll--;
						lb[ll] = '\0';
						terminal_write_line(lb);
						ln2++;
						ls2 = si + 1;
					}
				}
				return;
			}
			terminal_write_line("Usage: head [-N] <file>");
			return;
		}
		{
			unsigned long k;
			for (k = 0; ppath[k]; k++) path[k] = ppath[k];
			path[k] = '\0';
		}
	}
	{
		unsigned char data[FS_MAX_FILE_SIZE];
		unsigned long size = 0, i, line_start;
		int line_no;
		if (fat_mode_active())
		{
			if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0 ||
				fat32_read_file_path(full_path, data, sizeof(data) - 1, &size) != 0)
			{
				terminal_write_line("head: cannot read file");
				return;
			}
		}
		else
		{
			if (fs_read_file(path, data, sizeof(data) - 1, &size) != 0)
			{
				terminal_write_line("head: cannot read file");
				return;
			}
		}
		data[size] = '\0';
		line_start = 0;
		line_no = 0;
		for (i = 0; i <= size && line_no < n; i++)
		{
			if (i == size || data[i] == '\n')
			{
				char line_buf[512];
				unsigned long ll = i - line_start;
				if (ll >= sizeof(line_buf)) ll = sizeof(line_buf) - 1;
				{
					unsigned long k;
					for (k = 0; k < ll; k++) line_buf[k] = (char)data[line_start + k];
				}
				if (ll > 0 && line_buf[ll - 1] == '\r') ll--;
				line_buf[ll] = '\0';
				terminal_write_line(line_buf);
				line_no++;
				line_start = i + 1;
			}
		}
	}
}

static void cmd_tail(const char *args)
{
	char path[128], full_path[128];
	int n = 10;
	const char *p = args;

	if (*p == '-')
	{
		p++;
		n = 0;
		while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
		while (*p == ' ') p++;
	}
	{
		char ppath[128];
		p = read_token(p, ppath, sizeof(ppath));
		if (p == (void *)0 || ppath[0] == '\0')
		{
			if (pipe_stdin_buf && pipe_stdin_len > 0)
			{
				unsigned long si, ls2;
				int total2 = 0, start2, ln2;
				for (si = 0; si < pipe_stdin_len; si++)
					if (pipe_stdin_buf[si] == '\n') total2++;
				if (pipe_stdin_len > 0 && pipe_stdin_buf[pipe_stdin_len - 1] != '\n') total2++;
				start2 = total2 - n;
				if (start2 < 0) start2 = 0;
				ls2 = 0; ln2 = 0;
				for (si = 0; si <= pipe_stdin_len; si++)
				{
					if (si == pipe_stdin_len || pipe_stdin_buf[si] == '\n')
					{
						if (ln2 >= start2)
						{
							char lb[512];
							unsigned long ll = si - ls2, k2;
							if (ll >= sizeof(lb)) ll = sizeof(lb) - 1;
							for (k2 = 0; k2 < ll; k2++) lb[k2] = pipe_stdin_buf[ls2 + k2];
							if (ll > 0 && lb[ll - 1] == '\r') ll--;
							lb[ll] = '\0';
							terminal_write_line(lb);
						}
						ln2++;
						ls2 = si + 1;
					}
				}
				return;
			}
			terminal_write_line("Usage: tail [-N] <file>");
			return;
		}
		{
			unsigned long k;
			for (k = 0; ppath[k]; k++) path[k] = ppath[k];
			path[k] = '\0';
		}
	}
	{
		unsigned char data[FS_MAX_FILE_SIZE];
		unsigned long size = 0;
		int total_lines, start_line, line_no;
		unsigned long i, line_start;
		if (fat_mode_active())
		{
			if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0 ||
				fat32_read_file_path(full_path, data, sizeof(data) - 1, &size) != 0)
			{
				terminal_write_line("tail: cannot read file");
				return;
			}
		}
		else
		{
			if (fs_read_file(path, data, sizeof(data) - 1, &size) != 0)
			{
				terminal_write_line("tail: cannot read file");
				return;
			}
		}
		data[size] = '\0';
		total_lines = 0;
		for (i = 0; i < size; i++)
		{
			if (data[i] == '\n') total_lines++;
		}
		if (size > 0 && data[size - 1] != '\n') total_lines++;
		start_line = total_lines - n;
		if (start_line < 0) start_line = 0;
		line_start = 0;
		line_no = 0;
		for (i = 0; i <= size; i++)
		{
			if (i == size || data[i] == '\n')
			{
				if (line_no >= start_line)
				{
					char line_buf[512];
					unsigned long ll = i - line_start;
					if (ll >= sizeof(line_buf)) ll = sizeof(line_buf) - 1;
					{
						unsigned long k;
						for (k = 0; k < ll; k++) line_buf[k] = (char)data[line_start + k];
					}
					if (ll > 0 && line_buf[ll - 1] == '\r') ll--;
					line_buf[ll] = '\0';
					terminal_write_line(line_buf);
				}
				line_no++;
				line_start = i + 1;
			}
		}
	}
}

static void cmd_xxd(const char *args)
{
	char path[128], full_path[128];
	const char *p = read_token(args, path, sizeof(path));
	if (p == (void *)0 || path[0] == '\0')
	{
		if (pipe_stdin_buf && pipe_stdin_len > 0)
		{
			unsigned long i2, j2;
			for (i2 = 0; i2 < pipe_stdin_len; i2 += 16)
			{
				unsigned long row = (pipe_stdin_len - i2 < 16) ? (pipe_stdin_len - i2) : 16;
				char off_buf[16];
				unsigned long off = i2;
				int d;
				for (d = 7; d >= 0; d--) { off_buf[d] = "0123456789abcdef"[off & 0xF]; off >>= 4; }
				off_buf[8] = '\0';
				terminal_write(off_buf); terminal_write(": ");
				for (j2 = 0; j2 < 16; j2++)
				{
					if (j2 < row)
					{
						char hx[3];
						unsigned char b = (unsigned char)pipe_stdin_buf[i2 + j2];
						hx[0] = "0123456789abcdef"[(b >> 4) & 0xF];
						hx[1] = "0123456789abcdef"[b & 0xF];
						hx[2] = '\0';
						terminal_write(hx);
					}
					else terminal_write("  ");
					if (j2 % 2 == 1) terminal_putc(' ');
				}
				terminal_write(" ");
				for (j2 = 0; j2 < row; j2++)
				{
					unsigned char b = (unsigned char)pipe_stdin_buf[i2 + j2];
					terminal_putc(b >= 0x20 && b < 0x7F ? (char)b : '.');
				}
				terminal_putc('\n');
			}
			return;
		}
		terminal_write_line("Usage: xxd <file>");
		return;
	}
	{
		unsigned char data[FS_MAX_FILE_SIZE];
		unsigned long size = 0, i, j;
		if (fat_mode_active())
		{
			if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0 ||
				fat32_read_file_path(full_path, data, sizeof(data) - 1, &size) != 0)
			{
				terminal_write_line("xxd: cannot read file");
				return;
			}
		}
		else
		{
			if (fs_read_file(path, data, sizeof(data), &size) != 0)
			{
				terminal_write_line("xxd: cannot read file");
				return;
			}
		}
		for (i = 0; i < size; i += 16)
		{
			unsigned long row = (size - i < 16) ? (size - i) : 16;
			char off_buf[16];
			unsigned long off = i;
			int d;
			for (d = 7; d >= 0; d--)
			{
				off_buf[d] = "0123456789abcdef"[off & 0xF];
				off >>= 4;
			}
			off_buf[8] = '\0';
			terminal_write(off_buf);
			terminal_write(": ");
			for (j = 0; j < 16; j++)
			{
				if (j < row)
				{
					char hx[3];
					hx[0] = "0123456789abcdef"[(data[i + j] >> 4) & 0xF];
					hx[1] = "0123456789abcdef"[data[i + j] & 0xF];
					hx[2] = '\0';
					terminal_write(hx);
				}
				else
				{
					terminal_write("  ");
				}
				if (j % 2 == 1) terminal_putc(' ');
			}
			terminal_write(" ");
			for (j = 0; j < row; j++)
			{
				unsigned char b = data[i + j];
				terminal_putc(b >= 0x20 && b < 0x7F ? (char)b : '.');
			}
			terminal_putc('\n');
		}
	}
}

/* ------------------------------------------------------------------ */
/* Sprint 4: sort, uniq, tee, tr, seq, diff, cmp, calc, whoami, hostname */
/* ------------------------------------------------------------------ */

static int read_file_or_pipe(const char *path, unsigned char *data, unsigned long cap, unsigned long *out_size)
{
	if (path == (void *)0 || path[0] == '\0')
	{
		if (pipe_stdin_buf && pipe_stdin_len > 0)
		{
			unsigned long n = pipe_stdin_len < cap ? pipe_stdin_len : cap;
			unsigned long k;
			for (k = 0; k < n; k++) data[k] = (unsigned char)pipe_stdin_buf[k];
			*out_size = n;
			return 0;
		}
		return -1;
	}
	if (fat_mode_active())
	{
		char full[128];
		if (fat_resolve_path(path, full, sizeof(full)) != 0) return -1;
		return fat32_read_file_path(full, data, cap, out_size);
	}
	return fs_read_file(path, data, cap, out_size);
}

static void cmd_sort(const char *args)
{
	unsigned char data[FS_MAX_FILE_SIZE];
	unsigned long size = 0;
	char *lines[512];
	int line_count = 0;
	int reverse = 0;
	int numeric = 0;
	const char *p = args;

	while (*p == '-')
	{
		p++;
		while (*p && *p != ' ')
		{
			if (*p == 'r') reverse = 1;
			else if (*p == 'n') numeric = 1;
			p++;
		}
		while (*p == ' ') p++;
	}
	{
		char pp[128];
		const char *rest = read_token(p, pp, sizeof(pp));
		(void)rest;
		if (read_file_or_pipe(pp, data, sizeof(data) - 1, &size) != 0)
		{
			terminal_write_line("sort: cannot read input");
			return;
		}
	}
	data[size] = '\0';

	/* Split into lines */
	{
		unsigned long i, start = 0;
		for (i = 0; i <= size; i++)
		{
			if (i == size || data[i] == '\n')
			{
				if (data[i] == '\n') data[i] = '\0';
				if (line_count < 512)
					lines[line_count++] = (char *)&data[start];
				start = i + 1;
			}
		}
	}

	/* Bubble sort */
	{
		int i2, j2, swapped;
		for (i2 = 0; i2 < line_count - 1; i2++)
		{
			swapped = 0;
			for (j2 = 0; j2 < line_count - 1 - i2; j2++)
			{
				const char *a = lines[j2], *b = lines[j2 + 1];
				int cmp = 0;
				if (numeric)
				{
					long va = 0, vb = 0;
					int neg_a = 0, neg_b = 0;
					const char *pa = a, *pb = b;
					while (*pa == ' ') pa++;
					while (*pb == ' ') pb++;
					if (*pa == '-') { neg_a = 1; pa++; }
					if (*pb == '-') { neg_b = 1; pb++; }
					while (*pa >= '0' && *pa <= '9') { va = va * 10 + (*pa - '0'); pa++; }
					while (*pb >= '0' && *pb <= '9') { vb = vb * 10 + (*pb - '0'); pb++; }
					if (neg_a) va = -va;
					if (neg_b) vb = -vb;
					if (va < vb) cmp = -1;
					else if (va > vb) cmp = 1;
				}
				else
				{
				while (*a && *b)
				{
					if (*a < *b) { cmp = -1; break; }
					if (*a > *b) { cmp = 1; break; }
					a++; b++;
				}
				if (cmp == 0)
				{
					if (*a) cmp = 1;
					else if (*b) cmp = -1;
				}
				}
				if (reverse) cmp = -cmp;
				if (cmp > 0)
				{
					char *tmp = lines[j2];
					lines[j2] = lines[j2 + 1];
					lines[j2 + 1] = tmp;
					swapped = 1;
				}
			}
			if (!swapped) break;
		}
	}

	{
		int i2;
		for (i2 = 0; i2 < line_count; i2++)
		{
			if (lines[i2][0] != '\0' || i2 < line_count - 1)
				terminal_write_line(lines[i2]);
		}
	}
}

static void cmd_uniq(const char *args)
{
	unsigned char data[FS_MAX_FILE_SIZE];
	unsigned long size = 0;
	const char *p = args;
	int flag_c = 0, flag_d = 0, flag_u = 0;

	while (*p == '-')
	{
		p++;
		while (*p && *p != ' ')
		{
			if (*p == 'c') flag_c = 1;
			else if (*p == 'd') flag_d = 1;
			else if (*p == 'u') flag_u = 1;
			p++;
		}
		while (*p == ' ') p++;
	}
	{
		char pp[128];
		const char *rest = read_token(p, pp, sizeof(pp));
		(void)rest;
		if (read_file_or_pipe(pp, data, sizeof(data) - 1, &size) != 0)
		{
			terminal_write_line("uniq: cannot read input");
			return;
		}
	}
	data[size] = '\0';

	{
		unsigned long i, start = 0;
		char prev[512];
		int count = 0;
		prev[0] = '\1'; /* sentinel — won't match any real line */
		for (i = 0; i <= size; i++)
		{
			if (i == size || data[i] == '\n')
			{
				char line[512];
				unsigned long ll = i - start, k;
				if (ll >= sizeof(line)) ll = sizeof(line) - 1;
				for (k = 0; k < ll; k++) line[k] = (char)data[start + k];
				if (ll > 0 && line[ll - 1] == '\r') ll--;
				line[ll] = '\0';

				if (string_equals(line, prev))
				{
					count++;
				}
				else
				{
					/* emit previous group */
					if (count > 0)
					{
						int emit = 1;
						if (flag_d && count == 1) emit = 0;
						if (flag_u && count > 1) emit = 0;
						if (emit)
						{
							if (flag_c)
							{
								char nb[16];
								uint_to_dec((unsigned long)count, nb, sizeof(nb));
								int pad = 7 - (int)string_length(nb);
								while (pad-- > 0) terminal_write(" ");
								terminal_write(nb);
								terminal_write(" ");
							}
							terminal_write_line(prev);
						}
					}
					count = 1;
					for (k = 0; k <= ll; k++) prev[k] = line[k];
				}
				start = i + 1;
			}
		}
		/* emit last group */
		if (count > 0)
		{
			int emit = 1;
			if (flag_d && count == 1) emit = 0;
			if (flag_u && count > 1) emit = 0;
			if (emit)
			{
				if (flag_c)
				{
					char nb[16];
					uint_to_dec((unsigned long)count, nb, sizeof(nb));
					int pad = 7 - (int)string_length(nb);
					while (pad-- > 0) terminal_write(" ");
					terminal_write(nb);
					terminal_write(" ");
				}
				terminal_write_line(prev);
			}
		}
	}
}

static void cmd_tee(const char *args)
{
	char path[128];
	const char *p = read_token(args, path, sizeof(path));
	(void)p;
	if (path[0] == '\0')
	{
		terminal_write_line("Usage: tee <file>");
		return;
	}
	if (pipe_stdin_buf && pipe_stdin_len > 0)
	{
		unsigned long k;
		/* Write to terminal */
		for (k = 0; k < pipe_stdin_len; k++)
			terminal_putc(pipe_stdin_buf[k]);
		if (pipe_stdin_len > 0 && pipe_stdin_buf[pipe_stdin_len - 1] != '\n')
			terminal_putc('\n');
		/* Write to file */
		if (fat_mode_active())
		{
			char full[128];
			if (fat_resolve_path(path, full, sizeof(full)) == 0)
				fat32_write_file_path(full, (const unsigned char *)pipe_stdin_buf, pipe_stdin_len);
			else
				terminal_write_line("tee: cannot resolve FAT path");
		}
		else
		{
			fs_write_file(path, (const unsigned char *)pipe_stdin_buf, pipe_stdin_len);
		}
	}
	else
	{
		terminal_write_line("tee: no pipe input");
	}
}

static void cmd_tr(const char *args)
{
	char set1[64], set2[64];
	const char *p = read_token(args, set1, sizeof(set1));
	if (p == (void *)0 || set1[0] == '\0')
	{
		terminal_write_line("Usage: tr <set1> <set2>");
		return;
	}
	p = read_token(p, set2, sizeof(set2));
	if (p == (void *)0 || set2[0] == '\0')
	{
		terminal_write_line("Usage: tr <set1> <set2>");
		return;
	}
	if (pipe_stdin_buf && pipe_stdin_len > 0)
	{
		unsigned long i;
		unsigned long s1len = string_length(set1);
		for (i = 0; i < pipe_stdin_len; i++)
		{
			char c = pipe_stdin_buf[i];
			unsigned long j;
			for (j = 0; j < s1len; j++)
			{
				if (c == set1[j])
				{
					c = (j < string_length(set2)) ? set2[j] : set2[string_length(set2) - 1];
					break;
				}
			}
			terminal_putc(c);
		}
		if (pipe_stdin_len > 0 && pipe_stdin_buf[pipe_stdin_len - 1] != '\n')
			terminal_putc('\n');
	}
	else
	{
		terminal_write_line("tr: no pipe input");
	}
}

static void cmd_seq(const char *args)
{
	char tok1[16], tok2[16], tok3[16];
	unsigned int start = 1, end_val, step = 1;
	const char *p;
	char num_buf[16];

	p = read_token(args, tok1, sizeof(tok1));
	if (p == (void *)0 || tok1[0] == '\0')
	{
		terminal_write_line("Usage: seq [start] <end> [step]");
		return;
	}
	p = read_token(p, tok2, sizeof(tok2));
	if (tok2[0] == '\0')
	{
		/* seq <end> */
		if (parse_dec_u32(tok1, &end_val) != 0) { terminal_write_line("seq: bad number"); return; }
	}
	else
	{
		p = read_token(p, tok3, sizeof(tok3));
		if (parse_dec_u32(tok1, &start) != 0 || parse_dec_u32(tok2, &end_val) != 0)
		{
			terminal_write_line("seq: bad number");
			return;
		}
		if (tok3[0] != '\0' && parse_dec_u32(tok3, &step) != 0)
		{
			terminal_write_line("seq: bad step");
			return;
		}
	}
	if (step == 0) step = 1;
	{
		unsigned int i;
		for (i = start; i <= end_val; i += step)
		{
			uint_to_dec((unsigned long)i, num_buf, sizeof(num_buf));
			terminal_write_line(num_buf);
		}
	}
}

static void cmd_diff(const char *args)
{
	char path1[128], path2[128];
	unsigned char data1[FS_MAX_FILE_SIZE], data2[FS_MAX_FILE_SIZE];
	unsigned long size1 = 0, size2 = 0;
	const char *p;

	p = read_token(args, path1, sizeof(path1));
	if (p == (void *)0 || path1[0] == '\0')
	{
		terminal_write_line("Usage: diff <file1> <file2>");
		return;
	}
	p = read_token(p, path2, sizeof(path2));
	if (p == (void *)0 || path2[0] == '\0')
	{
		terminal_write_line("Usage: diff <file1> <file2>");
		return;
	}

	if (read_file_or_pipe(path1, data1, sizeof(data1) - 1, &size1) != 0)
	{
		terminal_write("diff: cannot read "); terminal_write_line(path1);
		return;
	}
	if (read_file_or_pipe(path2, data2, sizeof(data2) - 1, &size2) != 0)
	{
		terminal_write("diff: cannot read "); terminal_write_line(path2);
		return;
	}
	data1[size1] = '\0';
	data2[size2] = '\0';

	/* Line-by-line comparison */
	{
		unsigned long i1 = 0, i2 = 0;
		int line_no = 1;
		int diffs = 0;
		char num_buf[16];

		while (i1 <= size1 || i2 <= size2)
		{
			char l1[512], l2[512];
			unsigned long l1len = 0, l2len = 0;

			/* Extract line from file1 */
			if (i1 <= size1)
			{
				while (i1 + l1len < size1 && data1[i1 + l1len] != '\n' && l1len < sizeof(l1) - 1)
					{ l1[l1len] = (char)data1[i1 + l1len]; l1len++; }
				if (l1len > 0 && l1[l1len - 1] == '\r') l1len--;
				l1[l1len] = '\0';
				i1 += l1len + (i1 + l1len < size1 ? 1 : 0);
				if (i1 > size1) i1 = size1 + 1;
			}
			else l1[0] = '\0';

			/* Extract line from file2 */
			if (i2 <= size2)
			{
				while (i2 + l2len < size2 && data2[i2 + l2len] != '\n' && l2len < sizeof(l2) - 1)
					{ l2[l2len] = (char)data2[i2 + l2len]; l2len++; }
				if (l2len > 0 && l2[l2len - 1] == '\r') l2len--;
				l2[l2len] = '\0';
				i2 += l2len + (i2 + l2len < size2 ? 1 : 0);
				if (i2 > size2) i2 = size2 + 1;
			}
			else l2[0] = '\0';

			if (i1 > size1 + 1 && i2 > size2 + 1) break;

			if (!string_equals(l1, l2))
			{
				uint_to_dec((unsigned long)line_no, num_buf, sizeof(num_buf));
				terminal_write(num_buf);
				terminal_write_line("c");
				terminal_write("< ");
				terminal_write_line(l1);
				terminal_write("> ");
				terminal_write_line(l2);
				diffs++;
			}
			line_no++;
			if (i1 > size1 && i2 > size2) break;
		}
		if (diffs == 0)
			terminal_write_line("Files are identical.");
	}
}

static void cmd_cmp(const char *args)
{
	char path1[128], path2[128];
	unsigned char data1[FS_MAX_FILE_SIZE], data2[FS_MAX_FILE_SIZE];
	unsigned long size1 = 0, size2 = 0;
	const char *p;

	p = read_token(args, path1, sizeof(path1));
	if (p == (void *)0 || path1[0] == '\0')
	{
		terminal_write_line("Usage: cmp <file1> <file2>");
		return;
	}
	p = read_token(p, path2, sizeof(path2));
	if (p == (void *)0 || path2[0] == '\0')
	{
		terminal_write_line("Usage: cmp <file1> <file2>");
		return;
	}

	if (read_file_or_pipe(path1, data1, sizeof(data1), &size1) != 0)
	{
		terminal_write("cmp: cannot read "); terminal_write_line(path1);
		return;
	}
	if (read_file_or_pipe(path2, data2, sizeof(data2), &size2) != 0)
	{
		terminal_write("cmp: cannot read "); terminal_write_line(path2);
		return;
	}

	{
		unsigned long i, min_size = size1 < size2 ? size1 : size2;
		char num_buf[16];
		for (i = 0; i < min_size; i++)
		{
			if (data1[i] != data2[i])
			{
				terminal_write("differ at byte ");
				uint_to_dec(i + 1, num_buf, sizeof(num_buf));
				terminal_write_line(num_buf);
				return;
			}
		}
		if (size1 != size2)
		{
			terminal_write("EOF on ");
			terminal_write_line(size1 < size2 ? path1 : path2);
		}
		else
		{
			terminal_write_line("Files are identical.");
		}
	}
}

static void cmd_calc(const char *args)
{
	long eval_arith_expr_inner(const char **);
	const char *p = args;
	long result;
	char num_buf[24];
	int neg = 0;

	if (args[0] == '\0')
	{
		terminal_write_line("Usage: calc <expression>");
		terminal_write_line("  e.g. calc 2+3*4  calc (10-3)/2");
		return;
	}

	result = eval_arith_expr_inner(&p);

	if (result < 0) { neg = 1; result = -result; }
	uint_to_dec((unsigned long)result, num_buf + 1, sizeof(num_buf) - 1);
	if (neg)
	{
		num_buf[0] = '-';
		terminal_write_line(num_buf);
	}
	else
	{
		terminal_write_line(num_buf + 1);
	}
}

static void cmd_whoami(void)
{
	terminal_write_line("root");
}

static void cmd_hostname(const char *args)
{
	if (args[0] == '\0')
	{
		terminal_write_line(hostname_buf);
		return;
	}
	{
		unsigned long i = 0;
		while (args[i] && i + 1 < sizeof(hostname_buf))
		{
			hostname_buf[i] = args[i];
			i++;
		}
		hostname_buf[i] = '\0';
	}
}

static void cmd_df(void)
{
	char num_buf[16];
	if (!fat_mode_active())
	{
		terminal_write_line("df: no FAT volume mounted");
		last_exit_code = 1;
		return;
	}
	{
		unsigned long free_bytes = 0;
		if (fat32_get_free_bytes(&free_bytes) != 0)
		{
			terminal_write_line("df: cannot read free space");
			last_exit_code = 1;
			return;
		}
		terminal_write("Free: ");
		uint_to_dec(free_bytes / 1024, num_buf, sizeof(num_buf));
		terminal_write(num_buf);
		terminal_write_line(" KB");
	}
}

static void cmd_stat(const char *args)
{
	char path[128], full_path[128];
	char num_buf[16];
	int is_dir = 0;
	unsigned long fsize = 0;
	read_token(args, path, sizeof(path));
	if (path[0] == '\0') { terminal_write_line("Usage: stat <path>"); return; }
	if (fat_mode_active())
	{
		if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0 ||
			fat32_stat_path(full_path, &is_dir, &fsize) != 0)
		{
			terminal_write("stat: cannot stat "); terminal_write_line(path);
			last_exit_code = 1;
			return;
		}
	}
	else
	{
		/* ramfs: try reading file to get size */
		unsigned char tmp[FS_MAX_FILE_SIZE];
		unsigned long sz = 0;
		if (fs_read_file(path, tmp, sizeof(tmp), &sz) != 0)
		{
			terminal_write("stat: cannot stat "); terminal_write_line(path);
			last_exit_code = 1;
			return;
		}
		fsize = sz;
		is_dir = 0;
	}
	terminal_write("  File: "); terminal_write_line(path);
	terminal_write("  Type: "); terminal_write_line(is_dir ? "directory" : "file");
	terminal_write("  Size: ");
	uint_to_dec(fsize, num_buf, sizeof(num_buf));
	terminal_write(num_buf);
	terminal_write_line(" bytes");
}

static void cmd_cut(const char *args)
{
	/* cut -d<delim> -f<field> [file] or from pipe */
	char delim = '\t';
	int field = 1;
	const char *p = args;
	unsigned char data[FS_MAX_FILE_SIZE];
	unsigned long size = 0;

	while (*p == '-')
	{
		if (p[1] == 'd' && p[2] != '\0') { delim = p[2]; p += 3; }
		else if (p[1] == 'f' && p[2] >= '0' && p[2] <= '9')
		{
			p += 2;
			field = 0;
			while (*p >= '0' && *p <= '9') field = field * 10 + (*p++ - '0');
		}
		else break;
		while (*p == ' ') p++;
	}
	if (field < 1) field = 1;

	if (read_file_or_pipe(p, data, sizeof(data) - 1, &size) != 0)
	{
		terminal_write_line("Usage: cut -d<delim> -f<field> [file]");
		return;
	}
	data[size] = '\0';

	/* Process line by line */
	{
		unsigned long i, ls = 0;
		for (i = 0; i <= size; i++)
		{
			if (i == size || data[i] == '\n')
			{
				char line[512];
				unsigned long ll = i - ls;
				unsigned long k;
				int fnum = 1;
				unsigned long fs2 = 0;
				if (ll >= sizeof(line)) ll = sizeof(line) - 1;
				for (k = 0; k < ll; k++) line[k] = (char)data[ls + k];
				if (ll > 0 && line[ll - 1] == '\r') ll--;
				line[ll] = '\0';

				/* Find the requested field */
				for (k = 0; k <= ll; k++)
				{
					if (k == ll || line[k] == delim)
					{
						if (fnum == field)
						{
							char seg[512];
							unsigned long sl = k - fs2;
							unsigned long m;
							if (sl >= sizeof(seg)) sl = sizeof(seg) - 1;
							for (m = 0; m < sl; m++) seg[m] = line[fs2 + m];
							seg[sl] = '\0';
							terminal_write_line(seg);
							break;
						}
						fnum++;
						fs2 = k + 1;
					}
				}
				ls = i + 1;
			}
		}
	}
}

static void cmd_rev(const char *args)
{
	unsigned char data[FS_MAX_FILE_SIZE];
	unsigned long size = 0;

	if (read_file_or_pipe(args, data, sizeof(data) - 1, &size) != 0)
	{
		terminal_write_line("Usage: rev [file]");
		return;
	}
	data[size] = '\0';

	{
		unsigned long i, ls = 0;
		for (i = 0; i <= size; i++)
		{
			if (i == size || data[i] == '\n')
			{
				char line[512];
				unsigned long ll = i - ls;
				unsigned long k;
				if (ll >= sizeof(line)) ll = sizeof(line) - 1;
				for (k = 0; k < ll; k++) line[k] = (char)data[ls + k];
				if (ll > 0 && line[ll - 1] == '\r') ll--;
				line[ll] = '\0';
				/* Reverse in-place */
				{
					unsigned long a = 0, b = ll > 0 ? ll - 1 : 0;
					while (a < b) { char t = line[a]; line[a] = line[b]; line[b] = t; a++; b--; }
				}
				terminal_write_line(line);
				ls = i + 1;
			}
		}
	}
}

static void cmd_printf(const char *args)
{
	/* Simple printf: supports %s, %d, \\n, \\t — no full formatting */
	const char *p = args;
	char fmt[256];
	const char *fmt_p;
	unsigned long fi = 0;

	/* Read format string */
	if (*p == '"')
	{
		p++;
		while (*p != '\0' && *p != '"' && fi + 1 < sizeof(fmt)) fmt[fi++] = *p++;
		if (*p == '"') p++;
	}
	else
	{
		while (*p != '\0' && *p != ' ' && fi + 1 < sizeof(fmt)) fmt[fi++] = *p++;
	}
	fmt[fi] = '\0';
	while (*p == ' ') p++;

	fmt_p = fmt;
	while (*fmt_p)
	{
		if (*fmt_p == '\\' && fmt_p[1] == 'n') { terminal_putc('\n'); fmt_p += 2; }
		else if (*fmt_p == '\\' && fmt_p[1] == 't') { terminal_putc('\t'); fmt_p += 2; }
		else if (*fmt_p == '\\' && fmt_p[1] == '\\') { terminal_putc('\\'); fmt_p += 2; }
		else if (*fmt_p == '%' && fmt_p[1] == 's')
		{
			char arg[128];
			unsigned long ai = 0;
			fmt_p += 2;
			/* Read next arg from remaining args */
			if (*p == '"')
			{
				p++;
				while (*p && *p != '"' && ai + 1 < sizeof(arg)) arg[ai++] = *p++;
				if (*p == '"') p++;
			}
			else
			{
				while (*p && *p != ' ' && ai + 1 < sizeof(arg)) arg[ai++] = *p++;
			}
			arg[ai] = '\0';
			while (*p == ' ') p++;
			terminal_write(arg);
		}
		else if (*fmt_p == '%' && fmt_p[1] == 'd')
		{
			char arg[32], num_buf[16];
			unsigned long ai = 0;
			long val = 0;
			int neg = 0;
			const char *vp;
			fmt_p += 2;
			while (*p && *p != ' ' && ai + 1 < sizeof(arg)) arg[ai++] = *p++;
			arg[ai] = '\0';
			while (*p == ' ') p++;
			vp = arg;
			if (*vp == '-') { neg = 1; vp++; }
			while (*vp >= '0' && *vp <= '9') val = val * 10 + (*vp++ - '0');
			if (neg) val = -val;
			if (val < 0)
			{
				terminal_putc('-');
				uint_to_dec((unsigned long)(-val), num_buf, sizeof(num_buf));
			}
			else
			{
				uint_to_dec((unsigned long)val, num_buf, sizeof(num_buf));
			}
			terminal_write(num_buf);
		}
		else
		{
			terminal_putc(*fmt_p);
			fmt_p++;
		}
	}
}

/* ================================================================== */
/* Sprint 6 commands                                                  */
/* ================================================================== */

static void cmd_true(void)  { last_exit_code = 0; }
static void cmd_false(void) { last_exit_code = 1; }

static void cmd_test(const char *args)
{
	const char *p = args;
	char tok[128];
	int result = 0;

	while (*p == ' ') p++;
	/* Strip trailing ] if invoked as [ */
	{
		char work[INPUT_BUFFER_SIZE];
		unsigned long wi = 0;
		while (p[wi] && wi + 1 < sizeof(work)) { work[wi] = p[wi]; wi++; }
		work[wi] = '\0';
		while (wi > 0 && (work[wi - 1] == ' ' || work[wi - 1] == ']')) { work[--wi] = '\0'; }
		p = work;
		while (*p == ' ') p++;

		if (*p == '!') /* negation */
		{
			p++; while (*p == ' ') p++;
			cmd_test(p);
			last_exit_code = last_exit_code ? 0 : 1;
			return;
		}

		/* Unary file tests */
		if (p[0] == '-' && p[2] == ' ')
		{
			char flag = p[1];
			const char *path = p + 3;
			while (*path == ' ') path++;
			if (flag == 'e' || flag == 'f' || flag == 'd')
			{
				int is_dir = 0;
				unsigned long fsize = 0;
				int exists = 0;
				if (fat_mode_active())
				{
					char full[128];
					if (fat_resolve_path(path, full, sizeof(full)) == 0 &&
					    fat32_stat_path(full, &is_dir, &fsize) == 0) exists = 1;
				}
				else
				{
					unsigned char tmp[1];
					unsigned long ts = 0;
					if (fs_read_file(path, tmp, 0, &ts) == 0) { exists = 1; is_dir = 0; }
					else
					{
						char n2[FS_MAX_LIST][FS_NAME_MAX + 2];
						int t2[FS_MAX_LIST]; int c2 = 0;
						if (fs_ls(path, n2, t2, 1, &c2) == 0) { exists = 1; is_dir = 1; }
					}
				}
				if (flag == 'e') result = exists;
				else if (flag == 'f') result = exists && !is_dir;
				else if (flag == 'd') result = exists && is_dir;
				last_exit_code = result ? 0 : 1;
				return;
			}
			/* String tests */
			if (flag == 'z') { last_exit_code = (path[0] == '\0') ? 0 : 1; return; }
			if (flag == 'n') { last_exit_code = (path[0] != '\0') ? 0 : 1; return; }
		}

		/* Two-operand tests */
		{
			char left[64], op[8], right[64];
			unsigned long li = 0, ri = 0, oi = 0;
			const char *q = p;
			while (*q && *q != ' ' && li + 1 < sizeof(left)) left[li++] = *q++;
			left[li] = '\0';
			while (*q == ' ') q++;
			while (*q && *q != ' ' && oi + 1 < sizeof(op)) op[oi++] = *q++;
			op[oi] = '\0';
			while (*q == ' ') q++;
			while (*q && *q != ' ' && ri + 1 < sizeof(right)) right[ri++] = *q++;
			right[ri] = '\0';

			if (string_equals(op, "=") || string_equals(op, "=="))
				result = string_equals(left, right);
			else if (string_equals(op, "!="))
				result = !string_equals(left, right);
			else if (string_equals(op, "-eq"))
			{
				long a = 0, b = 0; int an = 0, bn = 0; const char *s;
				s = left; if (*s == '-') { an = 1; s++; } while (*s >= '0' && *s <= '9') { a = a * 10 + (*s++ - '0'); } if (an) a = -a;
				s = right; if (*s == '-') { bn = 1; s++; } while (*s >= '0' && *s <= '9') { b = b * 10 + (*s++ - '0'); } if (bn) b = -b;
				result = (a == b);
			}
			else if (string_equals(op, "-ne"))
			{
				long a = 0, b = 0; int an = 0, bn = 0; const char *s;
				s = left; if (*s == '-') { an = 1; s++; } while (*s >= '0' && *s <= '9') { a = a * 10 + (*s++ - '0'); } if (an) a = -a;
				s = right; if (*s == '-') { bn = 1; s++; } while (*s >= '0' && *s <= '9') { b = b * 10 + (*s++ - '0'); } if (bn) b = -b;
				result = (a != b);
			}
			else if (string_equals(op, "-lt"))
			{
				long a = 0, b = 0; int an = 0, bn = 0; const char *s;
				s = left; if (*s == '-') { an = 1; s++; } while (*s >= '0' && *s <= '9') { a = a * 10 + (*s++ - '0'); } if (an) a = -a;
				s = right; if (*s == '-') { bn = 1; s++; } while (*s >= '0' && *s <= '9') { b = b * 10 + (*s++ - '0'); } if (bn) b = -b;
				result = (a < b);
			}
			else if (string_equals(op, "-gt"))
			{
				long a = 0, b = 0; int an = 0, bn = 0; const char *s;
				s = left; if (*s == '-') { an = 1; s++; } while (*s >= '0' && *s <= '9') { a = a * 10 + (*s++ - '0'); } if (an) a = -a;
				s = right; if (*s == '-') { bn = 1; s++; } while (*s >= '0' && *s <= '9') { b = b * 10 + (*s++ - '0'); } if (bn) b = -b;
				result = (a > b);
			}
			else if (string_equals(op, "-le"))
			{
				long a = 0, b = 0; int an = 0, bn = 0; const char *s;
				s = left; if (*s == '-') { an = 1; s++; } while (*s >= '0' && *s <= '9') { a = a * 10 + (*s++ - '0'); } if (an) a = -a;
				s = right; if (*s == '-') { bn = 1; s++; } while (*s >= '0' && *s <= '9') { b = b * 10 + (*s++ - '0'); } if (bn) b = -b;
				result = (a <= b);
			}
			else if (string_equals(op, "-ge"))
			{
				long a = 0, b = 0; int an = 0, bn = 0; const char *s;
				s = left; if (*s == '-') { an = 1; s++; } while (*s >= '0' && *s <= '9') { a = a * 10 + (*s++ - '0'); } if (an) a = -a;
				s = right; if (*s == '-') { bn = 1; s++; } while (*s >= '0' && *s <= '9') { b = b * 10 + (*s++ - '0'); } if (bn) b = -b;
				result = (a >= b);
			}
			else
			{
				/* Single token: true if non-empty */
				result = (left[0] != '\0');
			}
		}
		last_exit_code = result ? 0 : 1;
		(void)tok;
	}
}

static void cmd_which(const char *args)
{
	char name[64];
	unsigned long ni = 0;
	while (*args == ' ') args++;
	while (*args && *args != ' ' && ni + 1 < sizeof(name)) name[ni++] = *args++;
	name[ni] = '\0';
	if (name[0] == '\0') { terminal_write_line("Usage: which <command>"); return; }
	if (command_is_builtin_name(name))
	{
		terminal_write(name);
		terminal_write_line(": shell builtin");
	}
	else
	{
		int ai;
		for (ai = 0; ai < command_alias_count; ai++)
		{
			if (string_equals(command_alias_names[ai], name))
			{
				terminal_write(name);
				terminal_write(": aliased to '");
				terminal_write(command_alias_expansions[ai]);
				terminal_write_line("'");
				return;
			}
		}
		terminal_write(name);
		terminal_write_line(": not found");
		last_exit_code = 1;
	}
}

static void cmd_type(const char *args)
{
	char name[64];
	unsigned long ni = 0;
	while (*args == ' ') args++;
	while (*args && *args != ' ' && ni + 1 < sizeof(name)) name[ni++] = *args++;
	name[ni] = '\0';
	if (name[0] == '\0') { terminal_write_line("Usage: type <command>"); return; }
	{
		int ai;
		for (ai = 0; ai < command_alias_count; ai++)
		{
			if (string_equals(command_alias_names[ai], name))
			{
				terminal_write(name);
				terminal_write(" is aliased to '");
				terminal_write(command_alias_expansions[ai]);
				terminal_write_line("'");
				return;
			}
		}
	}
	if (command_is_builtin_name(name))
	{
		terminal_write(name);
		terminal_write_line(" is a shell builtin");
	}
	else
	{
		terminal_write("type: ");
		terminal_write(name);
		terminal_write_line(": not found");
		last_exit_code = 1;
	}
}

static void cmd_basename(const char *args)
{
	const char *p = args;
	const char *last_slash = (void *)0;
	const char *s;
	while (*p == ' ') p++;
	if (*p == '\0') { terminal_write_line("Usage: basename <path>"); return; }
	for (s = p; *s; s++) { if (*s == '/') last_slash = s; }
	if (last_slash != (void *)0 && last_slash[1] != '\0')
		terminal_write_line(last_slash + 1);
	else if (last_slash == (void *)0)
		terminal_write_line(p);
	else
		terminal_write_line("/");
}

static void cmd_dirname(const char *args)
{
	const char *p = args;
	int last_slash = -1;
	int si;
	char buf[128];
	while (*p == ' ') p++;
	if (*p == '\0') { terminal_write_line("Usage: dirname <path>"); return; }
	for (si = 0; p[si]; si++) { if (p[si] == '/') last_slash = si; }
	if (last_slash <= 0)
	{
		terminal_write_line(last_slash == 0 ? "/" : ".");
		return;
	}
	{
		int k;
		for (k = 0; k < last_slash && k + 1 < (int)sizeof(buf); k++) buf[k] = p[k];
		buf[k] = '\0';
		terminal_write_line(buf);
	}
}

static void cmd_yes(const char *args)
{
	const char *msg = "y";
	int count = 0, max_lines = 100;
	while (*args == ' ') args++;
	if (*args != '\0') msg = args;
	while (count < max_lines)
	{
		terminal_write_line(msg);
		count++;
	}
}

static void cmd_nl(const char *args)
{
	unsigned char data[FS_MAX_FILE_SIZE];
	unsigned long size = 0;

	if (read_file_or_pipe(args, data, sizeof(data) - 1, &size) != 0)
	{
		terminal_write_line("Usage: nl [file]");
		return;
	}
	data[size] = '\0';

	{
		unsigned long i, ls = 0;
		int line_num = 1;
		for (i = 0; i <= size; i++)
		{
			if (i == size || data[i] == '\n')
			{
				char line[512], nbuf[16], out[540];
				unsigned long ll = i - ls, k, oi = 0;
				if (ll >= sizeof(line)) ll = sizeof(line) - 1;
				for (k = 0; k < ll; k++) line[k] = (char)data[ls + k];
				if (ll > 0 && line[ll - 1] == '\r') ll--;
				line[ll] = '\0';
				uint_to_dec((unsigned long)line_num, nbuf, sizeof(nbuf));
				/* Right-justify number in 6 chars */
				{
					unsigned long nlen = string_length(nbuf);
					unsigned long pad = (nlen < 6) ? 6 - nlen : 0;
					for (k = 0; k < pad && oi + 1 < sizeof(out); k++) out[oi++] = ' ';
					for (k = 0; nbuf[k] && oi + 1 < sizeof(out); k++) out[oi++] = nbuf[k];
					if (oi + 2 < sizeof(out)) { out[oi++] = ' '; out[oi++] = ' '; }
					for (k = 0; line[k] && oi + 1 < sizeof(out); k++) out[oi++] = line[k];
					out[oi] = '\0';
				}
				if (i < size || ll > 0) /* skip trailing empty line */
				{
					terminal_write_line(out);
					line_num++;
				}
				ls = i + 1;
			}
		}
	}
}

static void cmd_factor(const char *args)
{
	unsigned long n = 0, d;
	const char *p = args;
	char nbuf[16];
	while (*p == ' ') p++;
	while (*p >= '0' && *p <= '9') { n = n * 10 + (unsigned long)(*p - '0'); p++; }
	if (n < 2) { terminal_write_line("Usage: factor <number>"); return; }
	uint_to_dec(n, nbuf, sizeof(nbuf));
	terminal_write(nbuf);
	terminal_write(":");
	d = 2;
	while (n > 1)
	{
		while (n % d == 0)
		{
			terminal_putc(' ');
			uint_to_dec(d, nbuf, sizeof(nbuf));
			terminal_write(nbuf);
			n /= d;
		}
		d++;
		if (d * d > n && n > 1) { d = n; }
	}
	terminal_putc('\n');
}

static void cmd_du(const char *args)
{
	char path[128];
	while (*args == ' ') args++;

	if (fat_mode_active())
	{
		char full[128];
		char fat_names[64][40];
		int fat_count, fi;
		unsigned long total = 0;
		const char *target = (*args != '\0') ? args : ".";

		if (fat_resolve_path(target, full, sizeof(full)) != 0)
		{
			terminal_write_line("du: cannot access path");
			return;
		}

		if (fat32_ls_path(full, fat_names, 64, &fat_count) == 0)
		{
			for (fi = 0; fi < fat_count; fi++)
			{
				char child[128];
				unsigned long ci2 = 0, k;
				int cis_dir = 0;
				unsigned long csz = 0;
				char nbuf[16];
				for (k = 0; full[k] && ci2 + 1 < sizeof(child); k++) child[ci2++] = full[k];
				if (ci2 > 0 && child[ci2 - 1] != '/') child[ci2++] = '/';
				for (k = 0; fat_names[fi][k] && ci2 + 1 < sizeof(child); k++) child[ci2++] = fat_names[fi][k];
				child[ci2] = '\0';
				if (fat32_stat_path(child, &cis_dir, &csz) == 0 && !cis_dir)
				{
					unsigned long kb = (csz + 1023) / 1024;
					if (kb == 0) kb = 1;
					uint_to_dec(kb, nbuf, sizeof(nbuf));
					terminal_write(nbuf);
					terminal_write("K  ");
					terminal_write_line(fat_names[fi]);
					total += csz;
				}
			}
		}
		{
			char nbuf[16];
			unsigned long kb = (total + 1023) / 1024;
			uint_to_dec(kb, nbuf, sizeof(nbuf));
			terminal_write(nbuf);
			terminal_write_line("K  total");
		}
	}
	else
	{
		char names[FS_MAX_LIST][FS_NAME_MAX + 2];
		int types[FS_MAX_LIST];
		int count = 0, ii;
		unsigned long total = 0;
		const char *target = (*args != '\0') ? args : (void *)0;

		if (fs_ls(target, names, types, FS_MAX_LIST, &count) != 0)
		{
			terminal_write_line("du: cannot access path");
			return;
		}
		for (ii = 0; ii < count; ii++)
		{
			if (!types[ii])
			{
				unsigned char tmp[FS_MAX_FILE_SIZE];
				unsigned long sz = 0;
				char full_name[128];
				unsigned long fi2 = 0, k;
				char nbuf[16];
				if (target)
				{
					for (k = 0; target[k] && fi2 + 1 < sizeof(full_name); k++) full_name[fi2++] = target[k];
					if (fi2 > 0 && full_name[fi2 - 1] != '/') full_name[fi2++] = '/';
				}
				for (k = 0; names[ii][k] && fi2 + 1 < sizeof(full_name); k++) full_name[fi2++] = names[ii][k];
				full_name[fi2] = '\0';
				if (fs_read_file(full_name, tmp, sizeof(tmp), &sz) == 0)
				{
					unsigned long kb = (sz + 1023) / 1024;
					if (kb == 0) kb = 1;
					uint_to_dec(kb, nbuf, sizeof(nbuf));
					terminal_write(nbuf);
					terminal_write("K  ");
					terminal_write_line(names[ii]);
					total += sz;
				}
			}
		}
		{
			char nbuf[16];
			unsigned long kb = (total + 1023) / 1024;
			uint_to_dec(kb, nbuf, sizeof(nbuf));
			terminal_write(nbuf);
			terminal_write_line("K  total");
		}
	}
	(void)path;
}

static void cmd_xargs(const char *args)
{
	char cmd_name[64];
	unsigned long ci = 0;
	while (*args == ' ') args++;
	while (*args && *args != ' ' && ci + 1 < sizeof(cmd_name)) cmd_name[ci++] = *args++;
	cmd_name[ci] = '\0';
	while (*args == ' ') args++;

	if (cmd_name[0] == '\0') { terminal_write_line("Usage: xargs <command>"); return; }

	if (pipe_stdin_buf && pipe_stdin_len > 0)
	{
		unsigned long i, ls = 0;
		for (i = 0; i <= pipe_stdin_len; i++)
		{
			if (i == pipe_stdin_len || pipe_stdin_buf[i] == '\n')
			{
				char line[INPUT_BUFFER_SIZE];
				unsigned long ll = i - ls, k, li = 0;
				if (ll > 0 && pipe_stdin_buf[ls + ll - 1] == '\r') ll--;
				if (ll == 0) { ls = i + 1; continue; }
				/* Build: cmd_name args line */
				for (k = 0; cmd_name[k] && li + 1 < sizeof(line); k++) line[li++] = cmd_name[k];
				if (*args)
				{
					if (li + 1 < sizeof(line)) line[li++] = ' ';
					for (k = 0; args[k] && li + 1 < sizeof(line); k++) line[li++] = args[k];
				}
				if (li + 1 < sizeof(line)) line[li++] = ' ';
				for (k = 0; k < ll && li + 1 < sizeof(line); k++) line[li++] = pipe_stdin_buf[ls + k];
				line[li] = '\0';
				/* Execute by copying into input_buffer */
				{
					unsigned long oi;
					for (oi = 0; oi < li && oi + 1 < INPUT_BUFFER_SIZE; oi++) input_buffer[oi] = line[oi];
					input_buffer[oi] = '\0';
					input_length = oi;
					cursor_pos = oi;
					run_command_dispatch();
				}
				ls = i + 1;
			}
		}
	}
	else
	{
		terminal_write_line("xargs: no pipe input");
	}
}

/* ================================================================== */
/* Sprint 7 commands                                                  */
/* ================================================================== */

static void cmd_less(const char *args)
{
	unsigned char data[FS_MAX_FILE_SIZE];
	unsigned long size = 0;
	char *lines[2048];
	int line_count = 0;
	int top = 0;
	int page_h;
	int running = 1;

	if (read_file_or_pipe(args, data, sizeof(data) - 1, &size) != 0)
	{
		terminal_write_line("Usage: less <file>");
		return;
	}
	data[size] = '\0';

	/* Split into lines */
	{
		unsigned long i, start = 0;
		for (i = 0; i <= size; i++)
		{
			if (i == size || data[i] == '\n')
			{
				if (data[i] == '\n') data[i] = '\0';
				if (line_count < 2048)
					lines[line_count++] = (char *)&data[start];
				start = i + 1;
			}
		}
	}

	page_h = (int)screen_get_height();
	if (page_h < 4) page_h = 25;
	page_h -= 1; /* Reserve bottom line for status */

	while (running)
	{
		int i2;
		char nbuf[16], nbuf2[16];
		screen_clear();
		for (i2 = top; i2 < top + page_h && i2 < line_count; i2++)
			terminal_write_line(lines[i2]);
		/* Status line */
		screen_set_color(0x70); /* black on grey */
		uint_to_dec((unsigned long)(top + 1), nbuf, sizeof(nbuf));
		uint_to_dec((unsigned long)line_count, nbuf2, sizeof(nbuf2));
		terminal_write(" lines ");
		terminal_write(nbuf);
		terminal_write("-");
		{
			int last = top + page_h;
			if (last > line_count) last = line_count;
			uint_to_dec((unsigned long)last, nbuf, sizeof(nbuf));
		}
		terminal_write(nbuf);
		terminal_write("/");
		terminal_write(nbuf2);
		terminal_write("  q=quit j/k=scroll space/b=page ");
		screen_set_color(terminal_text_color);

		/* Wait for keypress */
		for (;;)
		{
			while (scancode_queue_tail == scancode_queue_head)
				; /* spin waiting for input */
			{
				unsigned char sc = scancode_queue[scancode_queue_tail];
				scancode_queue_tail = (scancode_queue_tail + 1) % SCANCODE_QUEUE_SIZE;
				if (sc & 0x80) continue; /* skip key releases */
				if (sc == 0x10) { running = 0; break; } /* q */
				if (sc == 0x01) { running = 0; break; } /* Esc */
				if (sc == 0x50 || sc == 0x24) /* Down or j */
				{
					if (top + page_h < line_count) top++;
					break;
				}
				if (sc == 0x48 || sc == 0x25) /* Up or k */
				{
					if (top > 0) top--;
					break;
				}
				if (sc == 0x39) /* Space — page down */
				{
					top += page_h;
					if (top + page_h > line_count) top = line_count - page_h;
					if (top < 0) top = 0;
					break;
				}
				if (sc == 0x30) /* b — page up */
				{
					top -= page_h;
					if (top < 0) top = 0;
					break;
				}
				if (sc == 0x22) /* g — go to top */
				{
					top = 0;
					break;
				}
				if (sc == 0x22 && shift_held) /* G — go to bottom */
				{
					top = line_count - page_h;
					if (top < 0) top = 0;
					break;
				}
				if (sc == 0x47) /* Home */
				{
					top = 0;
					break;
				}
				if (sc == 0x4F) /* End */
				{
					top = line_count - page_h;
					if (top < 0) top = 0;
					break;
				}
			}
		}
	}
	screen_clear();
}

static void cmd_tac(const char *args)
{
	unsigned char data[FS_MAX_FILE_SIZE];
	unsigned long size = 0;

	if (read_file_or_pipe(args, data, sizeof(data) - 1, &size) != 0)
	{
		terminal_write_line("Usage: tac [file]");
		return;
	}
	data[size] = '\0';

	{
		char *lines[1024];
		int lc = 0;
		unsigned long i, start = 0;
		for (i = 0; i <= size; i++)
		{
			if (i == size || data[i] == '\n')
			{
				if (data[i] == '\n') data[i] = '\0';
				if (lc < 1024) lines[lc++] = (char *)&data[start];
				start = i + 1;
			}
		}
		/* Print in reverse */
		{
			int j;
			for (j = lc - 1; j >= 0; j--)
			{
				if (j == lc - 1 && lines[j][0] == '\0') continue; /* skip trailing empty */
				terminal_write_line(lines[j]);
			}
		}
	}
}

static void cmd_expr(const char *args)
{
	char value[64];
	if (args[0] == '\0') { terminal_write_line("Usage: expr <expression>"); return; }
	if (eval_arithmetic(args, value, sizeof(value)) == 0)
		terminal_write_line(value);
	else
	{
		terminal_write_line("expr: invalid expression");
		last_exit_code = 1;
	}
}

/* ---- Sprint 8 commands ---- */

static void cmd_watch(const char *args)
{
	unsigned long interval = 2; /* default 2 seconds */
	const char *cmd = args;
	char nb[16];

	/* parse -n <seconds> */
	if (cmd[0] == '-' && cmd[1] == 'n' && cmd[2] == ' ')
	{
		cmd += 3;
		while (*cmd == ' ') cmd++;
		interval = 0;
		while (*cmd >= '0' && *cmd <= '9') { interval = interval * 10 + (unsigned long)(*cmd - '0'); cmd++; }
		while (*cmd == ' ') cmd++;
	}
	if (cmd[0] == '\0') { terminal_write_line("Usage: watch [-n <sec>] <command>"); return; }
	if (interval == 0) interval = 1;
	if (interval > 3600) interval = 3600;

	for (;;)
	{
		unsigned long target;
		screen_clear();
		terminal_write("Every ");
		uint_to_dec(interval, nb, sizeof(nb));
		terminal_write(nb);
		terminal_write_line("s  (q to quit)");
		terminal_write_line("");

		/* run the command */
		{
			unsigned long ci = 0;
			while (cmd[ci] && ci + 1 < INPUT_BUFFER_SIZE) { input_buffer[ci] = cmd[ci]; ci++; }
			input_buffer[ci] = '\0';
			input_length = ci;
		}
		run_command_dispatch();

		/* wait interval seconds, polling for 'q' */
		target = timer_ticks() + interval * 100;
		while (timer_ticks() < target)
		{
			if (scancode_queue_tail != scancode_queue_head)
			{
				unsigned char sc = scancode_queue[scancode_queue_tail];
				scancode_queue_tail = (scancode_queue_tail + 1) % SCANCODE_QUEUE_SIZE;
				if (sc == 0x10 || sc == 0x01) goto watch_done; /* q or Esc */
			}
			__asm__ volatile("hlt");
		}
	}
watch_done:
	/* drain remaining keys */
	while (scancode_queue_tail != scancode_queue_head)
		scancode_queue_tail = (scancode_queue_tail + 1) % SCANCODE_QUEUE_SIZE;
	terminal_write_line("");
}

static void cmd_paste(const char *args)
{
	unsigned char data1[FS_MAX_FILE_SIZE];
	unsigned char data2[FS_MAX_FILE_SIZE];
	unsigned long size1 = 0, size2 = 0;
	char f1[128], f2[128];
	const char *p;
	const char *delim = "  "; /* default delimiter: two spaces (tab substitute) */

	p = read_token(args, f1, sizeof(f1));
	if (p == (void *)0 || f1[0] == '\0') { terminal_write_line("Usage: paste <file1> <file2> [-d <delim>]"); return; }
	p = skip_spaces(p);
	/* check for -d flag before second file */
	if (*p == '-' && *(p+1) == 'd' && *(p+2) == ' ')
	{
		p += 3;
		while (*p == ' ') p++;
		unsigned long di = 0;
		while (*p && *p != ' ' && di + 1 < sizeof(f2)) { f2[di++] = *p++; }
		f2[di] = '\0';
		delim = f2; /* reuse f2 temporarily for delim */
		p = skip_spaces(p);
		/* re-read delimiter into a stable buffer and re-read f2 */
		{
			static char delim_buf[16];
			unsigned long k = 0;
			const char *d = delim;
			while (*d && k + 1 < sizeof(delim_buf)) delim_buf[k++] = *d++;
			delim_buf[k] = '\0';
			delim = delim_buf;
		}
		p = read_token(p, f2, sizeof(f2));
	}
	else
	{
		p = read_token(p, f2, sizeof(f2));
	}
	if (f2[0] == '\0') { terminal_write_line("Usage: paste <file1> <file2> [-d <delim>]"); return; }

	if (read_file_or_pipe(f1, data1, sizeof(data1) - 1, &size1) != 0)
	{
		terminal_write("paste: cannot read "); terminal_write_line(f1); return;
	}
	if (read_file_or_pipe(f2, data2, sizeof(data2) - 1, &size2) != 0)
	{
		terminal_write("paste: cannot read "); terminal_write_line(f2); return;
	}
	data1[size1] = '\0';
	data2[size2] = '\0';

	{
		unsigned long i1 = 0, i2 = 0;
		while (i1 < size1 || i2 < size2)
		{
			/* print line from file1 */
			while (i1 < size1 && data1[i1] != '\n') { terminal_putc((char)data1[i1]); i1++; }
			if (i1 < size1) i1++; /* skip \n */
			terminal_write(delim);
			/* print line from file2 */
			while (i2 < size2 && data2[i2] != '\n') { terminal_putc((char)data2[i2]); i2++; }
			if (i2 < size2) i2++; /* skip \n */
			terminal_putc('\n');
		}
	}
}

static void cmd_column(const char *args)
{
	unsigned char data[FS_MAX_FILE_SIZE];
	unsigned long size = 0;
	char pp[128];
	const char *rest = read_token(args, pp, sizeof(pp));
	(void)rest;

	if (read_file_or_pipe(pp, data, sizeof(data) - 1, &size) != 0)
	{
		terminal_write_line("column: cannot read input");
		return;
	}
	data[size] = '\0';

	/* collect all words, find max width, print in columns */
	{
		char *words[512];
		int wcount = 0;
		unsigned long maxw = 0;
		unsigned long i = 0;
		int in_word = 0;
		unsigned long wstart = 0;

		while (i <= size)
		{
			int is_ws = (i == size || data[i] == ' ' || data[i] == '\n' || data[i] == '\r');
			if (is_ws)
			{
				if (in_word && wcount < 512)
				{
					data[i] = '\0';
					words[wcount] = (char *)&data[wstart];
					unsigned long wl = i - wstart;
					if (wl > maxw) maxw = wl;
					wcount++;
				}
				in_word = 0;
				i++;
			}
			else
			{
				if (!in_word) wstart = i;
				in_word = 1;
				i++;
			}
		}
		if (wcount == 0) return;
		if (maxw == 0) maxw = 1;
		{
			unsigned long tw = (unsigned long)screen_get_width();
			unsigned long col_w = maxw + 2;
			int cols = (int)(tw / col_w);
			int wi;
			if (cols < 1) cols = 1;
			for (wi = 0; wi < wcount; wi++)
			{
				terminal_write(words[wi]);
				if ((wi + 1) % cols == 0 || wi == wcount - 1)
					terminal_putc('\n');
				else
				{
					unsigned long wl = string_length(words[wi]);
					unsigned long pad = col_w - wl;
					while (pad-- > 0) terminal_putc(' ');
				}
			}
		}
	}
}

static void cmd_strings(const char *args)
{
	unsigned char data[FS_MAX_FILE_SIZE];
	unsigned long size = 0;
	int min_len = 4; /* default minimum printable sequence length */
	const char *p = args;

	if (*p == '-' && p[1] == 'n' && p[2] == ' ')
	{
		p += 3;
		while (*p == ' ') p++;
		min_len = 0;
		while (*p >= '0' && *p <= '9') { min_len = min_len * 10 + (*p - '0'); p++; }
		while (*p == ' ') p++;
	}
	if (min_len < 1) min_len = 1;
	{
		char pp[128];
		const char *rest = read_token(p, pp, sizeof(pp));
		(void)rest;
		if (read_file_or_pipe(pp, data, sizeof(data) - 1, &size) != 0)
		{
			terminal_write_line("strings: cannot read input");
			return;
		}
	}

	{
		unsigned long i = 0, run = 0, start = 0;
		for (i = 0; i <= size; i++)
		{
			int printable = (i < size && data[i] >= 0x20 && data[i] <= 0x7e);
			if (printable)
			{
				if (run == 0) start = i;
				run++;
			}
			else
			{
				if ((int)run >= min_len)
				{
					unsigned long k;
					for (k = start; k < start + run; k++)
						terminal_putc((char)data[k]);
					terminal_putc('\n');
				}
				run = 0;
			}
		}
	}
}

static void cmd_rmdir(const char *args)
{
	char path[128];
	char full_path[128];
	const char *rest = read_token(args, path, sizeof(path));
	(void)rest;
	if (path[0] == '\0') { terminal_write_line("Usage: rmdir <directory>"); return; }

	if (fat_mode_active())
	{
		if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0)
		{
			terminal_write_line("rmdir: path not found");
			last_exit_code = 1;
			return;
		}
		/* check if dir is empty */
		{
			char names[64][40];
			int count = 0;
			if (fat32_ls_path(full_path, names, 64, &count) != 0)
			{
				terminal_write_line("rmdir: not a directory");
				last_exit_code = 1;
				return;
			}
			if (count > 0)
			{
				terminal_write_line("rmdir: directory not empty");
				last_exit_code = 1;
				return;
			}
			if (fat32_remove_path(full_path) != 0)
			{
				terminal_write_line("rmdir: failed to remove");
				last_exit_code = 1;
			}
		}
	}
	else
	{
		char names[FS_MAX_LIST][FS_NAME_MAX + 2];
		int types[FS_MAX_LIST];
		int count = 0;
		if (fs_ls(path, names, types, FS_MAX_LIST, &count) != 0)
		{
			terminal_write_line("rmdir: not a directory");
			last_exit_code = 1;
			return;
		}
		if (count > 0)
		{
			terminal_write_line("rmdir: directory not empty");
			last_exit_code = 1;
			return;
		}
		if (fs_rm(path) != 0)
		{
			terminal_write_line("rmdir: failed to remove");
			last_exit_code = 1;
		}
	}
}

/* ================================================================== */
/* GUI / Desktop Subsystem                                            */
/* ================================================================== */

/* Arrow cursor bitmap (12x19, 1=white, 2=black outline, 0=transparent) */
static const unsigned char gui_cursor_bitmap[GUI_CURSOR_H][GUI_CURSOR_W] = {
	{2,0,0,0,0,0,0,0,0,0,0,0},
	{2,2,0,0,0,0,0,0,0,0,0,0},
	{2,1,2,0,0,0,0,0,0,0,0,0},
	{2,1,1,2,0,0,0,0,0,0,0,0},
	{2,1,1,1,2,0,0,0,0,0,0,0},
	{2,1,1,1,1,2,0,0,0,0,0,0},
	{2,1,1,1,1,1,2,0,0,0,0,0},
	{2,1,1,1,1,1,1,2,0,0,0,0},
	{2,1,1,1,1,1,1,1,2,0,0,0},
	{2,1,1,1,1,1,1,1,1,2,0,0},
	{2,1,1,1,1,1,1,1,1,1,2,0},
	{2,1,1,1,1,1,2,2,2,2,2,0},
	{2,1,1,1,2,1,1,2,0,0,0,0},
	{2,1,1,2,0,2,1,1,2,0,0,0},
	{2,1,2,0,0,2,1,1,2,0,0,0},
	{2,2,0,0,0,0,2,1,1,2,0,0},
	{2,0,0,0,0,0,2,1,1,2,0,0},
	{0,0,0,0,0,0,0,2,2,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0}
};

static void gui_save_under_cursor(int mx, int my)
{
	int dy, dx;
	unsigned int sw = screen_fb_width();
	unsigned int sh = screen_fb_height();
	for (dy = 0; dy < GUI_CURSOR_H; dy++)
	{
		for (dx = 0; dx < GUI_CURSOR_W; dx++)
		{
			int px = mx + dx, py = my + dy;
			if (px >= 0 && (unsigned int)px < sw && py >= 0 && (unsigned int)py < sh)
				gui_cursor_save[dy * GUI_CURSOR_W + dx] = screen_fb_read_pixel((unsigned int)px, (unsigned int)py);
			else
				gui_cursor_save[dy * GUI_CURSOR_W + dx] = 0;
		}
	}
	gui_cursor_saved = 1;
}

static void gui_restore_under_cursor(int mx, int my)
{
	int dy, dx;
	unsigned int sw = screen_fb_width();
	unsigned int sh = screen_fb_height();
	if (!gui_cursor_saved) return;
	for (dy = 0; dy < GUI_CURSOR_H; dy++)
	{
		for (dx = 0; dx < GUI_CURSOR_W; dx++)
		{
			int px = mx + dx, py = my + dy;
			if (px >= 0 && (unsigned int)px < sw && py >= 0 && (unsigned int)py < sh)
				screen_fb_plot_pixel((unsigned int)px, (unsigned int)py, gui_cursor_save[dy * GUI_CURSOR_W + dx]);
		}
	}
	gui_cursor_saved = 0;
}

static void gui_draw_cursor(int mx, int my)
{
	int dy, dx;
	unsigned int sw = screen_fb_width();
	unsigned int sh = screen_fb_height();
	gui_save_under_cursor(mx, my);
	for (dy = 0; dy < GUI_CURSOR_H; dy++)
	{
		for (dx = 0; dx < GUI_CURSOR_W; dx++)
		{
			unsigned char b = gui_cursor_bitmap[dy][dx];
			if (b == 0) continue;
			int px = mx + dx, py = my + dy;
			if (px >= 0 && (unsigned int)px < sw && py >= 0 && (unsigned int)py < sh)
			{
				unsigned int color = (b == 1) ? GUI_COL_CURSOR : GUI_COL_CURSOR_BRD;
				screen_fb_plot_pixel((unsigned int)px, (unsigned int)py, color);
			}
		}
	}
}

static void gui_draw_desktop(void)
{
	unsigned int sw = screen_fb_width();
	unsigned int sh = screen_fb_height();
	/* Desktop area */
	screen_fb_fill_rect(0, 0, sw, sh - GUI_TASKBAR_H, GUI_COL_DESKTOP);
	/* Taskbar */
	screen_fb_fill_rect(0, sh - GUI_TASKBAR_H, sw, GUI_TASKBAR_H, GUI_COL_TASKBAR);
	/* Taskbar top line */
	screen_fb_fill_rect(0, sh - GUI_TASKBAR_H, sw, 1, 0x444444);
}

static void gui_draw_taskbar_clock(void)
{
	unsigned char sec, min, hr, status_b;
	char tbuf[16];
	unsigned int sw = screen_fb_width();
	unsigned int sh = screen_fb_height();

	while (rtc_read(0x0A) & 0x80) {}
	sec = rtc_read(0x00);
	min = rtc_read(0x02);
	hr  = rtc_read(0x04);
	status_b = rtc_read(0x0B);
	if (!(status_b & 0x04))
	{
		sec = (unsigned char)bcd_to_bin(sec);
		min = (unsigned char)bcd_to_bin(min);
		hr  = (unsigned char)bcd_to_bin(hr & 0x7F);
	}
	if (!(status_b & 0x02) && (rtc_read(0x04) & 0x80))
		hr = (unsigned char)((hr + 12) % 24);

	/* Apply timezone offset: UTC → EST/EDT (UTC-4) */
	{
		int h = (int)hr - 4;
		if (h < 0) h += 24;
		hr = (unsigned char)h;
	}

	tbuf[0] = (char)('0' + hr / 10);
	tbuf[1] = (char)('0' + hr % 10);
	tbuf[2] = ':';
	tbuf[3] = (char)('0' + min / 10);
	tbuf[4] = (char)('0' + min % 10);
	tbuf[5] = ':';
	tbuf[6] = (char)('0' + sec / 10);
	tbuf[7] = (char)('0' + sec % 10);
	tbuf[8] = '\0';

	{
		unsigned int tx = sw - 80;
		unsigned int ty = sh - GUI_TASKBAR_H + (GUI_TASKBAR_H - screen_fb_cell_h()) / 2;
		/* clear clock area */
		screen_fb_fill_rect(tx - 4, sh - GUI_TASKBAR_H + 1, 84, GUI_TASKBAR_H - 1, GUI_COL_TASKBAR);
		screen_fb_draw_string(tx, ty, tbuf, GUI_COL_CLOCK_FG, GUI_COL_TASKBAR);
	}
}

static void gui_draw_taskbar_items(void)
{
	int i;
	unsigned int sh = screen_fb_height();
	unsigned int bx = 6;
	unsigned int by = sh - GUI_TASKBAR_H + 4;
	unsigned int bh = GUI_TASKBAR_H - 8;
	unsigned int fw = screen_fb_font_w();

	/* Start button */
	screen_fb_fill_rect(bx, by, 56, bh, GUI_COL_START_BG);
	screen_fb_draw_string(bx + 8, by + (bh - screen_fb_cell_h()) / 2, "Start", GUI_COL_START_FG, GUI_COL_START_BG);
	bx += 64;

	/* Window buttons */
	for (i = 0; i < gui_window_count; i++)
	{
		if (!gui_windows[i].visible) continue;
		{
			unsigned int bw = (unsigned int)(string_length(gui_windows[i].title) * (int)fw) + 16;
			if (bw < 60) bw = 60;
			if (bw > 160) bw = 160;
			{
				unsigned int bg = (i == gui_focused) ? 0x3A3A5E : 0x2A2A3E;
				screen_fb_fill_rect(bx, by, bw, bh, bg);
				screen_fb_draw_string(bx + 8, by + (bh - screen_fb_cell_h()) / 2, gui_windows[i].title, GUI_COL_TASK_TEXT, bg);
			}
			bx += bw + 4;
		}
	}
}

/* ── Paint pixel buffer helpers ───────────────────────────────────── */

static void gui_paint_buf_clear(void)
{
	int i;
	for (i = 0; i < GUI_PAINT_BUF_W * GUI_PAINT_BUF_H; i++)
		gui_paint_pixels[i] = 0xFFFFFF;
}

static void gui_paint_buf_set(int px, int py, unsigned int color)
{
	if (px >= 0 && px < GUI_PAINT_BUF_W && py >= 0 && py < GUI_PAINT_BUF_H)
		gui_paint_pixels[py * GUI_PAINT_BUF_W + px] = color;
}

static unsigned int gui_paint_buf_get(int px, int py)
{
	if (px >= 0 && px < GUI_PAINT_BUF_W && py >= 0 && py < GUI_PAINT_BUF_H)
		return gui_paint_pixels[py * GUI_PAINT_BUF_W + px];
	return 0xFFFFFF;
}

/* Draw a filled circle of given radius into paint buffer */
static void gui_paint_brush_circle(int bx, int by, int radius, unsigned int color)
{
	int dy, dx;
	for (dy = -radius; dy <= radius; dy++)
		for (dx = -radius; dx <= radius; dx++)
			if (dx * dx + dy * dy <= radius * radius)
				gui_paint_buf_set(bx + dx, by + dy, color);
}

/* Bresenham line into paint buffer */
static void gui_paint_buf_line(int x0, int y0, int x1, int y1, int radius, unsigned int color)
{
	int dx = x1 - x0;
	int dy = y1 - y0;
	int sx = (dx > 0) ? 1 : -1;
	int sy = (dy > 0) ? 1 : -1;
	int adx = dx * sx, ady = dy * sy;
	int err, e2;
	if (adx == 0 && ady == 0) { gui_paint_brush_circle(x0, y0, radius, color); return; }
	err = adx - ady;
	for (;;)
	{
		gui_paint_brush_circle(x0, y0, radius, color);
		if (x0 == x1 && y0 == y1) break;
		e2 = err * 2;
		if (e2 > -ady) { err -= ady; x0 += sx; }
		if (e2 < adx) { err += adx; y0 += sy; }
	}
}

/* Draw a rectangle outline into paint buffer */
static void gui_paint_buf_rect(int x0, int y0, int x1, int y1, int radius, unsigned int color)
{
	gui_paint_buf_line(x0, y0, x1, y0, radius, color);
	gui_paint_buf_line(x1, y0, x1, y1, radius, color);
	gui_paint_buf_line(x1, y1, x0, y1, radius, color);
	gui_paint_buf_line(x0, y1, x0, y0, radius, color);
}

/* Flood fill in paint buffer (iterative scanline approach) */
static void gui_paint_buf_flood(int sx, int sy, unsigned int fill_color)
{
	/* Scanline flood fill — much more efficient than per-pixel stack */
	static int fill_stack[16384]; /* pairs of x,y — scanline seeds */
	int sp = 0;
	unsigned int target;
	if (sx < 0 || sx >= GUI_PAINT_BUF_W || sy < 0 || sy >= GUI_PAINT_BUF_H) return;
	target = gui_paint_buf_get(sx, sy);
	if (target == fill_color) return;
	fill_stack[sp++] = sx; fill_stack[sp++] = sy;
	while (sp > 0 && sp < 16382)
	{
		int cy = fill_stack[--sp];
		int cx = fill_stack[--sp];
		int lx, rx, x;
		if (cy < 0 || cy >= GUI_PAINT_BUF_H) continue;
		if (cx < 0 || cx >= GUI_PAINT_BUF_W) continue;
		if (gui_paint_buf_get(cx, cy) != target) continue;
		/* Scan left */
		lx = cx;
		while (lx > 0 && gui_paint_buf_get(lx - 1, cy) == target) lx--;
		/* Scan right */
		rx = cx;
		while (rx < GUI_PAINT_BUF_W - 1 && gui_paint_buf_get(rx + 1, cy) == target) rx++;
		/* Fill the scanline */
		for (x = lx; x <= rx; x++)
			gui_paint_buf_set(x, cy, fill_color);
		/* Push seeds for row above and below */
		for (x = lx; x <= rx; x++)
		{
			if (cy > 0 && gui_paint_buf_get(x, cy - 1) == target && sp < 16382)
			{ fill_stack[sp++] = x; fill_stack[sp++] = cy - 1; }
			if (cy < GUI_PAINT_BUF_H - 1 && gui_paint_buf_get(x, cy + 1) == target && sp < 16382)
			{ fill_stack[sp++] = x; fill_stack[sp++] = cy + 1; }
		}
	}
}

static void gui_draw_window(int idx)
{
	struct gui_window *win;
	unsigned int fw, fh;
	int cx, cy, cw, ch;
	int r, c;

	if (idx < 0 || idx >= gui_window_count) return;
	win = &gui_windows[idx];
	if (!win->visible || win->minimized) return;

	fw = screen_fb_font_w();
	fh = screen_fb_cell_h();

	/* Border */
	screen_fb_fill_rect((unsigned int)win->x, (unsigned int)win->y, (unsigned int)win->w, (unsigned int)win->h, GUI_COL_BORDER);

	/* Title bar */
	{
		unsigned int tb_color = (idx == gui_focused) ? GUI_COL_TITLEBAR : GUI_COL_TITLE_INAC;
		screen_fb_fill_rect((unsigned int)(win->x + GUI_BORDER_W), (unsigned int)(win->y + GUI_BORDER_W),
			(unsigned int)(win->w - GUI_BORDER_W * 2), GUI_TITLEBAR_H, tb_color);
		screen_fb_draw_string((unsigned int)(win->x + GUI_BORDER_W + 6),
			(unsigned int)(win->y + GUI_BORDER_W + (GUI_TITLEBAR_H - fh) / 2),
			win->title, GUI_COL_TITLE_TEXT, tb_color);
	}

	/* Close button (right side of title bar) */
	{
		unsigned int cbx = (unsigned int)(win->x + win->w - GUI_BORDER_W - 20);
		unsigned int cby = (unsigned int)(win->y + GUI_BORDER_W + 2);
		screen_fb_fill_rect(cbx, cby, 18, 18, GUI_COL_CLOSE_BG);
		screen_fb_draw_char(cbx + 5, cby + (18 - fh) / 2, 'X', GUI_COL_CLOSE_FG, GUI_COL_CLOSE_BG);
	}

	/* Content area */
	cx = win->x + GUI_BORDER_W;
	cy = win->y + GUI_BORDER_W + GUI_TITLEBAR_H;
	cw = win->w - GUI_BORDER_W * 2;
	ch = win->h - GUI_BORDER_W * 2 - GUI_TITLEBAR_H;

	if (win->wtype == GUI_WTYPE_PAINT)
	{
		/* Paint: toolbar + canvas from pixel buffer + palette bar */
		int toolbar_y = cy;
		int canvas_y = cy + GUI_PAINT_TOOLBAR_H;
		int canvas_h = ch - GUI_PAINT_PALETTE_H - GUI_PAINT_TOOLBAR_H;
		int palette_y = canvas_y + canvas_h;
		int px, py;

		if (canvas_h < 1) canvas_h = 1;

		/* ── Toolbar ── */
		screen_fb_fill_rect((unsigned int)cx, (unsigned int)toolbar_y,
			(unsigned int)cw, GUI_PAINT_TOOLBAR_H, 0x444444);
		{
			int ti;
			int btn_w = 50;
			for (ti = 0; ti < GUI_PAINT_TOOL_COUNT; ti++)
			{
				int bx2 = cx + 4 + ti * (btn_w + 4);
				int by2 = toolbar_y + 2;
				unsigned int bg = (win->paint_tool == ti) ? 0x888888 : 0x555555;
				screen_fb_fill_rect((unsigned int)bx2, (unsigned int)by2,
					(unsigned int)btn_w, GUI_PAINT_TOOLBAR_H - 4, bg);
				screen_fb_draw_string((unsigned int)(bx2 + 4), (unsigned int)(by2 + 1),
					gui_paint_tool_names[ti], 0xFFFFFF, bg);
			}
			/* Show brush size indicator */
			{
				char sz_str[12];
				int bx3 = cx + 4 + GUI_PAINT_TOOL_COUNT * (btn_w + 4) + 8;
				sz_str[0] = 'S'; sz_str[1] = 'z'; sz_str[2] = ':';
				sz_str[3] = (char)('0' + win->paint_brush_size);
				sz_str[4] = '\0';
				screen_fb_draw_string((unsigned int)bx3, (unsigned int)(toolbar_y + 3),
					sz_str, 0xCCCCCC, 0x444444);
			}
		}

		/* ── Canvas from pixel buffer ── */
		{
			int draw_w = cw;
			int draw_h = canvas_h;
			if (draw_w > GUI_PAINT_BUF_W) draw_w = GUI_PAINT_BUF_W;
			if (draw_h > GUI_PAINT_BUF_H) draw_h = GUI_PAINT_BUF_H;
			for (py = 0; py < draw_h; py++)
			{
				for (px = 0; px < draw_w; px++)
				{
					screen_fb_plot_pixel((unsigned int)(cx + px), (unsigned int)(canvas_y + py),
						gui_paint_pixels[py * GUI_PAINT_BUF_W + px]);
				}
			}
			/* Fill any extra space (if window is wider/taller than buffer) */
			if (cw > GUI_PAINT_BUF_W)
				screen_fb_fill_rect((unsigned int)(cx + GUI_PAINT_BUF_W), (unsigned int)canvas_y,
					(unsigned int)(cw - GUI_PAINT_BUF_W), (unsigned int)canvas_h, 0xCCCCCC);
			if (canvas_h > GUI_PAINT_BUF_H)
				screen_fb_fill_rect((unsigned int)cx, (unsigned int)(canvas_y + GUI_PAINT_BUF_H),
					(unsigned int)cw, (unsigned int)(canvas_h - GUI_PAINT_BUF_H), 0xCCCCCC);
		}

		/* ── Palette bar ── */
		screen_fb_fill_rect((unsigned int)cx, (unsigned int)palette_y,
			(unsigned int)cw, GUI_PAINT_PALETTE_H, 0x333333);
		{
			int pi;
			int swatch_w = cw / GUI_PAINT_COLORS;
			if (swatch_w < 8) swatch_w = 8;
			for (pi = 0; pi < GUI_PAINT_COLORS; pi++)
			{
				int sx = cx + pi * swatch_w + 2;
				int sy = palette_y + 3;
				int ssz = GUI_PAINT_PALETTE_H - 6;
				screen_fb_fill_rect((unsigned int)sx, (unsigned int)sy,
					(unsigned int)swatch_w - 4, (unsigned int)ssz, gui_paint_palette[pi]);
				/* Highlight selected color */
				if (gui_paint_palette[pi] == win->paint_color)
				{
					screen_fb_fill_rect((unsigned int)sx, (unsigned int)(sy - 2),
						(unsigned int)swatch_w - 4, 2, 0xFFFF00);
				}
			}
		}
	}
	else if (win->wtype == GUI_WTYPE_EDITOR)
	{
		/* Editor: blue background text area with scrolling */
		int vis_rows = ch / (int)fh - 1; /* -1 for status bar */
		if (vis_rows < 1) vis_rows = 1;
		if (vis_rows > win->text_rows) vis_rows = win->text_rows;

		screen_fb_fill_rect((unsigned int)cx, (unsigned int)cy, (unsigned int)cw, (unsigned int)ch, 0x000080);

		/* Auto-scroll: ensure cursor row is visible */
		if (win->text_row < win->scroll_offset) win->scroll_offset = win->text_row;
		if (win->text_row >= win->scroll_offset + vis_rows) win->scroll_offset = win->text_row - vis_rows + 1;
		if (win->scroll_offset < 0) win->scroll_offset = 0;

		/* Render visible text content with scroll offset */
		for (r = 0; r < vis_rows; r++)
		{
			int src_row = r + win->scroll_offset;
			if (src_row >= win->text_rows) break;
			for (c = 0; c < win->text_cols; c++)
			{
				int offset = src_row * win->text_cols + c;
				char ch_char = win->text[offset];
				int px = cx + c * (int)fw;
				if (px + (int)fw > cx + cw) break; /* clip to content width */
				if (ch_char == '\0') ch_char = ' ';
				screen_fb_draw_char((unsigned int)px, (unsigned int)(cy + r * (int)fh),
					ch_char, 0xFFFFFF, 0x000080);
			}
		}

		/* Text cursor (relative to scroll offset) */
		if (idx == gui_focused && win->text_row >= win->scroll_offset &&
		    win->text_row < win->scroll_offset + vis_rows &&
		    win->text_col < win->text_cols)
		{
			unsigned int cur_x = (unsigned int)(cx + win->text_col * (int)fw);
			unsigned int cur_y = (unsigned int)(cy + (win->text_row - win->scroll_offset) * (int)fh);
			screen_fb_fill_rect(cur_x, cur_y, fw, fh, 0xFFFFFF);
			{
				int offset = win->text_row * win->text_cols + win->text_col;
				char ch_char = win->text[offset];
				if (ch_char == '\0' || ch_char == ' ') ch_char = ' ';
				screen_fb_draw_char(cur_x, cur_y, ch_char, 0x000080, 0xFFFFFF);
			}
		}

		/* Status bar at bottom: filepath + row/col + modified + Ctrl+S hint */
		{
			char status[80];
			int si = 0;
			/* Show filepath if any */
			if (win->filepath[0])
			{
				int pi = 0;
				while (win->filepath[pi] && si < 30) status[si++] = win->filepath[pi++];
				status[si++] = ' ';
			}
			status[si++] = 'L';
			{
				char numbuf[8];
				int ni = 0, val = win->text_row + 1;
				if (val == 0) numbuf[ni++] = '0';
				else { char tmp[8]; int ti = 0; while (val > 0) { tmp[ti++] = '0' + val % 10; val /= 10; } while (ti > 0) numbuf[ni++] = tmp[--ti]; }
				numbuf[ni] = '\0';
				{ int k; for (k = 0; numbuf[k]; k++) status[si++] = numbuf[k]; }
			}
			status[si++] = ' ';
			status[si++] = 'C';
			{
				char numbuf[8];
				int ni = 0, val = win->text_col + 1;
				if (val == 0) numbuf[ni++] = '0';
				else { char tmp[8]; int ti = 0; while (val > 0) { tmp[ti++] = '0' + val % 10; val /= 10; } while (ti > 0) numbuf[ni++] = tmp[--ti]; }
				numbuf[ni] = '\0';
				{ int k; for (k = 0; numbuf[k]; k++) status[si++] = numbuf[k]; }
			}
			if (win->editor_modified) { status[si++] = ' '; status[si++] = '*'; }
			{ const char *h = " ^S=Save"; int hi = 0; while (h[hi] && si < 78) status[si++] = h[hi++]; }
			status[si] = '\0';
			screen_fb_fill_rect((unsigned int)cx, (unsigned int)(cy + ch - (int)fh),
				(unsigned int)cw, fh, 0x008080);
			screen_fb_draw_string((unsigned int)cx, (unsigned int)(cy + ch - (int)fh),
				status, 0xFFFFFF, 0x008080);
		}
	}
	else if (win->wtype == GUI_WTYPE_EXPLORER)
	{
		/* File explorer: path bar + scrollable file list */
		int bar_h = (int)fh + 4; /* path/status bar at top */
		int list_y = cy + bar_h;
		int list_h = ch - bar_h;
		int visible_rows = list_h / GUI_EXPLORER_ITEM_H;
		int ei;

		/* Background */
		screen_fb_fill_rect((unsigned int)cx, (unsigned int)cy, (unsigned int)cw, (unsigned int)ch, 0xFFFFFF);

		/* Path bar */
		screen_fb_fill_rect((unsigned int)cx, (unsigned int)cy, (unsigned int)cw, (unsigned int)bar_h, 0xE0E0E0);
		screen_fb_draw_string((unsigned int)(cx + 4), (unsigned int)(cy + 2),
			win->filepath, 0x000000, 0xE0E0E0);

		/* ".." entry at top if not root */
		{
			int row_idx = 0;
			int start = win->explorer_scroll;
			int has_parent = (win->filepath[0] != '\0' &&
			                  !(win->filepath[0] == '/' && win->filepath[1] == '\0'));

			if (has_parent && start == 0)
			{
				int ey = list_y + row_idx * GUI_EXPLORER_ITEM_H;
				unsigned int bg = (win->explorer_selected == -1) ? 0x3399FF : 0xFFFFFF;
				unsigned int fg = (win->explorer_selected == -1) ? 0xFFFFFF : 0x000000;
				if (ey + GUI_EXPLORER_ITEM_H <= cy + ch)
				{
					screen_fb_fill_rect((unsigned int)cx, (unsigned int)ey, (unsigned int)cw, GUI_EXPLORER_ITEM_H, bg);
					screen_fb_draw_string((unsigned int)(cx + 20), (unsigned int)(ey + 1),
						"[..] Up", fg, bg);
				}
				row_idx++;
			}

			/* File entries */
			for (ei = start; ei < win->explorer_count && row_idx < visible_rows; ei++)
			{
				int ey = list_y + row_idx * GUI_EXPLORER_ITEM_H;
				unsigned int bg = (ei == win->explorer_selected) ? 0x3399FF : 0xFFFFFF;
				unsigned int fg = (ei == win->explorer_selected) ? 0xFFFFFF : 0x000000;
				if (ey + GUI_EXPLORER_ITEM_H > cy + ch) break;

				screen_fb_fill_rect((unsigned int)cx, (unsigned int)ey, (unsigned int)cw, GUI_EXPLORER_ITEM_H, bg);

				/* Icon prefix */
				if (win->explorer_types[ei])
				{
					/* Directory: folder icon */
					screen_fb_draw_string((unsigned int)(cx + 4), (unsigned int)(ey + 1),
						"[D]", (ei == win->explorer_selected) ? 0xFFFF00 : 0xCC9900, bg);
				}
				else
				{
					/* File icon */
					screen_fb_draw_string((unsigned int)(cx + 4), (unsigned int)(ey + 1),
						" - ", (ei == win->explorer_selected) ? 0xCCCCCC : 0x666666, bg);
				}

				/* Name */
				screen_fb_draw_string((unsigned int)(cx + 30), (unsigned int)(ey + 1),
					win->explorer_names[ei], fg, bg);

				row_idx++;
			}

			/* Scrollbar track if content overflows */
			{
				int total_items = win->explorer_count + (has_parent ? 1 : 0);
				if (total_items > visible_rows && list_h > 20)
				{
					int sb_x = cx + cw - 8;
					int sb_h = list_h;
					int thumb_h = (visible_rows * sb_h) / total_items;
					int thumb_y;
					if (thumb_h < 16) thumb_h = 16;
					if (thumb_h > sb_h) thumb_h = sb_h;
					thumb_y = list_y + (start * (sb_h - thumb_h)) / (total_items - visible_rows);
					/* Track */
					screen_fb_fill_rect((unsigned int)sb_x, (unsigned int)list_y, 8, (unsigned int)sb_h, 0xDDDDDD);
					/* Thumb */
					screen_fb_fill_rect((unsigned int)sb_x, (unsigned int)thumb_y, 8, (unsigned int)thumb_h, 0x888888);
				}
			}
		}
	}
	else if (win->wtype == GUI_WTYPE_HEXEDIT)
	{
		/* Hex editor: offset | hex bytes | ASCII */
		unsigned long bytes_per_row = 16;
		int vis_rows = ch / (int)fh - 1; /* -1 for status bar */
		int hr;
		if (vis_rows < 1) vis_rows = 1;

		screen_fb_fill_rect((unsigned int)cx, (unsigned int)cy, (unsigned int)cw, (unsigned int)ch, 0x1E1E1E);

		/* Auto-scroll: keep cursor visible */
		{
			unsigned long cursor_row = win->hex_cursor / bytes_per_row;
			if (cursor_row < win->hex_offset / bytes_per_row)
				win->hex_offset = cursor_row * bytes_per_row;
			if (cursor_row >= win->hex_offset / bytes_per_row + (unsigned long)vis_rows)
				win->hex_offset = (cursor_row - (unsigned long)vis_rows + 1) * bytes_per_row;
		}

		for (hr = 0; hr < vis_rows; hr++)
		{
			unsigned long row_off = win->hex_offset + (unsigned long)hr * bytes_per_row;
			int hc;
			int py2 = cy + hr * (int)fh;
			char offbuf[10];

			if (row_off >= win->hex_size && row_off > 0) break;

			/* Offset column (8 hex digits) */
			{
				int di;
				for (di = 7; di >= 0; di--)
				{
					int nib = (int)((row_off >> (di * 4)) & 0xF);
					offbuf[7 - di] = (nib < 10) ? ('0' + nib) : ('A' + nib - 10);
				}
				offbuf[8] = '\0';
			}
			screen_fb_draw_string((unsigned int)cx, (unsigned int)py2, offbuf, 0x888888, 0x1E1E1E);

			/* Hex bytes */
			for (hc = 0; hc < (int)bytes_per_row; hc++)
			{
				unsigned long bi = row_off + (unsigned long)hc;
				int hx = cx + (10 + hc * 3) * (int)fw;
				if (hc >= 8) hx += (int)fw; /* gap between groups */
				if (bi < win->hex_size)
				{
					unsigned char b = win->hex_data[bi];
					char hex[3];
					hex[0] = ((b >> 4) < 10) ? ('0' + (b >> 4)) : ('A' + (b >> 4) - 10);
					hex[1] = ((b & 0xF) < 10) ? ('0' + (b & 0xF)) : ('A' + (b & 0xF) - 10);
					hex[2] = '\0';
					if (bi == win->hex_cursor && idx == gui_focused)
						screen_fb_draw_string((unsigned int)hx, (unsigned int)py2, hex, 0x1E1E1E, 0x00FF00);
					else
						screen_fb_draw_string((unsigned int)hx, (unsigned int)py2, hex, 0x00FF00, 0x1E1E1E);
				}
			}

			/* ASCII column */
			for (hc = 0; hc < (int)bytes_per_row; hc++)
			{
				unsigned long bi = row_off + (unsigned long)hc;
				int ax = cx + (10 + (int)bytes_per_row * 3 + 2 + hc) * (int)fw;
				if (bi < win->hex_size)
				{
					unsigned char b = win->hex_data[bi];
					char ac = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
					if (bi == win->hex_cursor && idx == gui_focused)
						screen_fb_draw_char((unsigned int)ax, (unsigned int)py2, ac, 0x1E1E1E, 0xFFFF00);
					else
						screen_fb_draw_char((unsigned int)ax, (unsigned int)py2, ac, 0xCCCCCC, 0x1E1E1E);
				}
			}
		}

		/* Status bar */
		{
			char status[60];
			int si = 0;
			const char *h = "Offset: 0x";
			while (*h) status[si++] = *h++;
			{
				int di;
				for (di = 7; di >= 0; di--)
				{
					int nib = (int)((win->hex_cursor >> (di * 4)) & 0xF);
					status[si++] = (nib < 10) ? ('0' + nib) : ('A' + nib - 10);
				}
			}
			{ const char *s2 = " ^S=Save"; while (*s2 && si < 58) status[si++] = *s2++; }
			if (win->editor_modified) { status[si++] = ' '; status[si++] = '*'; }
			status[si] = '\0';
			screen_fb_fill_rect((unsigned int)cx, (unsigned int)(cy + ch - (int)fh),
				(unsigned int)cw, fh, 0x333333);
			screen_fb_draw_string((unsigned int)cx, (unsigned int)(cy + ch - (int)fh),
				status, 0x00FF00, 0x333333);
		}
	}
	else
	{
		/* Terminal window — original rendering */
		screen_fb_fill_rect((unsigned int)cx, (unsigned int)cy, (unsigned int)cw, (unsigned int)ch, GUI_COL_WINBG);

		/* Render terminal text */
		for (r = 0; r < win->text_rows; r++)
		{
			for (c = 0; c < win->text_cols; c++)
			{
				int offset = r * win->text_cols + c;
				char ch_char = win->text[offset];
				unsigned char attr = win->attrs[offset];
				unsigned int fg, bg;
				if (ch_char == '\0') ch_char = ' ';
				fg = 0xAAAAAAU;
				bg = GUI_COL_WINBG;
				{
					static const unsigned int pal[16] = {
						0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
						0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
						0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
						0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
					};
					fg = pal[attr & 0x0F];
					bg = pal[(attr >> 4) & 0x0F];
					if (bg == 0x000000) bg = GUI_COL_WINBG;
				}
				screen_fb_draw_char((unsigned int)(cx + c * (int)fw), (unsigned int)(cy + r * (int)fh), ch_char, fg, bg);
			}
		}

		/* Text cursor */
		if (idx == gui_focused && win->text_row < win->text_rows && win->text_col < win->text_cols)
		{
			unsigned int cur_x = (unsigned int)(cx + win->text_col * (int)fw);
			unsigned int cur_y = (unsigned int)(cy + win->text_row * (int)fh + (int)fh - 2);
			screen_fb_fill_rect(cur_x, cur_y, fw, 2, 0xCCCCCC);
		}
	}

	/* Resize grip (bottom-right corner triangle) */
	{
		int gx = win->x + win->w - GUI_RESIZE_HANDLE;
		int gy = win->y + win->h - GUI_RESIZE_HANDLE;
		int gi;
		for (gi = 0; gi < GUI_RESIZE_HANDLE; gi++)
		{
			int line_start = GUI_RESIZE_HANDLE - gi;
			screen_fb_fill_rect((unsigned int)(gx + line_start), (unsigned int)(gy + gi),
				(unsigned int)gi, 1, 0x888888);
		}
	}
}

static void gui_redraw_all(void)
{
	int i;
	gui_draw_desktop();
	gui_draw_taskbar_items();
	gui_draw_taskbar_clock();
	for (i = 0; i < gui_window_count; i++)
		gui_draw_window(i);
	if (gui_start_menu_open)
		gui_draw_start_menu();
	gui_draw_cursor(gui_mouse_x, gui_mouse_y);
}

/* Create a terminal window centered on screen */
static int gui_create_terminal_window(void)
{
	struct gui_window *win;
	unsigned int sw, sh, fw, fh;
	int ww, wh, i;

	if (gui_window_count >= GUI_MAX_WINDOWS) return -1;

	fw = screen_fb_font_w();
	fh = screen_fb_cell_h();
	sw = screen_fb_width();
	sh = screen_fb_height();

	win = &gui_windows[gui_window_count];
	for (i = 0; i < GUI_WIN_TEXT_COLS * GUI_WIN_TEXT_ROWS; i++)
	{
		win->text[i] = ' ';
		win->attrs[i] = 0x0F;
	}
	win->text_cols = GUI_WIN_TEXT_COLS;
	win->text_rows = GUI_WIN_TEXT_ROWS;
	/* Compute pixel size from text grid */
	ww = (int)(win->text_cols * (int)fw + GUI_BORDER_W * 2);
	wh = (int)(win->text_rows * (int)fh + GUI_BORDER_W * 2 + GUI_TITLEBAR_H);
	win->w = ww;
	win->h = wh;
	win->x = ((int)sw - ww) / 2;
	win->y = ((int)(sh - GUI_TASKBAR_H) - wh) / 2;
	if (win->y < 0) win->y = 0;
	win->visible = 1;
	win->minimized = 0;
	win->wtype = GUI_WTYPE_TERMINAL;
	win->text_col = 0;
	win->text_row = 0;
	win->scroll_offset = 0;

	{
		const char *t = "Terminal";
		int ti = 0;
		while (t[ti] && ti < 31) { win->title[ti] = t[ti]; ti++; }
		win->title[ti] = '\0';
	}

	gui_focused = gui_window_count;
	gui_window_count++;
	return gui_focused;
}

/* ── Custom theme saving/loading ──────────────────────────────────── */
/* Theme file format: one theme per line, 20 hex fields separated by spaces.
   First field is the name (up to 19 chars, underscores for spaces). */

static void gui_save_custom_themes(void)
{
	static char tbuf[1024];
	int len = 0;
	int i;
	for (i = GUI_BUILTIN_THEMES; i < GUI_THEME_COUNT; i++)
	{
		struct gui_theme *t = &gui_themes[i];
		int j;
		const char *n;
		if (!t->name) continue;
		/* Write name */
		n = t->name;
		while (*n && len < 980) { tbuf[len++] = (*n == ' ') ? '_' : *n; n++; }
		tbuf[len++] = ' ';
		/* Write 19 color values as hex */
		{
			unsigned int vals[19];
			vals[0] = t->desktop; vals[1] = t->taskbar; vals[2] = t->task_text;
			vals[3] = t->titlebar; vals[4] = t->title_inac; vals[5] = t->title_text;
			vals[6] = t->border; vals[7] = t->winbg; vals[8] = t->close_bg;
			vals[9] = t->close_fg; vals[10] = t->cursor; vals[11] = t->cursor_brd;
			vals[12] = t->start_bg; vals[13] = t->start_fg; vals[14] = t->clock_fg;
			vals[15] = t->menu_bg; vals[16] = t->menu_hl; vals[17] = t->menu_text;
			vals[18] = t->menu_sep;
			for (j = 0; j < 19 && len < 990; j++)
			{
				unsigned int v = vals[j];
				int ni;
				for (ni = 5; ni >= 0; ni--)
				{
					int d = (v >> (ni * 4)) & 0xF;
					tbuf[len++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
				}
				tbuf[len++] = (j < 18) ? ' ' : '\n';
			}
		}
	}
	if (len > 0)
	{
		if (fat_mode_active())
		{
			char resolved[128];
			if (fat_resolve_path("/desktop/themes.cfg", resolved, sizeof(resolved)) == 0)
				fat32_write_file_path(resolved, (const unsigned char *)tbuf, len);
		}
		else
			fs_write_file("/desktop/themes.cfg", (const unsigned char *)tbuf, len);
	}
}

static unsigned int gui_parse_hex6(const char *s)
{
	unsigned int v = 0;
	int i;
	for (i = 0; i < 6; i++)
	{
		char c = s[i];
		int d = 0;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		v = (v << 4) | d;
	}
	return v;
}

static void gui_load_custom_themes(void)
{
	static unsigned char fbuf[1024];
	unsigned long fsize = 0;
	int ok = 0;
	if (fat_mode_active())
	{
		char resolved[128];
		if (fat_resolve_path("/desktop/themes.cfg", resolved, sizeof(resolved)) == 0)
			if (fat32_read_file_path(resolved, fbuf, sizeof(fbuf) - 1, &fsize) == 0) ok = 1;
	}
	else
	{
		if (fs_read_file("/desktop/themes.cfg", fbuf, sizeof(fbuf) - 1, &fsize) == 0) ok = 1;
	}
	if (!ok || fsize == 0) return;
	fbuf[fsize] = 0;
	/* Parse lines */
	{
		int pos = 0;
		int slot = GUI_BUILTIN_THEMES;
		while (pos < (int)fsize && slot < GUI_THEME_COUNT)
		{
			static char name_buf[4][20]; /* static names for custom theme slots */
			char *nb = name_buf[slot - GUI_BUILTIN_THEMES];
			int ni = 0;
			unsigned int vals[19];
			int vi = 0;
			/* Read name */
			while (pos < (int)fsize && fbuf[pos] != ' ' && fbuf[pos] != '\n' && ni < 19)
			{
				nb[ni++] = (fbuf[pos] == '_') ? ' ' : (char)fbuf[pos];
				pos++;
			}
			nb[ni] = '\0';
			if (pos < (int)fsize && fbuf[pos] == ' ') pos++;
			/* Read 19 hex values */
			while (vi < 19 && pos + 5 < (int)fsize)
			{
				vals[vi++] = gui_parse_hex6((const char *)&fbuf[pos]);
				pos += 6;
				if (pos < (int)fsize && (fbuf[pos] == ' ' || fbuf[pos] == '\n')) pos++;
			}
			/* Skip to next line */
			while (pos < (int)fsize && fbuf[pos] != '\n') pos++;
			if (pos < (int)fsize) pos++;
			if (vi == 19 && ni > 0)
			{
				struct gui_theme *t = &gui_themes[slot];
				t->name = nb;
				t->desktop = vals[0]; t->taskbar = vals[1]; t->task_text = vals[2];
				t->titlebar = vals[3]; t->title_inac = vals[4]; t->title_text = vals[5];
				t->border = vals[6]; t->winbg = vals[7]; t->close_bg = vals[8];
				t->close_fg = vals[9]; t->cursor = vals[10]; t->cursor_brd = vals[11];
				t->start_bg = vals[12]; t->start_fg = vals[13]; t->clock_fg = vals[14];
				t->menu_bg = vals[15]; t->menu_hl = vals[16]; t->menu_text = vals[17];
				t->menu_sep = vals[18];
				slot++;
			}
		}
	}
}

/* Create a Paint window */
static int gui_create_paint_window(void)
{
	struct gui_window *win;
	unsigned int sw, sh;
	int ww, wh;

	if (gui_window_count >= GUI_MAX_WINDOWS) return -1;

	sw = screen_fb_width();
	sh = screen_fb_height();

	win = &gui_windows[gui_window_count];

	/* 500x350 canvas area + toolbar + palette bar */
	ww = 504; /* 500 + 2*border */
	wh = 350 + GUI_TITLEBAR_H + GUI_PAINT_TOOLBAR_H + GUI_PAINT_PALETTE_H + GUI_BORDER_W * 2;
	win->w = ww;
	win->h = wh;
	win->x = ((int)sw - ww) / 2 + 40;
	win->y = ((int)(sh - GUI_TASKBAR_H) - wh) / 2;
	if (win->y < 0) win->y = 0;
	win->visible = 1;
	win->minimized = 0;
	win->wtype = GUI_WTYPE_PAINT;
	win->text_col = 0;
	win->text_row = 0;
	win->text_cols = 500;
	win->text_rows = 350;
	win->scroll_offset = 0;
	win->paint_color = 0x000000;
	win->paint_drawing = 0;
	win->paint_tool = GUI_PAINT_TOOL_BRUSH;
	win->paint_brush_size = 2;
	win->paint_line_x0 = 0;
	win->paint_line_y0 = 0;

	/* Clear the shared pixel buffer */
	gui_paint_buf_clear();
	gui_paint_buf_owner = gui_window_count;

	{
		const char *t = "Paint";
		int ti = 0;
		while (t[ti] && ti < 31) { win->title[ti] = t[ti]; ti++; }
		win->title[ti] = '\0';
	}

	gui_focused = gui_window_count;
	gui_window_count++;
	return gui_focused;
}

/* Create a Text Editor window */
static int gui_create_editor_window(void)
{
	struct gui_window *win;
	unsigned int sw, sh, fw, fh;
	int ww, wh, i;

	if (gui_window_count >= GUI_MAX_WINDOWS) return -1;

	fw = screen_fb_font_w();
	fh = screen_fb_cell_h();
	sw = screen_fb_width();
	sh = screen_fb_height();

	win = &gui_windows[gui_window_count];
	for (i = 0; i < GUI_WIN_TEXT_COLS * GUI_WIN_TEXT_ROWS; i++)
	{
		win->text[i] = ' ';
		win->attrs[i] = 0x1F; /* white on blue (classic editor look) */
	}
	win->text_cols = GUI_WIN_TEXT_COLS;
	win->text_rows = GUI_WIN_TEXT_ROWS;
	ww = (int)(win->text_cols * (int)fw + GUI_BORDER_W * 2);
	wh = (int)(win->text_rows * (int)fh + GUI_BORDER_W * 2 + GUI_TITLEBAR_H);
	win->w = ww;
	win->h = wh;
	win->x = ((int)sw - ww) / 2 - 40;
	win->y = ((int)(sh - GUI_TASKBAR_H) - wh) / 2 + 20;
	if (win->y < 0) win->y = 0;
	win->visible = 1;
	win->minimized = 0;
	win->wtype = GUI_WTYPE_EDITOR;
	win->text_col = 0;
	win->text_row = 0;
	win->scroll_offset = 0;
	win->editor_modified = 0;

	{
		const char *t = "Editor";
		int ti = 0;
		while (t[ti] && ti < 31) { win->title[ti] = t[ti]; ti++; }
		win->title[ti] = '\0';
	}

	gui_focused = gui_window_count;
	gui_window_count++;
	return gui_focused;
}

/* ── Hex Editor ──────────────────────────────────────────────────── */

static int gui_create_hexedit_window(void)
{
	struct gui_window *win;
	unsigned int sw, sh, fw, fh;
	int ww, wh, i;

	if (gui_window_count >= GUI_MAX_WINDOWS) return -1;

	fw = screen_fb_font_w();
	fh = screen_fb_cell_h();
	sw = screen_fb_width();
	sh = screen_fb_height();

	win = &gui_windows[gui_window_count];
	for (i = 0; i < GUI_WIN_TEXT_COLS * GUI_WIN_TEXT_ROWS; i++)
	{
		win->text[i] = ' ';
		win->attrs[i] = 0x0F;
	}
	win->text_cols = GUI_WIN_TEXT_COLS;
	win->text_rows = GUI_WIN_TEXT_ROWS;
	/* Wider window for hex: offset(9) + hex(48) + gap(2) + ascii(16) + margins ~= 76 chars */
	ww = (int)(76 * (int)fw + GUI_BORDER_W * 2);
	wh = (int)(win->text_rows * (int)fh + GUI_BORDER_W * 2 + GUI_TITLEBAR_H);
	win->w = ww;
	win->h = wh;
	win->x = ((int)sw - ww) / 2 + 20;
	win->y = ((int)(sh - GUI_TASKBAR_H) - wh) / 2 - 10;
	if (win->y < 0) win->y = 0;
	win->visible = 1;
	win->minimized = 0;
	win->wtype = GUI_WTYPE_HEXEDIT;
	win->text_col = 0;
	win->text_row = 0;
	win->scroll_offset = 0;
	win->editor_modified = 0;
	win->filepath[0] = '\0';
	win->hex_size = 0;
	win->hex_offset = 0;
	win->hex_cursor = 0;
	win->hex_nibble = 0;
	for (i = 0; i < GUI_WIN_TEXT_COLS * GUI_WIN_TEXT_ROWS; i++)
		win->hex_data[i] = 0;

	{
		const char *t = "Hex Editor";
		int ti = 0;
		while (t[ti] && ti < 31) { win->title[ti] = t[ti]; ti++; }
		win->title[ti] = '\0';
	}

	gui_focused = gui_window_count;
	gui_window_count++;
	return gui_focused;
}

static void gui_hexedit_open_file(int widx, const char *path)
{
	struct gui_window *win;
	unsigned long size = 0;
	int ok = 0;
	int pi;

	if (widx < 0 || widx >= gui_window_count) return;
	win = &gui_windows[widx];

	/* Read the file into hex_data */
	if (fat_mode_active())
	{
		char resolved[128];
		if (fat_resolve_path(path, resolved, sizeof(resolved)) == 0)
		{
			if (fat32_read_file_path(resolved, win->hex_data,
				sizeof(win->hex_data), &size) == 0)
				ok = 1;
		}
	}
	else
	{
		if (fs_read_file(path, win->hex_data, sizeof(win->hex_data), &size) == 0)
			ok = 1;
	}

	if (!ok) return;
	win->hex_size = size;
	win->hex_offset = 0;
	win->hex_cursor = 0;
	win->hex_nibble = 0;
	win->editor_modified = 0;

	/* Store filepath */
	pi = 0;
	while (path[pi] && pi + 1 < 128) { win->filepath[pi] = path[pi]; pi++; }
	win->filepath[pi] = '\0';

	/* Update title */
	{
		int ti = 0;
		const char *pre = "Hex: ";
		while (pre[ti] && ti < 24) { win->title[ti] = pre[ti]; ti++; }
		{
			int last_slash = -1;
			pi = 0;
			while (path[pi]) { if (path[pi] == '/') last_slash = pi; pi++; }
			pi = (last_slash >= 0) ? last_slash + 1 : 0;
			while (path[pi] && ti < 31) { win->title[ti++] = path[pi++]; }
		}
		win->title[ti] = '\0';
	}
}

/* ── File Explorer ───────────────────────────────────────────────── */

/* Refresh the directory listing of an explorer window */
static void gui_explorer_refresh(struct gui_window *win)
{
	int count = 0;
	int i;

	win->explorer_count = 0;
	win->explorer_selected = 0;
	win->explorer_scroll = 0;

	if (fat_mode_active())
	{
		char fat_names[64][40];
		int fat_count = 0;
		if (fat32_ls_path(win->filepath, fat_names, 64, &fat_count) == 0)
		{
			for (i = 0; i < fat_count && count < GUI_EXPLORER_MAX; i++)
			{
				int is_dir = 0;
				unsigned long sz = 0;
				char child[128];
				unsigned long ci = 0, k;
				/* Build child path */
				for (k = 0; win->filepath[k] && ci + 1 < sizeof(child); k++) child[ci++] = win->filepath[k];
				if (ci > 0 && child[ci - 1] != '/') child[ci++] = '/';
				for (k = 0; fat_names[i][k] && ci + 1 < sizeof(child); k++) child[ci++] = fat_names[i][k];
				child[ci] = '\0';
				fat32_stat_path(child, &is_dir, &sz);
				{
					int ni = 0;
					while (fat_names[i][ni] && ni + 1 < GUI_EXPLORER_NAME)
					{
						win->explorer_names[count][ni] = fat_names[i][ni];
						ni++;
					}
					win->explorer_names[count][ni] = '\0';
				}
				win->explorer_types[count] = is_dir ? 1 : 0;
				count++;
			}
		}
	}
	else
	{
		char names[FS_MAX_LIST][FS_NAME_MAX + 2];
		int types[FS_MAX_LIST];
		int fs_count = 0;
		if (fs_ls(win->filepath[0] == '\0' ? (void *)0 : win->filepath, names, types, FS_MAX_LIST, &fs_count) == 0)
		{
			for (i = 0; i < fs_count && count < GUI_EXPLORER_MAX; i++)
			{
				int ni = 0;
				while (names[i][ni] && ni + 1 < GUI_EXPLORER_NAME)
				{
					win->explorer_names[count][ni] = names[i][ni];
					ni++;
				}
				win->explorer_names[count][ni] = '\0';
				win->explorer_types[count] = types[i] ? 1 : 0;
				count++;
			}
		}
	}
	win->explorer_count = count;

	/* Update title to show current path */
	{
		int ti = 0;
		const char *pre = "Files: ";
		while (pre[ti] && ti < 24) { win->title[ti] = pre[ti]; ti++; }
		{
			int pi = 0;
			while (win->filepath[pi] && ti < 31) { win->title[ti++] = win->filepath[pi++]; }
		}
		win->title[ti] = '\0';
	}
}

static int gui_create_explorer_window(void)
{
	struct gui_window *win;
	unsigned int sw, sh, fh;
	int ww, wh;

	if (gui_window_count >= GUI_MAX_WINDOWS) return -1;

	fh = screen_fb_cell_h();
	sw = screen_fb_width();
	sh = screen_fb_height();

	win = &gui_windows[gui_window_count];

	ww = 300;
	wh = (int)(20 * GUI_EXPLORER_ITEM_H + GUI_TITLEBAR_H + GUI_BORDER_W * 2 + (int)fh);
	if (wh > (int)(sh - GUI_TASKBAR_H - 40)) wh = (int)(sh - GUI_TASKBAR_H - 40);
	win->w = ww;
	win->h = wh;
	win->x = ((int)sw - ww) / 2 - 60;
	win->y = ((int)(sh - GUI_TASKBAR_H) - wh) / 2;
	if (win->y < 0) win->y = 0;
	win->visible = 1;
	win->minimized = 0;
	win->wtype = GUI_WTYPE_EXPLORER;
	win->text_col = 0;
	win->text_row = 0;
	win->text_cols = 0;
	win->text_rows = 0;
	win->scroll_offset = 0;
	win->explorer_count = 0;
	win->explorer_selected = 0;
	win->explorer_scroll = 0;

	/* Set initial path */
	if (fat_mode_active())
	{
		int pi = 0;
		while (fat_cwd[pi] && pi + 1 < 128) { win->filepath[pi] = fat_cwd[pi]; pi++; }
		win->filepath[pi] = '\0';
	}
	else
	{
		fs_get_pwd(win->filepath, sizeof(win->filepath));
	}

	gui_explorer_refresh(win);

	gui_focused = gui_window_count;
	gui_window_count++;
	return gui_focused;
}

/* ── Editor file loading ─────────────────────────────────────────── */

static void gui_editor_open_file(int widx, const char *path)
{
	struct gui_window *win;
	unsigned char data[GUI_WIN_TEXT_COLS * GUI_WIN_TEXT_ROWS];
	unsigned long size = 0;
	int ok = 0;
	int i;
	unsigned long di;

	if (widx < 0 || widx >= gui_window_count) return;
	win = &gui_windows[widx];

	/* Read the file */
	if (fat_mode_active())
	{
		char resolved[128];
		if (fat_resolve_path(path, resolved, sizeof(resolved)) == 0)
		{
			if (fat32_read_file_path(resolved, data, sizeof(data) - 1, &size) == 0)
				ok = 1;
		}
	}
	else
	{
		if (fs_read_file(path, data, sizeof(data) - 1, &size) == 0)
			ok = 1;
	}

	if (!ok) return;

	/* Clear buffer */
	for (i = 0; i < GUI_WIN_TEXT_COLS * GUI_WIN_TEXT_ROWS; i++)
	{
		win->text[i] = ' ';
		win->attrs[i] = 0x1F;
	}

	/* Load file content into text grid */
	{
		int row = 0, col = 0;
		for (di = 0; di < size && row < win->text_rows - 1; di++)
		{
			char ch = (char)data[di];
			if (ch == '\n')
			{
				row++;
				col = 0;
			}
			else if (ch == '\r')
			{
				continue; /* skip CR */
			}
			else if (ch == '\t')
			{
				int spaces = 4 - (col % 4);
				while (spaces-- > 0 && col < win->text_cols)
				{
					win->text[row * win->text_cols + col] = ' ';
					col++;
				}
			}
			else
			{
				if (col < win->text_cols)
				{
					win->text[row * win->text_cols + col] = (ch >= 0x20 && ch < 0x7F) ? ch : '.';
					col++;
				}
			}
		}
	}

	win->text_col = 0;
	win->text_row = 0;
	win->editor_modified = 0;

	/* Store filepath */
	{
		int pi = 0;
		while (path[pi] && pi + 1 < 128) { win->filepath[pi] = path[pi]; pi++; }
		win->filepath[pi] = '\0';
	}

	/* Update title */
	{
		int ti = 0;
		const char *pre = "Edit: ";
		while (pre[ti] && ti < 24) { win->title[ti] = pre[ti]; ti++; }
		{
			/* Show just the filename portion */
			int last_slash = -1, pi = 0;
			while (path[pi]) { if (path[pi] == '/') last_slash = pi; pi++; }
			pi = (last_slash >= 0) ? last_slash + 1 : 0;
			while (path[pi] && ti < 31) { win->title[ti++] = path[pi++]; }
		}
		win->title[ti] = '\0';
	}
}

/* Forward declarations for GUI terminal helpers used by explorer open */
static void gui_win_putc(int widx, char c);
static void gui_win_write(int widx, const char *s);
static void gui_win_write_line(int widx, const char *s);

/* Open a file from the explorer based on its extension */
static void gui_explorer_open_selected(struct gui_window *win)
{
	char full_path[128];
	unsigned long pi = 0, ni = 0;
	const char *name;
	int is_dir;

	if (win->explorer_selected < 0 || win->explorer_selected >= win->explorer_count) return;

	name = win->explorer_names[win->explorer_selected];
	is_dir = win->explorer_types[win->explorer_selected];

	/* Build full path */
	while (win->filepath[pi] && pi + 1 < sizeof(full_path)) { full_path[pi] = win->filepath[pi]; pi++; }
	if (pi > 0 && full_path[pi - 1] != '/') full_path[pi++] = '/';
	while (name[ni] && pi + 1 < sizeof(full_path)) full_path[pi++] = name[ni++];
	full_path[pi] = '\0';

	if (is_dir)
	{
		/* Navigate into directory */
		unsigned long ci = 0;
		while (full_path[ci] && ci + 1 < sizeof(win->filepath)) { win->filepath[ci] = full_path[ci]; ci++; }
		win->filepath[ci] = '\0';
		gui_explorer_refresh(win);
		return;
	}

	/* Check extension */
	{
		int len = 0;
		while (name[len]) len++;

		/* .elf → exec */
		if (len > 4 &&
		    name[len-4] == '.' &&
		    (name[len-3] == 'e' || name[len-3] == 'E') &&
		    (name[len-2] == 'l' || name[len-2] == 'L') &&
		    (name[len-1] == 'f' || name[len-1] == 'F'))
		{
			cmd_exec(full_path);
			return;
		}

		/* .bin, .o → open in hex editor */
		if ((len > 4 &&
		     name[len-4] == '.' &&
		     (name[len-3] == 'b' || name[len-3] == 'B') &&
		     (name[len-2] == 'i' || name[len-2] == 'I') &&
		     (name[len-1] == 'n' || name[len-1] == 'N')) ||
		    (len > 2 &&
		     name[len-2] == '.' &&
		     (name[len-1] == 'o' || name[len-1] == 'O')))
		{
			int widx = gui_create_hexedit_window();
			if (widx >= 0)
				gui_hexedit_open_file(widx, full_path);
			return;
		}

		/* .sh → run shell script in a new terminal window */
		if (len > 3 &&
		    name[len-3] == '.' &&
		    (name[len-2] == 's' || name[len-2] == 'S') &&
		    (name[len-1] == 'h' || name[len-1] == 'H'))
		{
			int widx = gui_create_terminal_window();
			if (widx >= 0)
			{
				gui_win_write(widx, "Running: ");
				gui_win_write_line(widx, full_path);
				/* Execute via run command */
				{
					unsigned long ki = 0;
					const char *pre = "run ";
					while (pre[ki]) { input_buffer[ki] = pre[ki]; ki++; }
					{
						unsigned long fi = 0;
						while (full_path[fi] && ki + 1 < INPUT_BUFFER_SIZE) { input_buffer[ki++] = full_path[fi++]; }
					}
					input_buffer[ki] = '\0';
					input_length = ki;
					terminal_output_capture = 1;
					{
						static char gui_script_cap[4096];
						terminal_output_buf = gui_script_cap;
						terminal_output_buf_size = sizeof(gui_script_cap);
						terminal_output_buf_len = 0;
					}
					suppress_prompt = 1;
					run_command_dispatch();
					suppress_prompt = 0;
					terminal_output_capture = 0;
					{
						unsigned long oi;
						for (oi = 0; oi < terminal_output_buf_len; oi++)
							gui_win_putc(widx, terminal_output_buf[oi]);
					}
					terminal_output_buf = (void *)0;
					terminal_output_buf_len = 0;
				}
				gui_win_write(widx, "> ");
			}
			return;
		}

		/* .bas, .basic → run BASIC program in a new terminal window */
		if ((len > 4 &&
		     name[len-4] == '.' &&
		     (name[len-3] == 'b' || name[len-3] == 'B') &&
		     (name[len-2] == 'a' || name[len-2] == 'A') &&
		     (name[len-1] == 's' || name[len-1] == 'S')) ||
		    (len > 6 &&
		     name[len-6] == '.' &&
		     (name[len-5] == 'b' || name[len-5] == 'B') &&
		     (name[len-4] == 'a' || name[len-4] == 'A') &&
		     (name[len-3] == 's' || name[len-3] == 'S') &&
		     (name[len-2] == 'i' || name[len-2] == 'I') &&
		     (name[len-1] == 'c' || name[len-1] == 'C')))
		{
			int widx = gui_create_terminal_window();
			if (widx >= 0)
			{
				gui_win_write(widx, "Running BASIC: ");
				gui_win_write_line(widx, full_path);
				/* Execute via basic command */
				{
					unsigned long ki = 0;
					const char *pre = "basic ";
					while (pre[ki]) { input_buffer[ki] = pre[ki]; ki++; }
					{
						unsigned long fi = 0;
						while (full_path[fi] && ki + 1 < INPUT_BUFFER_SIZE) { input_buffer[ki++] = full_path[fi++]; }
					}
					input_buffer[ki] = '\0';
					input_length = ki;
					terminal_output_capture = 1;
					{
						static char gui_basic_cap[4096];
						terminal_output_buf = gui_basic_cap;
						terminal_output_buf_size = sizeof(gui_basic_cap);
						terminal_output_buf_len = 0;
					}
					suppress_prompt = 1;
					run_command_dispatch();
					suppress_prompt = 0;
					terminal_output_capture = 0;
					{
						unsigned long oi;
						for (oi = 0; oi < terminal_output_buf_len; oi++)
							gui_win_putc(widx, terminal_output_buf[oi]);
					}
					terminal_output_buf = (void *)0;
					terminal_output_buf_len = 0;
				}
				gui_win_write(widx, "> ");
			}
			return;
		}

		/* Default: open in editor */
		{
			int widx = gui_create_editor_window();
			if (widx >= 0)
				gui_editor_open_file(widx, full_path);
		}
	}
}

/* Navigate up one directory in explorer */
static void gui_explorer_go_up(struct gui_window *win)
{
	int len = 0;
	while (win->filepath[len]) len++;
	/* Remove trailing slash */
	if (len > 1 && win->filepath[len - 1] == '/') len--;
	/* Find last slash */
	while (len > 0 && win->filepath[len - 1] != '/') len--;
	if (len <= 0) { win->filepath[0] = '/'; win->filepath[1] = '\0'; }
	else { win->filepath[len] = '\0'; }
	gui_explorer_refresh(win);
}

#define GUI_MENU_ITEM_H    28
#define GUI_MENU_W        160
#define GUI_MENU_ITEMS      8

static const char *gui_menu_labels[GUI_MENU_ITEMS] = {
	"Terminal", "Paint", "Editor", "Files", "Hex Edit", "Theme       >", "---", "Exit GUI"
};

/* Count how many themes are valid (have a non-null name) */
static int gui_theme_valid_count(void)
{
	int i, c = 0;
	for (i = 0; i < GUI_THEME_COUNT; i++)
		if (gui_themes[i].name) c++;
	return c;
}

static void gui_draw_start_menu(void)
{
	unsigned int sh = screen_fb_height();
	int mx = 6; /* aligned with start button */
	int my = (int)sh - GUI_TASKBAR_H - GUI_MENU_ITEMS * GUI_MENU_ITEM_H - 4;
	int i;
	unsigned int fh = screen_fb_cell_h();

	/* Background */
	screen_fb_fill_rect((unsigned int)mx, (unsigned int)my,
		GUI_MENU_W, (unsigned int)(GUI_MENU_ITEMS * GUI_MENU_ITEM_H + 4), GUI_COL_MENU_BG);

	/* Border top */
	screen_fb_fill_rect((unsigned int)mx, (unsigned int)my, GUI_MENU_W, 2, GUI_COL_MENU_HL);

	for (i = 0; i < GUI_MENU_ITEMS; i++)
	{
		int iy = my + 2 + i * GUI_MENU_ITEM_H;
		if (gui_menu_labels[i][0] == '-')
		{
			/* Separator */
			screen_fb_fill_rect((unsigned int)(mx + 8), (unsigned int)(iy + GUI_MENU_ITEM_H / 2),
				(unsigned int)(GUI_MENU_W - 16), 1, GUI_COL_MENU_SEP);
		}
		else
		{
			int hovering = (gui_mouse_x >= mx && gui_mouse_x < mx + GUI_MENU_W &&
			    gui_mouse_y >= iy && gui_mouse_y < iy + GUI_MENU_ITEM_H);
			if (hovering)
			{
				screen_fb_fill_rect((unsigned int)mx, (unsigned int)iy,
					GUI_MENU_W, GUI_MENU_ITEM_H, GUI_COL_MENU_HL);
			}
			screen_fb_draw_string((unsigned int)(mx + 12),
				(unsigned int)(iy + (GUI_MENU_ITEM_H - fh) / 2),
				gui_menu_labels[i], GUI_COL_MENU_TEXT,
				hovering ? GUI_COL_MENU_HL : GUI_COL_MENU_BG);

			/* Auto-open/close theme submenu on hover */
			if (i == 5) /* Theme item */
			{
				if (hovering)
					gui_theme_menu_open = 1;
			}
		}
	}

	/* ── Theme submenu (right of main menu) ── */
	if (gui_theme_menu_open)
	{
		int tc = gui_theme_valid_count();
		int smx = mx + GUI_MENU_W + 2;
		int smy = my; /* align top with main menu */
		int smh = tc * GUI_MENU_ITEM_H + 4;
		int smw = 170;
		int ti;

		screen_fb_fill_rect((unsigned int)smx, (unsigned int)smy,
			(unsigned int)smw, (unsigned int)smh, GUI_COL_MENU_BG);
		screen_fb_fill_rect((unsigned int)smx, (unsigned int)smy,
			(unsigned int)smw, 2, GUI_COL_MENU_HL);

		ti = 0;
		for (i = 0; i < GUI_THEME_COUNT; i++)
		{
			int siy;
			int th_hover;
			if (!gui_themes[i].name) continue;
			siy = smy + 2 + ti * GUI_MENU_ITEM_H;
			th_hover = (gui_mouse_x >= smx && gui_mouse_x < smx + smw &&
			            gui_mouse_y >= siy && gui_mouse_y < siy + GUI_MENU_ITEM_H);
			if (th_hover || i == gui_current_theme)
			{
				screen_fb_fill_rect((unsigned int)smx, (unsigned int)siy,
					(unsigned int)smw, GUI_MENU_ITEM_H,
					th_hover ? GUI_COL_MENU_HL : 0x333355);
			}
			/* Draw color swatch */
			screen_fb_fill_rect((unsigned int)(smx + 6), (unsigned int)(siy + 6),
				12, GUI_MENU_ITEM_H - 12, gui_themes[i].desktop);
			screen_fb_draw_string((unsigned int)(smx + 22),
				(unsigned int)(siy + (GUI_MENU_ITEM_H - fh) / 2),
				gui_themes[i].name, GUI_COL_MENU_TEXT,
				(th_hover) ? GUI_COL_MENU_HL :
				(i == gui_current_theme) ? 0x333355 : GUI_COL_MENU_BG);
			/* Checkmark for active theme */
			if (i == gui_current_theme)
				screen_fb_draw_char((unsigned int)(smx + smw - 16),
					(unsigned int)(siy + (GUI_MENU_ITEM_H - fh) / 2),
					'*', 0x00FF00,
					th_hover ? GUI_COL_MENU_HL : 0x333355);
			ti++;
		}

		/* Close submenu if mouse moves away from both menus */
		if (!(gui_mouse_x >= mx && gui_mouse_x < smx + smw &&
		      gui_mouse_y >= smy && gui_mouse_y < smy + smh + (int)(GUI_MENU_ITEMS * GUI_MENU_ITEM_H)))
		{
			/* Only close if not hovering the "Theme" menu item either */
			int theme_iy = my + 2 + 5 * GUI_MENU_ITEM_H;
			if (!(gui_mouse_x >= mx && gui_mouse_x < mx + GUI_MENU_W &&
			      gui_mouse_y >= theme_iy && gui_mouse_y < theme_iy + GUI_MENU_ITEM_H))
				gui_theme_menu_open = 0;
		}
	}
}

/* Returns menu item index (0-based) if click hits a menu item, or -1 */
static int gui_start_menu_hit(int px, int py)
{
	unsigned int sh = screen_fb_height();
	int mx = 6;
	int my = (int)sh - GUI_TASKBAR_H - GUI_MENU_ITEMS * GUI_MENU_ITEM_H - 4;
	int i;

	if (px < mx || px >= mx + GUI_MENU_W) return -1;
	if (py < my || py >= my + 2 + GUI_MENU_ITEMS * GUI_MENU_ITEM_H) return -1;

	for (i = 0; i < GUI_MENU_ITEMS; i++)
	{
		int iy = my + 2 + i * GUI_MENU_ITEM_H;
		if (py >= iy && py < iy + GUI_MENU_ITEM_H)
		{
			if (gui_menu_labels[i][0] == '-') return -1; /* separator */
			return i;
		}
	}
	return -1;
}

/* Returns theme index if click hits a theme submenu item, or -1 */
static int gui_theme_menu_hit(int px, int py)
{
	unsigned int sh = screen_fb_height();
	int mx = 6;
	int my = (int)sh - GUI_TASKBAR_H - GUI_MENU_ITEMS * GUI_MENU_ITEM_H - 4;
	int smx = mx + GUI_MENU_W + 2;
	int smy = my;
	int smw = 170;
	int ti = 0, i;
	if (!gui_theme_menu_open) return -1;
	if (px < smx || px >= smx + smw) return -1;

	for (i = 0; i < GUI_THEME_COUNT; i++)
	{
		int siy;
		if (!gui_themes[i].name) continue;
		siy = smy + 2 + ti * GUI_MENU_ITEM_H;
		if (py >= siy && py < siy + GUI_MENU_ITEM_H)
			return i;
		ti++;
	}
	return -1;
}

/* Hit-test for the Start button itself (returns 1 if clicked) */
static int gui_hit_start_button(int px, int py)
{
	unsigned int sh = screen_fb_height();
	int bx = 6;
	int by = (int)sh - GUI_TASKBAR_H + 4;
	int bw = 56;
	int bh = GUI_TASKBAR_H - 8;
	return (px >= bx && px < bx + bw && py >= by && py < by + bh);
}

/* Write a character to the focused terminal window */
static void gui_win_putc(int widx, char c)
{
	struct gui_window *win;
	int i;
	if (widx < 0 || widx >= gui_window_count) return;
	win = &gui_windows[widx];

	if (c == '\n')
	{
		win->text_col = 0;
		win->text_row++;
	}
	else if (c == '\r')
	{
		win->text_col = 0;
	}
	else if (c == '\b')
	{
		if (win->text_col > 0) win->text_col--;
	}
	else
	{
		if (win->text_col >= win->text_cols)
		{
			win->text_col = 0;
			win->text_row++;
		}
		if (win->text_row >= win->text_rows)
		{
			/* Scroll up */
			for (i = 0; i < win->text_cols * (win->text_rows - 1); i++)
			{
				win->text[i] = win->text[i + win->text_cols];
				win->attrs[i] = win->attrs[i + win->text_cols];
			}
			for (i = win->text_cols * (win->text_rows - 1); i < win->text_cols * win->text_rows; i++)
			{
				win->text[i] = ' ';
				win->attrs[i] = 0x0F;
			}
			win->text_row = win->text_rows - 1;
		}
		{
			int off = win->text_row * win->text_cols + win->text_col;
			win->text[off] = c;
			win->attrs[off] = terminal_text_color;
			win->text_col++;
		}
	}

	/* Handle scroll when past bottom */
	if (win->text_row >= win->text_rows)
	{
		for (i = 0; i < win->text_cols * (win->text_rows - 1); i++)
		{
			win->text[i] = win->text[i + win->text_cols];
			win->attrs[i] = win->attrs[i + win->text_cols];
		}
		for (i = win->text_cols * (win->text_rows - 1); i < win->text_cols * win->text_rows; i++)
		{
			win->text[i] = ' ';
			win->attrs[i] = 0x0F;
		}
		win->text_row = win->text_rows - 1;
	}
}

static void gui_win_write(int widx, const char *s)
{
	while (*s) { gui_win_putc(widx, *s); s++; }
}

static void gui_win_write_line(int widx, const char *s)
{
	gui_win_write(widx, s);
	gui_win_putc(widx, '\n');
}

/* Hit-test: returns window index at pixel (px,py) or -1 */
static int gui_hit_test(int px, int py)
{
	int i;
	/* Back to front, top window gets priority */
	for (i = gui_window_count - 1; i >= 0; i--)
	{
		struct gui_window *w = &gui_windows[i];
		if (!w->visible || w->minimized) continue;
		if (px >= w->x && px < w->x + w->w && py >= w->y && py < w->y + w->h)
			return i;
	}
	return -1;
}

/* Check if click is on close button */
static int gui_hit_close(int widx, int px, int py)
{
	struct gui_window *w;
	int cbx, cby;
	if (widx < 0 || widx >= gui_window_count) return 0;
	w = &gui_windows[widx];
	cbx = w->x + w->w - GUI_BORDER_W - 20;
	cby = w->y + GUI_BORDER_W + 2;
	return (px >= cbx && px < cbx + 18 && py >= cby && py < cby + 18);
}

/* Check if click is on title bar (for dragging) */
static int gui_hit_titlebar(int widx, int px, int py)
{
	struct gui_window *w;
	if (widx < 0 || widx >= gui_window_count) return 0;
	w = &gui_windows[widx];
	return (px >= w->x + GUI_BORDER_W && px < w->x + w->w - GUI_BORDER_W - 20 &&
	        py >= w->y + GUI_BORDER_W && py < w->y + GUI_BORDER_W + GUI_TITLEBAR_H);
}

/* Check if click is on resize grip (bottom-right corner) */
static int gui_hit_resize(int widx, int px, int py)
{
	struct gui_window *w;
	if (widx < 0 || widx >= gui_window_count) return 0;
	w = &gui_windows[widx];
	return (px >= w->x + w->w - GUI_RESIZE_HANDLE &&
	        px < w->x + w->w &&
	        py >= w->y + w->h - GUI_RESIZE_HANDLE &&
	        py < w->y + w->h);
}

static void gui_close_window(int widx)
{
	int i;
	if (widx < 0 || widx >= gui_window_count) return;
	/* shift windows down */
	for (i = widx; i < gui_window_count - 1; i++)
		gui_windows[i] = gui_windows[i + 1];
	gui_window_count--;
	if (gui_focused >= gui_window_count) gui_focused = gui_window_count - 1;
}

/* The main GUI event loop — runs until user exits GUI mode */
static void gui_event_loop(void)
{
	unsigned int sw = screen_fb_width();
	unsigned int sh = screen_fb_height();
	unsigned long last_clock_tick = 0;
	int need_redraw = 1;

	/* Initial mouse position: center of screen */
	gui_mouse_x = (int)sw / 2;
	gui_mouse_y = (int)sh / 2;
	/* Store current driver accumulators as baseline for delta tracking */
	gui_mouse_prev_x = mouse_get_x();
	gui_mouse_prev_y = mouse_get_y();
	gui_mouse_buttons = 0;
	gui_mouse_prev_buttons = 0;
	gui_cursor_saved = 0;

	/* Disable preemptive scheduling — the GUI loop owns the CPU */
	task_set_preemption(0);

	/* Guarantee interrupts are enabled (required for hlt and IRQs) */
	__asm__ volatile("sti");

	/* Drain any leftover scancodes (e.g. Enter-release from typing 'gui') */
	while (scancode_queue_tail != scancode_queue_head)
		scancode_queue_tail = (scancode_queue_tail + 1) % SCANCODE_QUEUE_SIZE;

	gui_redraw_all();

	for (;;)
	{
		int mx, my, mb;
		int dirty = 0;

		/* Poll mouse */
		{
			/* Try USB first (QEMU/Proxmox sends movement via USB tablet) */
			usb_hid_poll();
			if (usb_mouse_valid())
			{
				if (usb_mouse_is_absolute())
				{
					/* Tablet: map 0-32767 to screen pixels */
					mx = (int)((unsigned long)usb_mouse_abs_x() * sw / 32768);
					my = (int)((unsigned long)usb_mouse_abs_y() * sh / 32768);
				}
				else
				{
					/* Relative USB mouse */
					mx = gui_mouse_x + usb_mouse_rel_dx();
					my = gui_mouse_y + usb_mouse_rel_dy();
				}
				mb = usb_mouse_buttons();
			}
			else
			{
				/* Fallback: PS/2 mouse */
				int raw_x = mouse_get_x();
				int raw_y = mouse_get_y();
				mx = gui_mouse_x + (raw_x - gui_mouse_prev_x);
				my = gui_mouse_y + (raw_y - gui_mouse_prev_y);
				gui_mouse_prev_x = raw_x;
				gui_mouse_prev_y = raw_y;
				mb = mouse_get_buttons();
			}
		}

		/* Clamp to screen */
		if (mx < 0) mx = 0;
		if (my < 0) my = 0;
		if (mx >= (int)sw) mx = (int)sw - 1;
		if (my >= (int)sh) my = (int)sh - 1;

		/* Mouse moved? */
		if (mx != gui_mouse_x || my != gui_mouse_y)
		{
			/* Erase old cursor */
			gui_restore_under_cursor(gui_mouse_x, gui_mouse_y);

			/* Handle dragging */
			if (gui_drag_window >= 0 && gui_drag_window < gui_window_count)
			{
				struct gui_window *dw = &gui_windows[gui_drag_window];
				dw->x = mx - gui_drag_ox;
				dw->y = my - gui_drag_oy;
				/* Clamp to screen */
				if (dw->x < 0) dw->x = 0;
				if (dw->y < 0) dw->y = 0;
				if (dw->x + dw->w > (int)sw) dw->x = (int)sw - dw->w;
				if (dw->y + dw->h > (int)sh) dw->y = (int)sh - dw->h;
				dirty = 1;
			}

			/* Handle resize dragging */
			if (gui_resize_window >= 0 && gui_resize_window < gui_window_count)
			{
				struct gui_window *rw = &gui_windows[gui_resize_window];
				int nw = gui_resize_start_w + (mx - gui_resize_start_mx);
				int nh = gui_resize_start_h + (my - gui_resize_start_my);
				if (nw < GUI_MIN_WIN_W) nw = GUI_MIN_WIN_W;
				if (nh < GUI_MIN_WIN_H) nh = GUI_MIN_WIN_H;
				if (rw->x + nw > (int)sw) nw = (int)sw - rw->x;
				if (rw->y + nh > (int)sh) nh = (int)sh - rw->y;
				rw->w = nw;
				rw->h = nh;
				dirty = 1;
			}

			gui_mouse_x = mx;
			gui_mouse_y = my;

			if (dirty)
				gui_redraw_all();
			else
				gui_draw_cursor(gui_mouse_x, gui_mouse_y);
		}

		/* Mouse button events */
		if (mb != gui_mouse_prev_buttons)
		{
			int left_down = (mb & 1) && !(gui_mouse_prev_buttons & 1);
			int left_up = !(mb & 1) && (gui_mouse_prev_buttons & 1);

			if (left_down)
			{
				/* Check start menu first */
				if (gui_start_menu_open)
				{
					/* Check theme submenu first */
					int thi = gui_theme_menu_hit(mx, my);
					if (thi >= 0)
					{
						gui_current_theme = thi;
						gui_start_menu_open = 0;
						gui_theme_menu_open = 0;
						need_redraw = 1;
					}
					else
					{
					int mi = gui_start_menu_hit(mx, my);
					if (mi == 5) /* Theme — toggle submenu, keep start menu open */
					{
						gui_theme_menu_open = !gui_theme_menu_open;
						need_redraw = 1;
					}
					else
					{
					gui_start_menu_open = 0;
					gui_theme_menu_open = 0;
					if (mi == 0) /* Terminal */
					{
						int widx = gui_create_terminal_window();
						if (widx >= 0) {
							gui_win_write_line(widx, "TG11-OS Terminal");
							gui_win_write(widx, "> ");
						}
					}
					else if (mi == 1) /* Paint */
					{
						gui_create_paint_window();
					}
					else if (mi == 2) /* Editor */
					{
						int widx = gui_create_editor_window();
						if (widx >= 0) {
							gui_win_write_line(widx, "TG11-OS Editor  --  Type to edit. Esc closes.");
						}
					}
					else if (mi == 3) /* Files */
					{
						gui_create_explorer_window();
					}
					else if (mi == 4) /* Hex Edit */
					{
						gui_create_hexedit_window();
					}
					else if (mi == 7) /* Exit GUI */
					{
						gui_active = 0;
						gui_mouse_prev_buttons = mb;
						return;
					}
					/* If click was outside menu or on separator, just close menu */
					need_redraw = 1;
					}
				}
				} /* end else (theme submenu miss) */
				else if (gui_hit_start_button(mx, my))
				{
					gui_start_menu_open = !gui_start_menu_open;
					gui_theme_menu_open = 0;
					need_redraw = 1;
				}
				else
				{
					/* Close start menu if clicking elsewhere */
					if (gui_start_menu_open) { gui_start_menu_open = 0; gui_theme_menu_open = 0; need_redraw = 1; }

					int hit = gui_hit_test(mx, my);
					if (hit >= 0)
					{
						if (gui_hit_close(hit, mx, my))
						{
							gui_close_window(hit);
							/* Don't exit GUI when closing windows — use Start > Exit */
							need_redraw = 1;
						}
						else if (gui_hit_resize(hit, mx, my))
						{
							gui_focused = hit;
							gui_resize_window = hit;
							gui_resize_start_w = gui_windows[hit].w;
							gui_resize_start_h = gui_windows[hit].h;
							gui_resize_start_mx = mx;
							gui_resize_start_my = my;
							need_redraw = 1;
						}
						else if (gui_hit_titlebar(hit, mx, my))
						{
							gui_focused = hit;
							gui_drag_window = hit;
							gui_drag_ox = mx - gui_windows[hit].x;
							gui_drag_oy = my - gui_windows[hit].y;
							need_redraw = 1;
						}
						else
						{
							gui_focused = hit;

							/* Paint: check toolbar/palette click or start drawing */
							if (gui_windows[hit].wtype == GUI_WTYPE_PAINT)
							{
								struct gui_window *pw = &gui_windows[hit];
								int cx2 = pw->x + GUI_BORDER_W;
								int cy2 = pw->y + GUI_BORDER_W + GUI_TITLEBAR_H;
								int toolbar_y2 = cy2;
								int canvas_y2 = cy2 + GUI_PAINT_TOOLBAR_H;
								int canvas_h = pw->h - GUI_BORDER_W * 2 - GUI_TITLEBAR_H - GUI_PAINT_PALETTE_H - GUI_PAINT_TOOLBAR_H;
								int palette_y = canvas_y2 + canvas_h;
								int cw2 = pw->w - GUI_BORDER_W * 2;

								if (my >= toolbar_y2 && my < toolbar_y2 + GUI_PAINT_TOOLBAR_H)
								{
									/* Clicked on toolbar — pick tool or change brush size */
									int btn_w = 50;
									int ti = (mx - cx2 - 4) / (btn_w + 4);
									if (ti >= 0 && ti < GUI_PAINT_TOOL_COUNT &&
									    mx >= cx2 + 4 + ti * (btn_w + 4) &&
									    mx < cx2 + 4 + ti * (btn_w + 4) + btn_w)
									{
										pw->paint_tool = ti;
									}
									else
									{
										/* Click in brush size area — cycle: 1,2,3,5 */
										int ns = pw->paint_brush_size + 1;
										if (ns == 4) ns = 5;
										if (ns > 5) ns = 1;
										pw->paint_brush_size = ns;
									}
									need_redraw = 1;
								}
								else if (my >= palette_y && my < palette_y + GUI_PAINT_PALETTE_H)
								{
									/* Clicked on palette — pick color */
									int swatch_w = cw2 / GUI_PAINT_COLORS;
									if (swatch_w < 8) swatch_w = 8;
									int pi = (mx - cx2) / swatch_w;
									if (pi >= 0 && pi < GUI_PAINT_COLORS)
										pw->paint_color = gui_paint_palette[pi];
									need_redraw = 1;
								}
								else if (mx >= cx2 && mx < cx2 + cw2 &&
								         my >= canvas_y2 && my < canvas_y2 + canvas_h)
								{
									/* Start drawing on canvas */
									int bpx = mx - cx2;
									int bpy = my - canvas_y2;
									pw->paint_drawing = 1;
									pw->paint_line_x0 = bpx;
									pw->paint_line_y0 = bpy;
									if (pw->paint_tool == GUI_PAINT_TOOL_BRUSH ||
									    pw->paint_tool == GUI_PAINT_TOOL_ERASER)
									{
										unsigned int dc = (pw->paint_tool == GUI_PAINT_TOOL_ERASER) ? 0xFFFFFF : pw->paint_color;
										gui_paint_brush_circle(bpx, bpy, pw->paint_brush_size, dc);
										need_redraw = 1;
									}
									else if (pw->paint_tool == GUI_PAINT_TOOL_FILL)
									{
										gui_paint_buf_flood(bpx, bpy, pw->paint_color);
										pw->paint_drawing = 0;
										need_redraw = 1;
									}
								}
							}
							/* Explorer: click to select, double-click to open */
							else if (gui_windows[hit].wtype == GUI_WTYPE_EXPLORER)
							{
								struct gui_window *ew = &gui_windows[hit];
								unsigned int ew_fh = screen_fb_cell_h();
								int bar_h = (int)ew_fh + 4;
								int list_y = ew->y + GUI_BORDER_W + GUI_TITLEBAR_H + bar_h;
								int list_h = ew->h - GUI_BORDER_W * 2 - GUI_TITLEBAR_H - bar_h;
								int visible_rows = list_h / GUI_EXPLORER_ITEM_H;
								int has_parent = (ew->filepath[0] != '\0' &&
								                  !(ew->filepath[0] == '/' && ew->filepath[1] == '\0'));
								(void)visible_rows;

								if (my >= list_y && my < list_y + list_h)
								{
									int row_clicked = (my - list_y) / GUI_EXPLORER_ITEM_H;
									int old_selected = ew->explorer_selected;

									if (has_parent && ew->explorer_scroll == 0)
									{
										if (row_clicked == 0)
										{
											/* Clicked ".." — go up */
											if (old_selected == -1)
												gui_explorer_go_up(ew);
											else
												ew->explorer_selected = -1;
											need_redraw = 1;
										}
										else
										{
											int entry_idx = ew->explorer_scroll + row_clicked - 1;
											if (entry_idx >= 0 && entry_idx < ew->explorer_count)
											{
												if (old_selected == entry_idx)
													gui_explorer_open_selected(ew);
												else
													ew->explorer_selected = entry_idx;
											}
										}
									}
									else
									{
										int entry_idx = ew->explorer_scroll + row_clicked;
										if (entry_idx >= 0 && entry_idx < ew->explorer_count)
										{
											if (old_selected == entry_idx)
												gui_explorer_open_selected(ew);
											else
												ew->explorer_selected = entry_idx;
										}
									}
									need_redraw = 1;
								}
							}
							need_redraw = 1;
						}
					}
				}
			}
			if (left_up)
			{
				gui_drag_window = -1;
				gui_resize_window = -1;
				/* Stop paint drawing — commit line/rect on release */
				if (gui_focused >= 0 && gui_focused < gui_window_count &&
				    gui_windows[gui_focused].wtype == GUI_WTYPE_PAINT &&
				    gui_windows[gui_focused].paint_drawing)
				{
					struct gui_window *pw2 = &gui_windows[gui_focused];
					int cx3 = pw2->x + GUI_BORDER_W;
					int canvas_y3 = pw2->y + GUI_BORDER_W + GUI_TITLEBAR_H + GUI_PAINT_TOOLBAR_H;
					int bpx2 = mx - cx3;
					int bpy2 = my - canvas_y3;
					if (pw2->paint_tool == GUI_PAINT_TOOL_LINE)
					{
						gui_paint_buf_line(pw2->paint_line_x0, pw2->paint_line_y0,
							bpx2, bpy2, pw2->paint_brush_size, pw2->paint_color);
						need_redraw = 1;
					}
					else if (pw2->paint_tool == GUI_PAINT_TOOL_RECT)
					{
						gui_paint_buf_rect(pw2->paint_line_x0, pw2->paint_line_y0,
							bpx2, bpy2, pw2->paint_brush_size, pw2->paint_color);
						need_redraw = 1;
					}
					pw2->paint_drawing = 0;
				}
			}
			gui_mouse_prev_buttons = mb;
		}

		/* Paint: continuous drawing while mouse held */
		if ((mb & 1) && gui_focused >= 0 && gui_focused < gui_window_count &&
		    gui_windows[gui_focused].wtype == GUI_WTYPE_PAINT &&
		    gui_windows[gui_focused].paint_drawing)
		{
			struct gui_window *pw = &gui_windows[gui_focused];
			int cx2 = pw->x + GUI_BORDER_W;
			int canvas_y2 = pw->y + GUI_BORDER_W + GUI_TITLEBAR_H + GUI_PAINT_TOOLBAR_H;
			int canvas_h = pw->h - GUI_BORDER_W * 2 - GUI_TITLEBAR_H - GUI_PAINT_PALETTE_H - GUI_PAINT_TOOLBAR_H;
			int cw2 = pw->w - GUI_BORDER_W * 2;

			if (mx >= cx2 && mx < cx2 + cw2 && my >= canvas_y2 && my < canvas_y2 + canvas_h)
			{
				int bpx = mx - cx2;
				int bpy = my - canvas_y2;
				if (pw->paint_tool == GUI_PAINT_TOOL_BRUSH ||
				    pw->paint_tool == GUI_PAINT_TOOL_ERASER)
				{
					unsigned int dc = (pw->paint_tool == GUI_PAINT_TOOL_ERASER) ? 0xFFFFFF : pw->paint_color;
					/* Draw line from last point to current for smooth strokes */
					gui_paint_buf_line(pw->paint_line_x0, pw->paint_line_y0,
						bpx, bpy, pw->paint_brush_size, dc);
					pw->paint_line_x0 = bpx;
					pw->paint_line_y0 = bpy;
					dirty = 1;
				}
				/* For line/rect tools, just track the endpoint — draw on release */
			}
		}

		/* Poll keyboard — process all queued scancodes this frame */
		while (scancode_queue_tail != scancode_queue_head)
		{
			unsigned char sc = scancode_queue[scancode_queue_tail];
			scancode_queue_tail = (scancode_queue_tail + 1) % SCANCODE_QUEUE_SIZE;

			/* Modifier tracking */
			if (sc == 0x2A || sc == 0x36) { shift_held = 1; continue; }
			if (sc == 0xAA || sc == 0xB6) { shift_held = 0; continue; }
			if (sc == 0x1D) { ctrl_held = 1; continue; }
			if (sc == 0x9D) { ctrl_held = 0; continue; }
			if (sc == 0x38) { alt_held = 1; continue; }
			if (sc == 0xB8) { alt_held = 0; continue; }
			if (sc >= 0x80) continue; /* key release */

			/* Esc: close start menu, or close focused editor/paint, or (if no windows) do nothing */
			if (sc == 0x01)
			{
				if (gui_start_menu_open) { gui_start_menu_open = 0; need_redraw = 1; continue; }
				/* If focused window is paint or editor, close it */
				if (gui_focused >= 0 && gui_focused < gui_window_count &&
				    gui_windows[gui_focused].wtype != GUI_WTYPE_TERMINAL)
				{
					gui_close_window(gui_focused);
					need_redraw = 1;
					continue;
				}
				/* Don't exit GUI on Esc — use Start > Exit GUI */
				continue;
			}

			/* Arrow key mouse movement removed — arrows are handled by
			   individual window types (editor, hexedit, explorer, terminal) */

			/* Tab simulates a left click at the current cursor position */
			if (sc == 0x0F) /* Tab */
			{
				int hit = gui_hit_test(gui_mouse_x, gui_mouse_y);
				if (hit >= 0)
				{
					if (gui_hit_close(hit, gui_mouse_x, gui_mouse_y))
					{
						gui_close_window(hit);
					}
					else
						gui_focused = hit;
					need_redraw = 1;
				}
				continue;
			}

			/* ── Type into focused window (dispatch by type) ──── */
			if (gui_focused >= 0 && gui_focused < gui_window_count)
			{
				struct gui_window *fwin = &gui_windows[gui_focused];

				/* ── EDITOR window keyboard handling ── */
				if (fwin->wtype == GUI_WTYPE_EDITOR)
				{
					char c = translate_scancode(sc);

					/* Ctrl+S: save file */
					if (ctrl_held && sc == 0x1F && fwin->filepath[0] != '\0')
					{
						/* Flatten text grid into a linear buffer */
						static char save_buf[GUI_WIN_TEXT_COLS * GUI_WIN_TEXT_ROWS + GUI_WIN_TEXT_ROWS];
						int save_len = 0;
						int sr, sc2;
						for (sr = 0; sr < fwin->text_rows - 1; sr++)
						{
							int line_end = fwin->text_cols - 1;
							while (line_end >= 0 && fwin->text[sr * fwin->text_cols + line_end] == ' ')
								line_end--;
							for (sc2 = 0; sc2 <= line_end; sc2++)
								save_buf[save_len++] = fwin->text[sr * fwin->text_cols + sc2];
							/* Always add newline unless this is a completely empty trailing line */
							if (line_end >= 0 || sr == 0)
								save_buf[save_len++] = '\n';
							else
							{
								/* Check if any subsequent rows have content */
								int has_more = 0, cr;
								for (cr = sr + 1; cr < fwin->text_rows - 1; cr++)
								{
									int ce;
									for (ce = 0; ce < fwin->text_cols; ce++)
										if (fwin->text[cr * fwin->text_cols + ce] != ' ') { has_more = 1; break; }
									if (has_more) break;
								}
								if (has_more)
									save_buf[save_len++] = '\n';
								else
									break; /* stop at trailing blank lines */
							}
						}
						/* Write file */
						if (fat_mode_active())
						{
							char resolved[128];
							if (fat_resolve_path(fwin->filepath, resolved, sizeof(resolved)) == 0)
								fat32_write_file_path(resolved, (const unsigned char *)save_buf, save_len);
						}
						else
						{
							fs_write_file(fwin->filepath, (const unsigned char *)save_buf, save_len);
						}
						fwin->editor_modified = 0;
						need_redraw = 1;
						continue;
					}

					/* Arrow keys move cursor in editor */
					if (sc == 0x48) /* Up */
					{
						if (fwin->text_row > 0) fwin->text_row--;
						need_redraw = 1; continue;
					}
					if (sc == 0x50) /* Down */
					{
						if (fwin->text_row < fwin->text_rows - 2) fwin->text_row++; /* -2: status bar */
						need_redraw = 1; continue;
					}
					if (sc == 0x4B) /* Left */
					{
						if (fwin->text_col > 0) fwin->text_col--;
						else if (fwin->text_row > 0) { fwin->text_row--; fwin->text_col = fwin->text_cols - 1; }
						need_redraw = 1; continue;
					}
					if (sc == 0x4D) /* Right */
					{
						if (fwin->text_col < fwin->text_cols - 1) fwin->text_col++;
						else if (fwin->text_row < fwin->text_rows - 2) { fwin->text_row++; fwin->text_col = 0; }
						need_redraw = 1; continue;
					}
					if (sc == 0x47) /* Home */
					{
						fwin->text_col = 0; need_redraw = 1; continue;
					}
					if (sc == 0x4F) /* End */
					{
						/* Find end of text on current line */
						int ei = fwin->text_cols - 1;
						while (ei > 0 && fwin->text[fwin->text_row * fwin->text_cols + ei] == ' ') ei--;
						fwin->text_col = ei + 1;
						if (fwin->text_col >= fwin->text_cols) fwin->text_col = fwin->text_cols - 1;
						need_redraw = 1; continue;
					}

					if (sc == 0x1C) /* Enter */
					{
						if (fwin->text_row < fwin->text_rows - 2)
						{
							fwin->text_row++;
							fwin->text_col = 0;
						}
						fwin->editor_modified = 1;
						need_redraw = 1;
					}
					else if (sc == 0x0E) /* Backspace */
					{
						if (fwin->text_col > 0)
						{
							fwin->text_col--;
							fwin->text[fwin->text_row * fwin->text_cols + fwin->text_col] = ' ';
							fwin->editor_modified = 1;
							need_redraw = 1;
						}
						else if (fwin->text_row > 0)
						{
							fwin->text_row--;
							fwin->text_col = fwin->text_cols - 1;
							fwin->editor_modified = 1;
							need_redraw = 1;
						}
					}
					else if (sc == 0x53) /* Delete */
					{
						int off = fwin->text_row * fwin->text_cols + fwin->text_col;
						int ei;
						for (ei = off; ei < fwin->text_row * fwin->text_cols + fwin->text_cols - 1; ei++)
							fwin->text[ei] = fwin->text[ei + 1];
						fwin->text[fwin->text_row * fwin->text_cols + fwin->text_cols - 1] = ' ';
						fwin->editor_modified = 1;
						need_redraw = 1;
					}
					else if (c != '\0')
					{
						int off = fwin->text_row * fwin->text_cols + fwin->text_col;
						fwin->text[off] = c;
						fwin->text_col++;
						if (fwin->text_col >= fwin->text_cols)
						{
							fwin->text_col = 0;
							if (fwin->text_row < fwin->text_rows - 2) fwin->text_row++;
						}
						fwin->editor_modified = 1;
						need_redraw = 1;
					}
					continue; /* editor consumed the key */
				}

				/* ── PAINT window: ignore keyboard (except handled above) ── */
				if (fwin->wtype == GUI_WTYPE_PAINT)
					continue;

				/* ── HEXEDIT window keyboard handling ── */
				if (fwin->wtype == GUI_WTYPE_HEXEDIT)
				{
					unsigned int hex_fh = screen_fb_cell_h();
					int hex_ch = fwin->h - GUI_BORDER_W * 2 - GUI_TITLEBAR_H;
					int hex_vis = hex_ch / (int)hex_fh - 1; /* minus status bar */
					int hex_bytes_per_page = hex_vis * 16;
					if (hex_vis < 1) hex_vis = 1;

					/* Ctrl+S: save hex data */
					if (ctrl_held && sc == 0x1F && fwin->filepath[0] != '\0')
					{
						if (fat_mode_active())
						{
							char resolved[128];
							if (fat_resolve_path(fwin->filepath, resolved, sizeof(resolved)) == 0)
								fat32_write_file_path(resolved, fwin->hex_data, fwin->hex_size);
						}
						else
						{
							fs_write_file(fwin->filepath, fwin->hex_data, fwin->hex_size);
						}
						fwin->editor_modified = 0;
						need_redraw = 1;
						continue;
					}

					/* Arrow keys */
					if (sc == 0x4D) /* Right */
					{
						if (fwin->hex_nibble == 0)
							fwin->hex_nibble = 1;
						else
						{
							fwin->hex_nibble = 0;
							if (fwin->hex_cursor + 1 < fwin->hex_size)
								fwin->hex_cursor++;
						}
						need_redraw = 1; continue;
					}
					if (sc == 0x4B) /* Left */
					{
						if (fwin->hex_nibble == 1)
							fwin->hex_nibble = 0;
						else
						{
							fwin->hex_nibble = 1;
							if (fwin->hex_cursor > 0)
								fwin->hex_cursor--;
						}
						need_redraw = 1; continue;
					}
					if (sc == 0x50) /* Down */
					{
						if (fwin->hex_cursor + 16 < fwin->hex_size)
							fwin->hex_cursor += 16;
						need_redraw = 1; continue;
					}
					if (sc == 0x48) /* Up */
					{
						if (fwin->hex_cursor >= 16)
							fwin->hex_cursor -= 16;
						need_redraw = 1; continue;
					}
					if (sc == 0x49) /* PgUp */
					{
						if (fwin->hex_cursor >= (unsigned long)hex_bytes_per_page)
							fwin->hex_cursor -= (unsigned long)hex_bytes_per_page;
						else
							fwin->hex_cursor = 0;
						need_redraw = 1; continue;
					}
					if (sc == 0x51) /* PgDn */
					{
						fwin->hex_cursor += (unsigned long)hex_bytes_per_page;
						if (fwin->hex_cursor >= fwin->hex_size && fwin->hex_size > 0)
							fwin->hex_cursor = fwin->hex_size - 1;
						need_redraw = 1; continue;
					}
					if (sc == 0x47) /* Home */
					{
						fwin->hex_cursor = 0;
						fwin->hex_nibble = 0;
						need_redraw = 1; continue;
					}
					if (sc == 0x4F) /* End */
					{
						if (fwin->hex_size > 0) fwin->hex_cursor = fwin->hex_size - 1;
						fwin->hex_nibble = 0;
						need_redraw = 1; continue;
					}

					/* Hex digit input: 0-9, A-F */
					{
						int hval = -1;
						char c2 = translate_scancode(sc);
						if (c2 >= '0' && c2 <= '9') hval = c2 - '0';
						else if (c2 >= 'a' && c2 <= 'f') hval = c2 - 'a' + 10;
						else if (c2 >= 'A' && c2 <= 'F') hval = c2 - 'A' + 10;

						if (hval >= 0 && fwin->hex_cursor < fwin->hex_size)
						{
							unsigned char b = fwin->hex_data[fwin->hex_cursor];
							if (fwin->hex_nibble == 0)
								b = (unsigned char)((hval << 4) | (b & 0x0F));
							else
								b = (unsigned char)((b & 0xF0) | hval);
							fwin->hex_data[fwin->hex_cursor] = b;
							fwin->editor_modified = 1;
							/* Advance to next nibble/byte */
							if (fwin->hex_nibble == 0)
								fwin->hex_nibble = 1;
							else
							{
								fwin->hex_nibble = 0;
								if (fwin->hex_cursor + 1 < fwin->hex_size)
									fwin->hex_cursor++;
							}
							need_redraw = 1;
						}
					}

					/* Auto-scroll hex view to keep cursor visible */
					{
						unsigned long cursor_row = fwin->hex_cursor / 16;
						unsigned long offset_row = fwin->hex_offset / 16;
						if (cursor_row < offset_row)
							fwin->hex_offset = cursor_row * 16;
						else if (cursor_row >= offset_row + (unsigned long)hex_vis)
							fwin->hex_offset = (cursor_row - (unsigned long)hex_vis + 1) * 16;
					}
					continue;
				}

				/* ── EXPLORER window keyboard handling ── */
				if (fwin->wtype == GUI_WTYPE_EXPLORER)
				{
					if (sc == 0x48) /* Up */
					{
						if (fwin->explorer_selected > 0)
							fwin->explorer_selected--;
						else
							fwin->explorer_selected = -1; /* select ".." */
						/* Scroll if needed */
						if (fwin->explorer_selected == -1)
							fwin->explorer_scroll = 0;
						else if (fwin->explorer_selected < fwin->explorer_scroll)
							fwin->explorer_scroll = fwin->explorer_selected;
						need_redraw = 1;
					}
					else if (sc == 0x50) /* Down */
					{
						if (fwin->explorer_selected < fwin->explorer_count - 1)
							fwin->explorer_selected++;
						{
							unsigned int ew_fh = screen_fb_cell_h();
							int bar_h = (int)ew_fh + 4;
							int list_h = fwin->h - GUI_BORDER_W * 2 - GUI_TITLEBAR_H - bar_h;
							int vis = list_h / GUI_EXPLORER_ITEM_H;
							int has_par = (fwin->filepath[0] != '\0' &&
							               !(fwin->filepath[0] == '/' && fwin->filepath[1] == '\0'));
							if (has_par) vis--;
							if (fwin->explorer_selected >= fwin->explorer_scroll + vis)
								fwin->explorer_scroll = fwin->explorer_selected - vis + 1;
						}
						need_redraw = 1;
					}
					else if (sc == 0x49) /* Page Up */
					{
						unsigned int ew_fh = screen_fb_cell_h();
						int bar_h = (int)ew_fh + 4;
						int list_h = fwin->h - GUI_BORDER_W * 2 - GUI_TITLEBAR_H - bar_h;
						int vis = list_h / GUI_EXPLORER_ITEM_H;
						int has_par = (fwin->filepath[0] != '\0' &&
						               !(fwin->filepath[0] == '/' && fwin->filepath[1] == '\0'));
						if (has_par) vis--;
						fwin->explorer_selected -= vis;
						if (fwin->explorer_selected < 0)
							fwin->explorer_selected = has_par ? -1 : 0;
						if (fwin->explorer_selected == -1)
							fwin->explorer_scroll = 0;
						else if (fwin->explorer_selected < fwin->explorer_scroll)
							fwin->explorer_scroll = fwin->explorer_selected;
						need_redraw = 1;
					}
					else if (sc == 0x51) /* Page Down */
					{
						unsigned int ew_fh = screen_fb_cell_h();
						int bar_h = (int)ew_fh + 4;
						int list_h = fwin->h - GUI_BORDER_W * 2 - GUI_TITLEBAR_H - bar_h;
						int vis = list_h / GUI_EXPLORER_ITEM_H;
						int has_par = (fwin->filepath[0] != '\0' &&
						               !(fwin->filepath[0] == '/' && fwin->filepath[1] == '\0'));
						if (has_par) vis--;
						fwin->explorer_selected += vis;
						if (fwin->explorer_selected >= fwin->explorer_count)
							fwin->explorer_selected = fwin->explorer_count - 1;
						if (fwin->explorer_selected >= fwin->explorer_scroll + vis)
							fwin->explorer_scroll = fwin->explorer_selected - vis + 1;
						need_redraw = 1;
					}
					else if (sc == 0x47) /* Home */
					{
						int has_par = (fwin->filepath[0] != '\0' &&
						               !(fwin->filepath[0] == '/' && fwin->filepath[1] == '\0'));
						fwin->explorer_selected = has_par ? -1 : 0;
						fwin->explorer_scroll = 0;
						need_redraw = 1;
					}
					else if (sc == 0x4F) /* End */
					{
						unsigned int ew_fh = screen_fb_cell_h();
						int bar_h = (int)ew_fh + 4;
						int list_h = fwin->h - GUI_BORDER_W * 2 - GUI_TITLEBAR_H - bar_h;
						int vis = list_h / GUI_EXPLORER_ITEM_H;
						int has_par = (fwin->filepath[0] != '\0' &&
						               !(fwin->filepath[0] == '/' && fwin->filepath[1] == '\0'));
						if (has_par) vis--;
						fwin->explorer_selected = fwin->explorer_count - 1;
						if (fwin->explorer_selected >= fwin->explorer_scroll + vis)
							fwin->explorer_scroll = fwin->explorer_selected - vis + 1;
						need_redraw = 1;
					}
					else if (sc == 0x1C) /* Enter */
					{
						if (fwin->explorer_selected == -1)
							gui_explorer_go_up(fwin);
						else
							gui_explorer_open_selected(fwin);
						need_redraw = 1;
					}
					else if (sc == 0x0E) /* Backspace — go up */
					{
						gui_explorer_go_up(fwin);
						need_redraw = 1;
					}
					continue;
				}

				/* ── TERMINAL window keyboard handling (original) ── */
				{
					char c = translate_scancode(sc);
					if (sc == 0x1C) /* Enter */
					{
						gui_win_putc(gui_focused, '\n');
						{
							char cmd[128];
							int ci = 0, ti;
							int row = (fwin->text_col == 0 && fwin->text_row > 0) ? fwin->text_row - 1 : fwin->text_row;
							int off = row * fwin->text_cols;
							ti = off;
							while (ti < off + fwin->text_cols && fwin->text[ti] == '>' ) ti++;
							while (ti < off + fwin->text_cols && fwin->text[ti] == ' ') ti++;
							while (ti < off + fwin->text_cols && fwin->text[ti] != ' ' && fwin->text[ti] != '\0' && ci < 126)
								cmd[ci++] = fwin->text[ti++];
							while (ti < off + fwin->text_cols && ci < 126)
							{
								if (fwin->text[ti] == ' ' && (ti + 1 >= off + fwin->text_cols || fwin->text[ti + 1] == ' '))
									break;
								cmd[ci++] = fwin->text[ti++];
							}
							cmd[ci] = '\0';
							while (ci > 0 && cmd[ci - 1] == ' ') cmd[--ci] = '\0';

							if (string_equals(cmd, "exit") || string_equals(cmd, "quit"))
							{
								gui_close_window(gui_focused);
								need_redraw = 1;
							}
							else if (string_equals(cmd, "clear") || string_equals(cmd, "cls"))
							{
								int ci2;
								for (ci2 = 0; ci2 < fwin->text_cols * fwin->text_rows; ci2++)
								{
									fwin->text[ci2] = ' ';
									fwin->attrs[ci2] = 0x0F;
								}
								fwin->text_col = 0;
								fwin->text_row = 0;
								need_redraw = 1;
							}
							else if (ci > 0)
							{
								unsigned long ki = 0;
								while (cmd[ki] && ki + 1 < INPUT_BUFFER_SIZE) { input_buffer[ki] = cmd[ki]; ki++; }
								input_buffer[ki] = '\0';
								input_length = ki;

								terminal_output_capture = 1;
								{
									static char gui_capture_buf[4096];
									terminal_output_buf = gui_capture_buf;
									terminal_output_buf_size = sizeof(gui_capture_buf);
									terminal_output_buf_len = 0;
								}
								suppress_prompt = 1;
								run_command_dispatch();
								suppress_prompt = 0;
								terminal_output_capture = 0;
								{
									unsigned long oi;
									for (oi = 0; oi < terminal_output_buf_len; oi++)
										gui_win_putc(gui_focused, terminal_output_buf[oi]);
								}
								terminal_output_buf = (void *)0;
								terminal_output_buf_len = 0;
							}
						}
						if (gui_focused >= 0 && gui_focused < gui_window_count)
							gui_win_write(gui_focused, "> ");
						need_redraw = 1;
					}
					else if (sc == 0x0E) /* Backspace */
					{
						if (fwin->text_col > 2)
						{
							fwin->text_col--;
							fwin->text[fwin->text_row * fwin->text_cols + fwin->text_col] = ' ';
							need_redraw = 1;
						}
					}
					else if (c != '\0')
					{
						gui_win_putc(gui_focused, c);
						need_redraw = 1;
					}
				}
			}
		}

		/* Update clock every ~100 ticks (1 second) */
		{
			unsigned long now = timer_ticks();
			if (now - last_clock_tick >= 100)
			{
				last_clock_tick = now;
				gui_restore_under_cursor(gui_mouse_x, gui_mouse_y);
				gui_draw_taskbar_clock();
				gui_draw_cursor(gui_mouse_x, gui_mouse_y);
			}
		}

		if (need_redraw)
		{
			gui_restore_under_cursor(gui_mouse_x, gui_mouse_y);
			gui_redraw_all();
			need_redraw = 0;
		}

		/* Keep network stack alive */
		net_poll();

		/* Idle until next interrupt (keyboard, mouse, or timer at 100 Hz) */
		__asm__ volatile("sti; hlt");
	}
}

static void cmd_gui(const char *args)
{
	(void)args;
	if (!screen_fb_is_active())
	{
		terminal_write_line("gui: framebuffer not active (run 'display fb' first)");
		return;
	}

	gui_active = 1;
	gui_window_count = 0;
	gui_focused = -1;
	gui_drag_window = -1;
	gui_resize_window = -1;
	gui_theme_menu_open = 0;
	gui_paint_buf_owner = -1;

	/* Load any saved custom themes */
	gui_load_custom_themes();

	/* Create initial terminal window */
	{
		int widx = gui_create_terminal_window();
		if (widx >= 0)
		{
			gui_win_write_line(widx, "TG11-OS Desktop");
			gui_win_write_line(widx, "Type commands. 'exit' to close window. Esc to leave GUI.");
			gui_win_write(widx, "> ");
		}
	}

	gui_event_loop();

	/* Re-enable preemptive scheduling */
	task_set_preemption(1);

	/* Restore text mode display */
	screen_clear();
	terminal_write_line("Returned to text mode.");
	terminal_prompt();
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
		if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0)
		{
			terminal_write_line("write: failed (bad path)");
			return;
		}
		if (fat32_write_file_path(full_path, (const unsigned char *)p, string_length(p)) != 0)
		{
			terminal_write_fat_failure("write: failed");
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
		int src_drive, dst_drive;
		if (fat_resolve_path(src, src_full, sizeof(src_full)) != 0)
		{
			terminal_write_line("cp: failed");
			return;
		}
		src_drive = fat_mounted_drive_index;
		if (fat_resolve_path(dst, dst_full, sizeof(dst_full)) != 0)
		{
			terminal_write_line("cp: failed");
			return;
		}
		dst_drive = fat_mounted_drive_index;

		if (src_drive >= 0 && dst_drive >= 0 && src_drive != dst_drive)
		{
			/* Cross-drive copy: read from src_drive then write to dst_drive */
			unsigned char xbuf[FS_MAX_FILE_SIZE];
			unsigned long xsize;
			char xtarget[128];
			unsigned long j;

			if (recursive)
			{
				terminal_write_line("cp: cross-drive recursive copy not supported");
				return;
			}
			/* Determine actual destination path (directory check needs dst drive mounted) */
			if (fat_mount_drive_now(dst_drive, 1) != 0)
			{
				terminal_write_line("cp: failed");
				return;
			}
			if (fat_path_is_dir(dst_full))
			{
				if (rm_path_join(xtarget, sizeof(xtarget), dst_full, path_basename_part(src_full)) != 0)
				{
					terminal_write_line("cp: failed");
					return;
				}
			}
			else
			{
				j = 0;
				while (dst_full[j] != '\0' && j + 1 < sizeof(xtarget)) { xtarget[j] = dst_full[j]; j++; }
				xtarget[j] = '\0';
			}
			if (no_clobber && fat_path_exists(xtarget)) return;
			if (interactive && fat_path_exists(xtarget) && !prompt_overwrite("cp", xtarget)) return;

			/* Read source file from src drive */
			if (fat_mount_drive_now(src_drive, 1) != 0)
			{
				terminal_write_line("cp: failed");
				return;
			}
			if (fat32_read_file_path(src_full, xbuf, sizeof(xbuf), &xsize) != 0)
			{
				terminal_write_line("cp: failed (read error or file too large)");
				return;
			}

			/* Write to destination drive */
			if (fat_mount_drive_now(dst_drive, 1) != 0)
			{
				terminal_write_line("cp: failed");
				return;
			}
			if (fat_path_exists(xtarget) && rm_fat_recursive(xtarget, 1, 0) != 0)
			{
				terminal_write_line("cp: failed");
				return;
			}
			if (fat32_write_file_path(xtarget, xbuf, xsize) != 0)
				terminal_write_line("cp: failed (write error)");
			return;
		}

		/* Same drive — ensure it's mounted, then do normal copy */
		if (src_drive >= 0 && fat_mounted_drive_index != src_drive)
			fat_mount_drive_now(src_drive, 1);
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

static void terminal_set_script_error(unsigned long line_no, const char *msg)
{
	unsigned long i = 0;
	script_last_error = 1;
	script_last_error_line = line_no;
	if (msg == (void *)0) msg = "error";
	while (msg[i] != '\0' && i + 1 < sizeof(script_last_error_text))
	{
		script_last_error_text[i] = msg[i];
		i++;
	}
	script_last_error_text[i] = '\0';
}

static int terminal_script_command_known(const char *line)
{
	char token[32];
	const char *p;
	p = read_token(line, token, sizeof(token));
	if (p == (void *)0 || token[0] == '\0') return 1;
	if (command_is_builtin_name(token)) return 1;
	if (command_alias_lookup(token) != (void *)0) return 1;
	return 0;
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

static int case_pattern_match(const char *value, const char *pattern)
{
	unsigned long vlen = string_length(value);
	unsigned long plen = string_length(pattern);
	if (plen == 1 && pattern[0] == '*') return 1;
	if (plen > 1 && pattern[0] == '*')
	{
		const char *suffix = pattern + 1;
		unsigned long slen = plen - 1;
		if (vlen >= slen && string_equals(value + vlen - slen, suffix)) return 1;
		return 0;
	}
	if (plen > 1 && pattern[plen - 1] == '*')
	{
		unsigned long pi;
		if (vlen < plen - 1) return 0;
		for (pi = 0; pi + 1 < plen; pi++) { if (value[pi] != pattern[pi]) return 0; }
		return 1;
	}
	return string_equals(value, pattern);
}

static int case_pattern_match_multi(const char *value, const char *patterns)
{
	char pat[64];
	unsigned long pi = 0;
	unsigned long i = 0;
	while (patterns[i] != '\0')
	{
		if (patterns[i] == '|')
		{
			pat[pi] = '\0';
			trim_spaces_inplace(pat);
			if (pat[0] != '\0' && case_pattern_match(value, pat)) return 1;
			pi = 0; i++; continue;
		}
		if (pi + 1 < sizeof(pat)) pat[pi++] = patterns[i];
		i++;
	}
	pat[pi] = '\0';
	trim_spaces_inplace(pat);
	if (pat[0] != '\0' && case_pattern_match(value, pat)) return 1;
	return 0;
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

	/* Numeric comparisons */
	{
		unsigned int lv, rv;
		int l_neg = 0, r_neg = 0;
		const char *lp = lhs, *rp = rhs;
		if (*lp == '-') { l_neg = 1; lp++; }
		if (*rp == '-') { r_neg = 1; rp++; }
		if (parse_dec_u32(lp, &lv) == 0 && parse_dec_u32(rp, &rv) == 0)
		{
			long lval = l_neg ? -(long)lv : (long)lv;
			long rval = r_neg ? -(long)rv : (long)rv;
			if (string_equals(op, "<"))  return lval < rval;
			if (string_equals(op, ">"))  return lval > rval;
			if (string_equals(op, "<=")) return lval <= rval;
			if (string_equals(op, ">=")) return lval >= rval;
		}
	}

	return 0;
}

static int terminal_try_mount_boot_fat(void)
{
	struct block_device *dev;
	if (fat32_is_mounted()) return 1;
	dev = blockdev_get_primary();
	if (dev != (void *)0 && dev->present && fat32_mount(dev) == 0)
	{
		fat_mounted_drive_index = 0;
		fat_active_drive_index = 0;
		fat_registered_drive_mask |= (1U << 0);
		fat_drive_cwd[0][0] = '/';
		fat_drive_cwd[0][1] = '\0';
		return 1;
	}
	dev = blockdev_get_secondary();
	if (dev != (void *)0 && dev->present && fat32_mount(dev) == 0)
	{
		fat_mounted_drive_index = 1;
		fat_active_drive_index = 1;
		fat_registered_drive_mask |= (1U << 1);
		fat_drive_cwd[1][0] = '/';
		fat_drive_cwd[1][1] = '\0';
		return 1;
	}
	return 0;
}

static void terminal_warn_if_fat_mount_suspicious(void)
{
	struct block_device *primary;
	struct block_device *secondary;
	if (!fat32_is_mounted()) return;
	if (fat_mounted_drive_index != 0) return;

	primary = blockdev_get_primary();
	secondary = blockdev_get_secondary();
	if (primary == (void *)0 || !primary->present) return;
	if (secondary == (void *)0 || !secondary->present) return;

	terminal_write_line("warning: FAT mounted on drive 0 while drive 1 is present");
	terminal_write_line("         (run-disk usually expects data on drive 1)");
}

static void terminal_auto_fatmount(void)
{
	if (!terminal_try_mount_boot_fat()) return;
	vfs_prefer_fat_root = 1;
	fat_cwd[0] = '/';
	fat_cwd[1] = '\0';
	terminal_warn_if_fat_mount_suspicious();
}

static int terminal_write_fat_text(const char *path, const char *text)
{
	unsigned long len = string_length(text);
	if (fat32_write_file_path(path, (const unsigned char *)text, len) == 0) return 0;
	if (fat32_touch_file_path(path) != 0) return -1;
	return fat32_write_file_path(path, (const unsigned char *)text, len);
}

static int terminal_read_config_text(const char *fat_path, const char *ram_path, const char **ram_text, unsigned char *fat_buf, unsigned long fat_buf_size, const char **out_text)
{
	unsigned long fat_size = 0;
	if (out_text == (void *)0) return -1;
	if (terminal_try_mount_boot_fat() && fat_path != (void *)0 && fat_buf != (void *)0 && fat_buf_size > 1 &&
		fat32_read_file_path(fat_path, fat_buf, fat_buf_size - 1, &fat_size) == 0)
	{
		fat_buf[fat_size] = '\0';
		*out_text = (const char *)fat_buf;
		return 0;
	}
	if (ram_path != (void *)0 && ram_text != (void *)0 && fs_read_text(ram_path, ram_text) == 0)
	{
		*out_text = *ram_text;
		return 0;
	}
	return -1;
}

static void terminal_write_config_text(const char *fat_path, const char *ram_path, const char *text)
{
	if (ram_path != (void *)0) fs_write_text(ram_path, text);
	if (fat_path != (void *)0 && terminal_try_mount_boot_fat()) terminal_write_fat_text(fat_path, text);
}

static int terminal_get_autorun_mode(void)
{
	const char *text = (void *)0;
	const char *ram_text = (void *)0;
	unsigned char fat_buf[32];
	if (terminal_read_config_text(FAT_AUTORUN_MODE_PATH, AUTORUN_MODE_PATH, &ram_text, fat_buf, sizeof(fat_buf), &text) != 0)
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
	terminal_write_config_text(FAT_AUTORUN_MODE_PATH, AUTORUN_MODE_PATH, text);
}

static int terminal_autorun_once_done(void)
{
	const char *text = (void *)0;
	const char *ram_text = (void *)0;
	unsigned char fat_buf[8];
	if (terminal_read_config_text(FAT_AUTORUN_ONCE_STATE_PATH, AUTORUN_ONCE_STATE_PATH, &ram_text, fat_buf, sizeof(fat_buf), &text) != 0)
	{
		return 0;
	}

	return (text[0] == '1') ? 1 : 0;
}

static void terminal_set_autorun_once_done(int done)
{
	const char *text = done ? "1\n" : "0\n";
	terminal_write_config_text(FAT_AUTORUN_ONCE_STATE_PATH, AUTORUN_ONCE_STATE_PATH, text);
}

static unsigned long terminal_get_autorun_delay_seconds(void)
{
	const char *text = (void *)0;
	const char *ram_text = (void *)0;
	char token[16];
	unsigned int seconds = (unsigned int)AUTORUN_DEFAULT_DELAY_SECONDS;
	unsigned char fat_buf[32];

	if (terminal_read_config_text(FAT_AUTORUN_DELAY_PATH, AUTORUN_DELAY_PATH, &ram_text, fat_buf, sizeof(fat_buf), &text) == 0 &&
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
	terminal_write_config_text(FAT_AUTORUN_DELAY_PATH, AUTORUN_DELAY_PATH, value);
}

static int terminal_get_autorun_safe_mode(void)
{
	const char *text = (void *)0;
	const char *ram_text = (void *)0;
	unsigned char fat_buf[16];
	if (terminal_read_config_text(FAT_AUTORUN_SAFE_MODE_PATH, AUTORUN_SAFE_MODE_PATH, &ram_text, fat_buf, sizeof(fat_buf), &text) != 0)
	{
		return 1;
	}
	if (string_starts_with(text, "off") || text[0] == '0') return 0;
	return 1;
}

static void terminal_set_autorun_safe_mode(int enabled)
{
	terminal_write_config_text(FAT_AUTORUN_SAFE_MODE_PATH, AUTORUN_SAFE_MODE_PATH, enabled ? "on\n" : "off\n");
}

static int terminal_get_autorun_clean_flag(void)
{
	const char *text = (void *)0;
	const char *ram_text = (void *)0;
	unsigned char fat_buf[16];
	if (terminal_read_config_text(FAT_AUTORUN_CLEAN_PATH, AUTORUN_CLEAN_PATH, &ram_text, fat_buf, sizeof(fat_buf), &text) != 0)
	{
		return 1;
	}
	return (text[0] == '1') ? 1 : 0;
}

static void terminal_set_autorun_clean_flag(int clean)
{
	terminal_write_config_text(FAT_AUTORUN_CLEAN_PATH, AUTORUN_CLEAN_PATH, clean ? "1\n" : "0\n");
}

static void terminal_get_autorun_last_status(char *out, unsigned long out_size)
{
	const char *text = (void *)0;
	const char *ram_text = (void *)0;
	unsigned char fat_buf[48];
	unsigned long i = 0;
	if (out_size == 0) return;
	if (terminal_read_config_text(FAT_AUTORUN_LAST_STATUS_PATH, AUTORUN_LAST_STATUS_PATH, &ram_text, fat_buf, sizeof(fat_buf), &text) != 0)
	{
		text = "idle";
	}
	while (text[i] != '\0' && text[i] != '\n' && i + 1 < out_size)
	{
		out[i] = text[i];
		i++;
	}
	out[i] = '\0';
}

static void terminal_set_autorun_last_status(const char *status)
{
	terminal_write_config_text(FAT_AUTORUN_LAST_STATUS_PATH, AUTORUN_LAST_STATUS_PATH, status);
}

static void terminal_get_autorun_last_source(char *out, unsigned long out_size)
{
	const char *text = (void *)0;
	const char *ram_text = (void *)0;
	unsigned char fat_buf[48];
	unsigned long i = 0;
	if (out_size == 0) return;
	if (terminal_read_config_text(FAT_AUTORUN_LAST_SOURCE_PATH, AUTORUN_LAST_SOURCE_PATH, &ram_text, fat_buf, sizeof(fat_buf), &text) != 0)
	{
		text = "none";
	}
	while (text[i] != '\0' && text[i] != '\n' && i + 1 < out_size)
	{
		out[i] = text[i];
		i++;
	}
	out[i] = '\0';
}

static void terminal_set_autorun_last_source(const char *source)
{
	terminal_write_config_text(FAT_AUTORUN_LAST_SOURCE_PATH, AUTORUN_LAST_SOURCE_PATH, source);
}

static void terminal_autorun_boot_begin(void)
{
	int safe_mode = terminal_get_autorun_safe_mode();
	int previous_clean = terminal_get_autorun_clean_flag();
	autorun_boot_started_at = timer_ticks();
	autorun_boot_clean_marked = 0;
	autorun_safe_latched = (safe_mode && !previous_clean) ? 1 : 0;
	terminal_set_autorun_clean_flag(0);
	terminal_set_autorun_last_source("none\n");
	if (autorun_safe_latched) terminal_set_autorun_last_status("skipped-safe\n");
	else terminal_set_autorun_last_status("armed\n");
}

static void terminal_autorun_boot_heartbeat(void)
{
	unsigned long now = timer_ticks();
	if (autorun_boot_clean_marked) return;
	if (now - autorun_boot_started_at < AUTORUN_SAFE_BOOT_WINDOW_TICKS) return;
	autorun_boot_clean_marked = 1;
	terminal_set_autorun_clean_flag(1);
}

static int terminal_run_autorun_script_now(void)
{
	const char *text;
	int old_fat_mode;

	script_last_error = 0;
	script_last_error_line = 0;
	script_last_error_text[0] = '\0';

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
			terminal_set_autorun_last_source("/autorun.sh\n");
			if (script_last_error) return -1;
			return 1;
		}
	}

	if (fs_read_text(AUTORUN_PATH, &text) != 0) return 0;
	if (text[0] == '\0') return 0;
	cmd_run(AUTORUN_PATH);
	terminal_set_autorun_last_source("/etc/autorun.sh\n");
	if (script_last_error) return -1;
	return 1;
}

static int terminal_autorun_should_run_on_boot(void)
{
	int mode = terminal_get_autorun_mode();
	if (mode == 0) return 0;
	if (mode == 2 && terminal_autorun_once_done()) return 0;
	if (autorun_safe_latched) return 0;
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
		if (autorun_safe_latched)
		{
			terminal_write_line("autorun: skipped (safe mode; previous boot was not marked clean)");
		}
		return;
	}
	delay_seconds = terminal_get_autorun_delay_seconds();
	delay_ticks = delay_seconds * 100UL;
	autorun_boot_pending = 1;
	autorun_boot_deadline = timer_ticks() + delay_ticks;
	terminal_set_autorun_last_status("scheduled\n");
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
	if (ran == 0)
	{
		terminal_set_autorun_last_status("no-script\n");
		terminal_set_autorun_last_source("none\n");
		terminal_write_line("autorun: no script found");
		return;
	}
	if (ran < 0)
	{
		terminal_set_autorun_last_status("error\n");
		terminal_write_line("autorun: script failed");
		return;
	}
	terminal_set_autorun_last_status("executed\n");
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
	struct run_while_state
	{
		unsigned long body_start;
		unsigned long body_line_no;
		char condition[128];
		int is_until;
		int parent_exec;
		int executing;
		unsigned long iterations;
	};
	struct run_case_state
	{
		char value[96];
		int parent_exec;
		int branch_matched;
		int executing;
	};
	struct run_if_state if_stack[8];
	struct run_while_state while_stack[8];
	struct run_case_state case_stack[8];
	int if_depth = 0;
	int while_depth = 0;
	int case_depth = 0;
	int current_exec = 1;
	int trace = 0;
	int old_script_var_count;
	char old_script_var_names[SCRIPT_VAR_MAX][16];
	char old_script_var_values[SCRIPT_VAR_MAX][96];
	char path[128];
	char resolved[128];
	char script[EDITOR_BUFFER_SIZE];
	char tok[32];
	unsigned long script_len = 0;
	unsigned long timeout_ticks = SCRIPT_DEFAULT_TIMEOUT_TICKS;
	unsigned long start_ticks;
	unsigned long line_no = 1;
	unsigned long i;
	unsigned long line_len = 0;
	char line[INPUT_BUFFER_SIZE];
	const char *p = args;

	path[0] = '\0';
	script_last_error = 0;
	script_last_error_line = 0;
	script_last_error_text[0] = '\0';

	for (;;)
	{
		p = read_token(p, tok, sizeof(tok));
		if (p == (void *)0 || tok[0] == '\0') break;
		if (string_equals(tok, "-x"))
		{
			trace = 1;
			continue;
		}
		if (string_equals(tok, "-t"))
		{
			unsigned int seconds;
			char sec_tok[16];
			p = read_token(p, sec_tok, sizeof(sec_tok));
			if (p == (void *)0 || sec_tok[0] == '\0' || parse_dec_u32(sec_tok, &seconds) != 0 || seconds > 3600U)
			{
				terminal_write_line("Usage: run [-x] [-t <1..3600>] <path>");
				terminal_set_script_error(0, "bad timeout");
				return;
			}
			timeout_ticks = (unsigned long)seconds * 100UL;
			continue;
		}
		{
			unsigned long c = 0;
			while (tok[c] != '\0' && c + 1 < sizeof(path)) { path[c] = tok[c]; c++; }
			path[c] = '\0';
		}
		break;
	}

	if (path[0] == '\0')
	{
		terminal_write_line("Usage: run [-x] [-t <1..3600>] <path>");
		terminal_set_script_error(0, "missing path");
		return;
	}

	if (script_depth >= 3)
	{
		terminal_write_line("run: max script depth reached");
		terminal_set_script_error(0, "max depth reached");
		return;
	}

	if (!run_source_mode)
	{
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
	}

	if (fat_mode_active())
	{
		if (fat_resolve_path(path, resolved, sizeof(resolved)) != 0)
		{
			terminal_write_line("run: invalid FAT path");
			terminal_set_script_error(0, "invalid FAT path");
			return;
		}
		if (fat32_read_file_path(resolved, (unsigned char *)script, sizeof(script) - 1, &script_len) != 0)
		{
			terminal_write_line("run: read failed");
			terminal_set_script_error(0, "read failed");
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
			terminal_set_script_error(0, "read failed");
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
	start_ticks = timer_ticks();

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
		terminal_poll_control_hotkeys();
		if (terminal_take_cancel_request())
		{
			terminal_set_script_error(line_no, "canceled");
			terminal_write("run:");
			{
				char ln[16];
				uint_to_dec(line_no, ln, sizeof(ln));
				terminal_write(ln);
			}
			terminal_write_line(": canceled");
			line_len = 0;
			break;
		}
		if (timer_ticks() - start_ticks > timeout_ticks)
		{
			terminal_set_script_error(line_no, "timeout");
			terminal_write("run:");
			{
				char ln[16];
				uint_to_dec(line_no, ln, sizeof(ln));
				terminal_write(ln);
			}
			terminal_write_line(": timeout");
			line_len = 0;
			break;
		}
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
						terminal_set_script_error(line_no, "if nesting too deep");
						terminal_write("run:");
						{
							char ln[16];
							uint_to_dec(line_no, ln, sizeof(ln));
							terminal_write(ln);
						}
						terminal_write_line(": if nesting too deep");
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
						terminal_set_script_error(line_no, "elif without if");
						terminal_write("run:");
						{
							char ln[16];
							uint_to_dec(line_no, ln, sizeof(ln));
							terminal_write(ln);
						}
						terminal_write_line(": elif without if");
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
						terminal_set_script_error(line_no, "else without if");
						terminal_write("run:");
						{
							char ln[16];
							uint_to_dec(line_no, ln, sizeof(ln));
							terminal_write(ln);
						}
						terminal_write_line(": else without if");
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
						terminal_set_script_error(line_no, "bad foreach syntax");
						terminal_write("run:");
						{
							char ln[16];
							uint_to_dec(line_no, ln, sizeof(ln));
							terminal_write(ln);
						}
						terminal_write_line(": bad foreach syntax");
						line_len = 0;
						continue;
					}
					rest = read_token(rest, items, sizeof(items));
					if (rest == (void *)0 || !string_equals(items, "in"))
					{
						terminal_set_script_error(line_no, "bad foreach syntax");
						terminal_write("run:");
						{
							char ln[16];
							uint_to_dec(line_no, ln, sizeof(ln));
							terminal_write(ln);
						}
						terminal_write_line(": use 'foreach <var> in <a,b,..> do <cmd>'");
						line_len = 0;
						continue;
					}
					rest = read_token(rest, items, sizeof(items));
					if (rest == (void *)0 || items[0] == '\0')
					{
						terminal_set_script_error(line_no, "missing foreach items");
						terminal_write("run:");
						{
							char ln[16];
							uint_to_dec(line_no, ln, sizeof(ln));
							terminal_write(ln);
						}
						terminal_write_line(": missing foreach items");
						line_len = 0;
						continue;
					}
					rest = read_token(rest, body, sizeof(body));
					if (rest == (void *)0 || !string_equals(body, "do"))
					{
						terminal_set_script_error(line_no, "missing 'do' in foreach");
						terminal_write("run:");
						{
							char ln[16];
							uint_to_dec(line_no, ln, sizeof(ln));
							terminal_write(ln);
						}
						terminal_write_line(": missing 'do' in foreach");
						line_len = 0;
						continue;
					}
					rest = skip_spaces(rest);
					if (*rest == '\0')
					{
						terminal_set_script_error(line_no, "missing foreach body");
						terminal_write("run:");
						{
							char ln[16];
							uint_to_dec(line_no, ln, sizeof(ln));
							terminal_write(ln);
						}
						terminal_write_line(": missing foreach body command");
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
				else if (string_starts_with(cmd, "while ") || string_starts_with(cmd, "until "))
				{
					int is_until = string_starts_with(cmd, "until ");
					if (while_depth >= 8)
					{
						terminal_set_script_error(line_no, "while nesting too deep");
						terminal_write("run:");
						{
							char ln[16];
							uint_to_dec(line_no, ln, sizeof(ln));
							terminal_write(ln);
						}
						terminal_write_line(": while/until nesting too deep");
						line_len = 0;
						break;
					}
					while_stack[while_depth].is_until = is_until;
					while_stack[while_depth].body_start = i + 1;
					while_stack[while_depth].body_line_no = line_no + 1;
					while_stack[while_depth].iterations = 0;
					while_stack[while_depth].parent_exec = current_exec;
					{
						const char *cond_str = cmd + 6;
						unsigned long ci = 0;
						while (cond_str[ci] != '\0' && ci + 1 < sizeof(while_stack[while_depth].condition))
						{
							while_stack[while_depth].condition[ci] = cond_str[ci];
							ci++;
						}
						while_stack[while_depth].condition[ci] = '\0';
					}
					if (current_exec)
					{
						int cond = eval_script_condition(while_stack[while_depth].condition);
						if (is_until) cond = !cond;
						while_stack[while_depth].executing = cond;
					}
					else
					{
						while_stack[while_depth].executing = 0;
					}
					current_exec = while_stack[while_depth].executing;
					while_depth++;
				}
				else if (string_equals(cmd, "done"))
				{
					if (while_depth == 0)
					{
						terminal_set_script_error(line_no, "done without while");
						terminal_write("run:");
						{
							char ln[16];
							uint_to_dec(line_no, ln, sizeof(ln));
							terminal_write(ln);
						}
						terminal_write_line(": done without while/until");
					}
					else
					{
						struct run_while_state *st = &while_stack[while_depth - 1];
						if (st->parent_exec && st->executing)
						{
							int cond;
							st->iterations++;
							if (st->iterations > 10000)
							{
								terminal_set_script_error(line_no, "while iteration limit");
								terminal_write("run:");
								{
									char ln[16];
									uint_to_dec(line_no, ln, sizeof(ln));
									terminal_write(ln);
								}
								terminal_write_line(": while loop exceeded 10000 iterations");
								while_depth--;
								current_exec = st->parent_exec;
								line_len = 0;
								continue;
							}
							cond = eval_script_condition(st->condition);
							if (st->is_until) cond = !cond;
							if (cond)
							{
								i = st->body_start - 1;
								line_no = st->body_line_no - 1;
								line_len = 0;
								continue;
							}
						}
						while_depth--;
						current_exec = (while_depth > 0) ? while_stack[while_depth - 1].executing : ((if_depth > 0) ? if_stack[if_depth - 1].executing : 1);
					}
				}
				else if (string_equals(cmd, "break"))
				{
					if (while_depth == 0)
					{
						terminal_set_script_error(line_no, "break outside loop");
						terminal_write("run:");
						{
							char ln[16];
							uint_to_dec(line_no, ln, sizeof(ln));
							terminal_write(ln);
						}
						terminal_write_line(": break outside while/until");
					}
					else if (current_exec)
					{
						while_stack[while_depth - 1].executing = 0;
						current_exec = 0;
					}
				}
				else if (string_equals(cmd, "continue"))
				{
					if (while_depth == 0)
					{
						terminal_set_script_error(line_no, "continue outside loop");
						terminal_write("run:");
						{
							char ln[16];
							uint_to_dec(line_no, ln, sizeof(ln));
							terminal_write(ln);
						}
						terminal_write_line(": continue outside while/until");
					}
					else if (current_exec)
					{
						struct run_while_state *st = &while_stack[while_depth - 1];
						int cond;
						st->iterations++;
						if (st->iterations > 10000)
						{
							terminal_set_script_error(line_no, "while iteration limit");
							terminal_write("run:");
							{
								char ln[16];
								uint_to_dec(line_no, ln, sizeof(ln));
								terminal_write(ln);
							}
							terminal_write_line(": while loop exceeded 10000 iterations");
							while_depth--;
							current_exec = st->parent_exec;
							line_len = 0;
							continue;
						}
						cond = eval_script_condition(st->condition);
						if (st->is_until) cond = !cond;
						if (cond)
						{
							i = st->body_start - 1;
							line_no = st->body_line_no - 1;
							line_len = 0;
							continue;
						}
						else
						{
							st->executing = 0;
							current_exec = 0;
						}
					}
				}
				else if (string_starts_with(cmd, "case "))
				{
					if (case_depth >= 8)
					{
						terminal_set_script_error(line_no, "case nesting too deep");
						terminal_write("run:");
						{
							char ln[16];
							uint_to_dec(line_no, ln, sizeof(ln));
							terminal_write(ln);
						}
						terminal_write_line(": case nesting too deep");
						line_len = 0;
						break;
					}
					case_stack[case_depth].parent_exec = current_exec;
					case_stack[case_depth].branch_matched = 0;
					case_stack[case_depth].executing = 0;
					case_stack[case_depth].value[0] = '\0';
					if (current_exec)
					{
						char val_raw[96];
						char val_res[96];
						const char *rest = cmd + 5;
						char in_tok[16];
						rest = read_token(rest, val_raw, sizeof(val_raw));
						if (rest != (void *)0)
						{
							rest = read_token(rest, in_tok, sizeof(in_tok));
						}
						if (resolve_script_value(val_raw, val_res, sizeof(val_res)) == 0)
						{
							unsigned long ci = 0;
							while (val_res[ci] != '\0' && ci + 1 < sizeof(case_stack[case_depth].value))
							{
								case_stack[case_depth].value[ci] = val_res[ci];
								ci++;
							}
							case_stack[case_depth].value[ci] = '\0';
						}
					}
					current_exec = 0;
					case_depth++;
				}
				else if (string_equals(cmd, "esac"))
				{
					if (case_depth == 0)
					{
						terminal_set_script_error(line_no, "esac without case");
						terminal_write("run:");
						{
							char ln[16];
							uint_to_dec(line_no, ln, sizeof(ln));
							terminal_write(ln);
						}
						terminal_write_line(": esac without case");
					}
					else
					{
						case_depth--;
						current_exec = case_stack[case_depth].parent_exec;
					}
				}
				else if (string_equals(cmd, ";;"))
				{
					if (case_depth > 0)
					{
						struct run_case_state *cs = &case_stack[case_depth - 1];
						if (cs->executing)
						{
							cs->branch_matched = 1;
							cs->executing = 0;
						}
						current_exec = 0;
					}
				}
				else if (string_equals(cmd, "fi"))
				{
					if (if_depth == 0)
					{
						terminal_set_script_error(line_no, "fi without if");
						terminal_write("run:");
						{
							char ln[16];
							uint_to_dec(line_no, ln, sizeof(ln));
							terminal_write(ln);
						}
						terminal_write_line(": fi without if");
					}
					else
					{
						if_depth--;
						if (if_depth == 0)
							current_exec = (while_depth > 0) ? while_stack[while_depth - 1].executing : ((case_depth > 0) ? case_stack[case_depth - 1].executing : 1);
						else current_exec = if_stack[if_depth - 1].executing;
					}
				}
				else
				{
					if (case_depth > 0)
					{
						unsigned long cl = string_length(cmd);
						if (cl > 1 && cmd[cl - 1] == ')')
						{
							struct run_case_state *cs = &case_stack[case_depth - 1];
							if (cs->parent_exec && !cs->branch_matched)
							{
								char patterns[96];
								unsigned long pi = 0;
								while (pi + 1 < cl && pi + 1 < sizeof(patterns))
								{
									patterns[pi] = cmd[pi];
									pi++;
								}
								patterns[pi] = '\0';
								trim_spaces_inplace(patterns);
								if (case_pattern_match_multi(cs->value, patterns))
								{
									cs->executing = 1;
									current_exec = 1;
								}
								else
								{
									cs->executing = 0;
									current_exec = 0;
								}
							}
							else
							{
								cs->executing = 0;
								current_exec = 0;
							}
							line_len = 0;
							line_no++;
							continue;
						}
					}
					if (current_exec)
					{
						if (!terminal_script_command_known(cmd))
						{
							terminal_set_script_error(line_no, "unknown command");
							terminal_write("run:");
							{
								char ln[16];
								uint_to_dec(line_no, ln, sizeof(ln));
								terminal_write(ln);
							}
							terminal_write(" unknown command: ");
							terminal_write_line(cmd);
							line_len = 0;
							break;
						}
						if (trace)
						{
							terminal_write("+ ");
							terminal_write_line(cmd);
						}
						run_inline_command(cmd);
					}
				}
			}
		}
		line_len = 0;
		line_no++;
	}

	if (if_depth != 0)
	{
		terminal_set_script_error(line_no, "missing fi");
		terminal_write_line("run: missing fi");
	}
	if (while_depth != 0)
	{
		terminal_set_script_error(line_no, "missing done");
		terminal_write_line("run: missing done");
	}
	if (case_depth != 0)
	{
		terminal_set_script_error(line_no, "missing esac");
		terminal_write_line("run: missing esac");
	}

	script_depth--;
	if (script_depth == 0) script_mode_active = 0;

	if (!run_source_mode)
	{
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
	for (i = 0; i < ATA_MAX_DRIVES; i++)
	{
		if (ata_is_present_drive(i))
		{
			terminal_write("ATA drive ");
			terminal_putc((char)('0' + i));
			terminal_write(" (");
			terminal_write(ata_drive_label(i));
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
	int used_default = 0;
	char mount_tok[16];
	const char *tail;
	struct block_device *dev;

	if (parse_fat_drive_index_arg(args, &drive_index, &used_default) != 0)
	{
		terminal_write_line("fatmount: Usage: fatmount <0..3|hd#> [/hd#|A:]");
		return;
	}

	tail = read_token(args, mount_tok, sizeof(mount_tok));
	if (tail == (void *)0)
	{
		terminal_write_line("fatmount: Usage: fatmount <0..3|hd#> [/hd#|A:]");
		return;
	}
	tail = read_token(tail, mount_tok, sizeof(mount_tok));
	if (tail == (void *)0)
	{
		terminal_write_line("fatmount: mountpoint token too long");
		return;
	}
	if (mount_tok[0] != '\0')
	{
		if (parse_mountpoint_token(mount_tok, drive_index) != 0 || skip_spaces(tail)[0] != '\0')
		{
			terminal_write_line("fatmount: Usage: fatmount <0..3|hd#> [/hd#|A:]");
			return;
		}
	}

	if (used_default)
	{
		dev = blockdev_get_secondary();
		if (dev != (void *)0 && dev->present) drive_index = 1;
	}

	dev = blockdev_get(drive_index);
	if (dev == (void *)0 || !dev->present)
	{
		terminal_write("fatmount: drive ");
		terminal_putc((char)('0' + drive_index));
		terminal_write_line(" not present");
		return;
	}

	if (fat_mount_drive_now(drive_index, 1) != 0)
	{
		terminal_write("fatmount: drive ");
		terminal_putc((char)('0' + drive_index));
		terminal_write_line(" mount failed (not FAT32?)");
		return;
	}
	fat_registered_drive_mask |= (1U << (unsigned int)drive_index);
	if (fat_drive_cwd[drive_index][0] == '\0')
	{
		fat_drive_cwd[drive_index][0] = '/';
		fat_drive_cwd[drive_index][1] = '\0';
	}
	/* No explicit mount-point token means this is a primary mount; make it the active drive */
	if (mount_tok[0] == '\0')
		fat_active_drive_index = drive_index;

	terminal_write("fatmount: drive ");
	terminal_putc((char)('0' + drive_index));
	terminal_write(" registered as /hd");
	terminal_putc((char)('0' + drive_index));
	terminal_write(" and ");
	terminal_putc((char)('A' + drive_index));
	terminal_write_line(":");
	terminal_warn_if_fat_mount_suspicious();
}

static int parse_fat_drive_index_arg(const char *args, int *out_drive_index, int *out_used_default)
{
	char tok[16];
	const char *p;

	if (out_drive_index == (void *)0 || out_used_default == (void *)0) return -1;

	*out_drive_index = 0;
	*out_used_default = 1;

	if (args == (void *)0) return 0;
	p = read_token(args, tok, sizeof(tok));
	if (p == (void *)0) return -1;
	if (tok[0] == '\0') return 0;
	if (parse_mount_target_drive(tok, out_drive_index) == 0)
	{
		*out_used_default = 0;
		return 0;
	}
	return -1;
}

static const char *ata_drive_label(int drive_index)
{
	switch (drive_index)
	{
		case 0: return "primary master";
		case 1: return "primary slave";
		case 2: return "secondary master";
		case 3: return "secondary slave";
		default: return "unknown";
	}
}

static void cmd_fatwhere(void)
{
	int i;
	int any = 0;
	int active = (fat_active_drive_index >= 0) ? fat_active_drive_index : fat_mounted_drive_index;

	if (!fat32_is_mounted() && fat_active_drive_index < 0)
	{
		terminal_write_line("fatwhere: not mounted");
	}
	else
	{
		terminal_write("fatwhere: context drive=");
		if (active >= 0 && active <= 9)
		{
			terminal_putc((char)('0' + active));
		}
		else
		{
			terminal_write("?");
		}
		terminal_write(" cwd=");
		terminal_write((active >= 0 && active != fat_mounted_drive_index) ? fat_drive_cwd[active] : fat_cwd);
		if (fat_mounted_drive_index != active && fat32_is_mounted())
		{
			terminal_write(" (phys=hd");
			terminal_putc((char)('0' + fat_mounted_drive_index));
			terminal_putc(')');
		}
		terminal_write(" mode=");
		terminal_write_line(vfs_prefer_fat_root ? "FAT generic-on" : "FAT mounted-only");
	}

	terminal_write_line("fatwhere: registered mount roots:");
	for (i = 0; i < BLOCKDEV_MAX_DRIVES; i++)
	{
		if ((fat_registered_drive_mask & (1U << (unsigned int)i)) == 0U) continue;
		terminal_write(i == active ? "* /hd" : "  /hd");
		terminal_putc((char)('0' + i));
		terminal_write("  ");
		terminal_putc((char)('A' + i));
		terminal_write(":  cwd=");
		terminal_write_line(fat_drive_cwd[i]);
		any = 1;
	}
	if (!any) terminal_write_line("  (none)");
}

static void cmd_drives(void)
{
	int i;
	char num[16];
	for (i = 0; i < BLOCKDEV_MAX_DRIVES; i++)
	{
		struct block_device *dev = blockdev_get(i);
		terminal_write("  drive ");
		terminal_putc((char)('0' + i));
		terminal_write(" (");
		terminal_write(ata_drive_label(i));
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

static void cmd_fatunmount(const char *args)
{
	char tok[16];
	const char *p;
	int drive_index;
	int i;
	int fallback = -1;

	p = read_token(args, tok, sizeof(tok));
	if (p == (void *)0)
	{
		terminal_write_line("fatunmount: argument too long");
		return;
	}
	if (tok[0] != '\0')
	{
		if (parse_mount_target_drive(tok, &drive_index) != 0 || skip_spaces(p)[0] != '\0')
		{
			terminal_write_line("fatunmount: Usage: fatunmount [hd#|A:]");
			return;
		}
		fat_registered_drive_mask &= ~(1U << (unsigned int)drive_index);
		if (fat_mounted_drive_index == drive_index && fat32_is_mounted())
		{
			fat32_unmount();
			fat_mounted_drive_index = -1;
			for (i = 0; i < BLOCKDEV_MAX_DRIVES; i++)
			{
				if ((fat_registered_drive_mask & (1U << (unsigned int)i)) != 0U)
				{
					fallback = i;
					break;
				}
			}
			if (fallback >= 0 && fat_mount_drive_now(fallback, 1) != 0)
			{
				vfs_prefer_fat_root = 0;
			}
			else if (fallback < 0)
			{
				vfs_prefer_fat_root = 0;
				fat_cwd[0] = '/';
				fat_cwd[1] = '\0';
			}
		}
		/* Update active drive if we just removed it */
		if (drive_index == fat_active_drive_index)
			fat_active_drive_index = (fallback >= 0) ? fallback : -1;
		terminal_write("fatunmount: removed drive ");
		terminal_putc((char)('0' + drive_index));
		terminal_write_line(" mount root");
		return;
	}

	if (!fat32_is_mounted())
	{
		terminal_write_line("fatunmount: not mounted");
		return;
	}

	fat32_unmount();
	vfs_prefer_fat_root = 0;
	fat_mounted_drive_index = -1;
	fat_active_drive_index = -1;
	fat_registered_drive_mask = 0;
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
	if (*used_count >= RAMFS2FAT_USED_MAX) return;
	while (name[i] != '\0' && i + 1 < 16)
	{
		used[*used_count][i] = fat83_safe_upper(name[i]);
		i++;
	}
	used[*used_count][i] = '\0';
	(*used_count)++;
}

static void ramfs2fat_report_error(const char *reason, const char *src, const char *dst)
{
	const char *fat_reason = fat32_last_error();
	terminal_write("ramfs2fat: ");
	terminal_write(reason);
	if (fat_reason != (void *)0 && fat_reason[0] != '\0')
	{
		terminal_write(" (");
		terminal_write(fat_reason);
		terminal_write(")");
	}
	if (src != (void *)0 && src[0] != '\0')
	{
		terminal_write(" src=");
		terminal_write(src);
	}
	if (dst != (void *)0 && dst[0] != '\0')
	{
		terminal_write(" dst=");
		terminal_write(dst);
	}
	terminal_putc('\n');
}

static void terminal_write_fat_failure(const char *prefix)
{
	const char *fat_reason = fat32_last_error();
	terminal_write(prefix);
	if (fat_reason != (void *)0 && fat_reason[0] != '\0')
	{
		terminal_write(" (");
		terminal_write(fat_reason);
		terminal_write(")");
	}
	terminal_putc('\n');
}

static void ramfs_copy_to_fat_r(const char *rpath, const char *fpath, int *copied, int *errors, int depth, int map_only)
{
	char names[RAMFS2FAT_BATCH_MAX][FS_NAME_MAX + 2];
	int types[RAMFS2FAT_BATCH_MAX];
	char used_names[RAMFS2FAT_USED_MAX][16];
	int used_count = 0;
	int count;
	int i;

	if (depth > 16) return;
	if (terminal_cancel_requested) return;
	count = 0;
	if (fs_ls(rpath, names, types, RAMFS2FAT_BATCH_MAX, &count) != 0) return;

	/*
	 * Track only names chosen during this sync pass so repeated runs update
	 * existing FAT files in-place (instead of creating APP~1, APP~2, ...).
	 */

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
			if (!map_only)
			{
				int dst_is_dir = 0;
				unsigned long dst_size = 0;
				if (fat32_stat_path(child_fpath, &dst_is_dir, &dst_size) == 0)
				{
					if (!dst_is_dir)
					{
						ramfs2fat_report_error("type conflict (file where directory expected)", child_rpath, child_fpath);
						(*errors)++;
						continue;
					}
				}
				else if (fat32_mkdir_path(child_fpath) != 0)
				{
					ramfs2fat_report_error("mkdir failed", child_rpath, child_fpath);
					(*errors)++;
					continue;
				}
			}
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
				int dst_is_dir = 0;
				unsigned long dst_size = 0;
				unsigned long size;
				int write_ok = 0;
				if (fat32_stat_path(child_fpath, &dst_is_dir, &dst_size) == 0 && dst_is_dir)
				{
					ramfs2fat_report_error("type conflict (directory where file expected)", child_rpath, child_fpath);
					(*errors)++;
					continue;
				}
				size = 0;
				if (fs_read_file(child_rpath, ramfs2fat_copy_buf, sizeof(ramfs2fat_copy_buf), &size) == 0)
				{
					if (fat32_write_file_path(child_fpath, ramfs2fat_copy_buf, size) == 0)
					{
						write_ok = 1;
					}
					else if (fat32_stat_path(child_fpath, &dst_is_dir, &dst_size) == 0 && !dst_is_dir)
					{
						if (fat32_remove_path(child_fpath) == 0 &&
							fat32_write_file_path(child_fpath, ramfs2fat_copy_buf, size) == 0)
						{
							write_ok = 1;
						}
					}

					if (write_ok)
					{
						(*copied)++;
					}
					else
					{
						ramfs2fat_report_error("write failed (replace retry failed)", child_rpath, child_fpath);
						(*errors)++;
					}
				}
				else
				{
					ramfs2fat_report_error("read failed", child_rpath, child_fpath);
					(*errors)++;
				}
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

static int fatstress_build_path(unsigned int slot, char *path, unsigned long path_size)
{
	char n[16];
	unsigned long i = 0;
	unsigned long j = 0;
	if (path_size < 9) return -1;
	path[i++] = '/';
	path[i++] = 'F';
	path[i++] = 'S';
	uint_to_dec((unsigned long)slot, n, sizeof(n));
	while (n[j] != '\0')
	{
		if (i + 1 >= path_size) return -1;
		path[i++] = n[j++];
	}
	if (i + 5 >= path_size) return -1;
	path[i++] = '.';
	path[i++] = 'R';
	path[i++] = 'E';
	path[i++] = 'C';
	path[i] = '\0';
	return 0;
}

static void cmd_fatstress(const char *args)
{
	unsigned int rounds = 200;
	unsigned int i;
	unsigned int failures = 0;
	unsigned int deleted = 0;
	unsigned int verbose = 0;
	unsigned int progress_every = 200;
	unsigned int payload_size = 32;
	unsigned int slots = 64;
	unsigned int delete_every = 8;
	char tok[16];
	char n[16];
	char path[32];
	const char *p;

	if (!fat32_is_mounted())
	{
		terminal_write_line("fatstress: not mounted (run fatmount)");
		return;
	}

	p = args;
	for (;;)
	{
		unsigned int parsed;
		p = read_token(p, tok, sizeof(tok));
		if (p == (void *)0)
		{
			terminal_write_line("Usage: fatstress [rounds] [-v] [-p <n>] [-s <bytes>] [-k <slots>] [-d <n>]");
			return;
		}
		if (tok[0] == '\0') break;
		if (string_equals(tok, "-v"))
		{
			verbose = 1;
			continue;
		}
		if (string_equals(tok, "-p"))
		{
			char p_tok[16];
			p = read_token(p, p_tok, sizeof(p_tok));
			if (p == (void *)0 || p_tok[0] == '\0' || parse_dec_u32(p_tok, &parsed) != 0 || parsed == 0 || parsed > 100000U)
			{
				terminal_write_line("Usage: fatstress [rounds] [-v] [-p <n>] [-s <bytes>] [-k <slots>] [-d <n>]");
				return;
			}
			progress_every = parsed;
			continue;
		}
		if (string_equals(tok, "-s"))
		{
			char s_tok[16];
			p = read_token(p, s_tok, sizeof(s_tok));
			if (p == (void *)0 || s_tok[0] == '\0' || parse_dec_u32(s_tok, &parsed) != 0 || parsed == 0 || parsed > FATSTRESS_MAX_PAYLOAD)
			{
				terminal_write_line("Usage: fatstress [rounds] [-v] [-p <n>] [-s <bytes>] [-k <slots>] [-d <n>]");
				return;
			}
			payload_size = parsed;
			continue;
		}
		if (string_equals(tok, "-k"))
		{
			char k_tok[16];
			p = read_token(p, k_tok, sizeof(k_tok));
			if (p == (void *)0 || k_tok[0] == '\0' || parse_dec_u32(k_tok, &parsed) != 0 || parsed == 0 || parsed > 9999U)
			{
				terminal_write_line("Usage: fatstress [rounds] [-v] [-p <n>] [-s <bytes>] [-k <slots>] [-d <n>]");
				return;
			}
			slots = parsed;
			continue;
		}
		if (string_equals(tok, "-d"))
		{
			char d_tok[16];
			p = read_token(p, d_tok, sizeof(d_tok));
			if (p == (void *)0 || d_tok[0] == '\0' || parse_dec_u32(d_tok, &parsed) != 0 || parsed == 0 || parsed > 100000U)
			{
				terminal_write_line("Usage: fatstress [rounds] [-v] [-p <n>] [-s <bytes>] [-k <slots>] [-d <n>]");
				return;
			}
			delete_every = parsed;
			continue;
		}

		if (parse_dec_u32(tok, &parsed) != 0 || parsed == 0 || parsed > 100000U)
		{
			terminal_write_line("Usage: fatstress [rounds] [-v] [-p <n>] [-s <bytes>] [-k <slots>] [-d <n>]");
			return;
		}
		rounds = parsed;
	}

	terminal_write("fatstress: rounds=");
	uint_to_dec((unsigned long)rounds, n, sizeof(n));
	terminal_write(n);
	terminal_write(" verbose=");
	terminal_write(verbose ? "on" : "off");
	terminal_write(" progress=");
	uint_to_dec((unsigned long)progress_every, n, sizeof(n));
	terminal_write(n);
	terminal_write(" payload=");
	uint_to_dec((unsigned long)payload_size, n, sizeof(n));
	terminal_write(n);
	terminal_write(" slots=");
	uint_to_dec((unsigned long)slots, n, sizeof(n));
	terminal_write(n);
	terminal_write(" del=");
	uint_to_dec((unsigned long)delete_every, n, sizeof(n));
	terminal_write_line(n);

	terminal_take_cancel_request();
	for (i = 0; i < rounds; i++)
	{
		unsigned long verify_len = 0;
		unsigned long j;
		unsigned int progress_now = (i == 0U) || (((i + 1U) % progress_every) == 0U);
		unsigned int slot = i % slots;

		terminal_poll_control_hotkeys();
		if (terminal_take_cancel_request())
		{
			terminal_write_line("fatstress: canceled");
			break;
		}

		if (fatstress_build_path(slot, path, sizeof(path)) != 0)
		{
			terminal_write_line("fatstress: internal path overflow");
			failures++;
			break;
		}

		if (verbose && progress_now)
		{
			unsigned long free_bytes = 0;
			unsigned long free_kib = 0;
			terminal_write("fatstress: round ");
			uint_to_dec((unsigned long)(i + 1U), n, sizeof(n));
			terminal_write(n);
			terminal_write(" path=");
			terminal_write(path);
			if (fat32_get_free_bytes(&free_bytes) == 0)
			{
				free_kib = free_bytes / 1024UL;
				terminal_write(" free_kib=");
				uint_to_dec(free_kib, n, sizeof(n));
				terminal_write(n);
			}
			terminal_putc('\n');
		}

		for (j = 0; j < payload_size; j++)
		{
			fatstress_payload_buf[j] = (unsigned char)(((unsigned long)i * 17UL + (unsigned long)slot * 31UL + j * 7UL) & 0xFFUL);
		}

		if (fat32_write_file_path(path, fatstress_payload_buf, payload_size) != 0)
		{
			failures++;
			terminal_write("fatstress: write failed at ");
			terminal_write(path);
			if (fat32_last_error()[0] != '\0')
			{
				terminal_write(" (");
				terminal_write(fat32_last_error());
				terminal_write(")");
			}
			terminal_putc('\n');
			break;
		}
		if (fat32_read_file_path(path, fatstress_verify_buf, payload_size, &verify_len) != 0)
		{
			failures++;
			terminal_write("fatstress: read failed at ");
			terminal_write(path);
			if (fat32_last_error()[0] != '\0')
			{
				terminal_write(" (");
				terminal_write(fat32_last_error());
				terminal_write(")");
			}
			terminal_putc('\n');
			break;
		}
		if (verify_len != payload_size)
		{
			failures++;
			terminal_write("fatstress: size mismatch at ");
			terminal_write_line(path);
			break;
		}
		for (j = 0; j < payload_size; j++)
		{
			if (fatstress_verify_buf[j] != fatstress_payload_buf[j])
			{
				failures++;
				terminal_write("fatstress: data mismatch at ");
				terminal_write_line(path);
				i = rounds;
				break;
			}
		}

		if (delete_every > 0U && ((i % delete_every) == (delete_every - 1U)))
		{
			if (fat32_remove_path(path) == 0)
			{
				deleted++;
				if (verbose && progress_now)
				{
					terminal_write("fatstress: deleted ");
					terminal_write_line(path);
				}
			}
		}
	}

	for (i = 0; i < slots; i++)
	{
		if (fatstress_build_path(i, path, sizeof(path)) != 0) break;
		if (fat32_remove_path(path) == 0) deleted++;
	}

	terminal_write("fatstress: ");
	if (failures == 0) terminal_write("OK ");
	else terminal_write("FAILED ");
	terminal_write("rounds=");
	uint_to_dec((unsigned long)rounds, n, sizeof(n));
	terminal_write(n);
	terminal_write(" deleted=");
	uint_to_dec((unsigned long)deleted, n, sizeof(n));
	terminal_write(n);
	terminal_write(" failures=");
	uint_to_dec((unsigned long)failures, n, sizeof(n));
	terminal_write_line(n);
}

static void cmd_fatperf(const char *args)
{
	char tok[64];
	char tok2[64];
	char tok3[64];
	const char *p = args;
	unsigned int v;

	p = read_token(p, tok, sizeof(tok));
	if (p == (void *)0)
	{
		terminal_write_line("Usage: fatperf [show|batch <1..16>|cache <on|off>|cache data|fat <on|off>|flush|dirbench [path] [list]]");
		return;
	}

	if (tok[0] == '\0' || string_equals(tok, "show"))
	{
		char n[16];
		terminal_write("fatperf: batch=");
		uint_to_dec((unsigned long)fat32_get_io_batch_sectors(), n, sizeof(n));
		terminal_write(n);
		terminal_write(" data-cache=");
		terminal_write(fat32_get_data_cache_enabled() ? "on" : "off");
		terminal_write(" fat-cache=");
		terminal_write_line(fat32_get_fat_cache_enabled() ? "on" : "off");
		return;
	}

	if (string_equals(tok, "batch"))
	{
		p = read_token(p, tok2, sizeof(tok2));
		if (p == (void *)0 || tok2[0] == '\0' || parse_dec_u32(tok2, &v) != 0 || fat32_set_io_batch_sectors(v) != 0)
		{
			terminal_write_line("Usage: fatperf batch <1..16>");
			return;
		}
		terminal_write("fatperf: batch=");
		uint_to_dec((unsigned long)fat32_get_io_batch_sectors(), tok2, sizeof(tok2));
		terminal_write_line(tok2);
		return;
	}

	if (string_equals(tok, "cache"))
	{
		p = read_token(p, tok2, sizeof(tok2));
		if (p == (void *)0 || tok2[0] == '\0')
		{
			terminal_write_line("Usage: fatperf cache <on|off>");
			terminal_write_line("   or: fatperf cache data|fat <on|off>");
			return;
		}
		if (string_equals(tok2, "on") || string_equals(tok2, "off"))
		{
			int on = string_equals(tok2, "on") ? 1 : 0;
			fat32_set_data_cache_enabled(on);
			fat32_set_fat_cache_enabled(on);
			terminal_write_line("fatperf: cache updated");
			return;
		}
		if (string_equals(tok2, "data") || string_equals(tok2, "fat"))
		{
			p = read_token(p, tok3, sizeof(tok3));
			if (p == (void *)0 || (!string_equals(tok3, "on") && !string_equals(tok3, "off")))
			{
				terminal_write_line("Usage: fatperf cache data|fat <on|off>");
				return;
			}
			if (string_equals(tok2, "data")) fat32_set_data_cache_enabled(string_equals(tok3, "on"));
			else fat32_set_fat_cache_enabled(string_equals(tok3, "on"));
			terminal_write_line("fatperf: cache updated");
			return;
		}
		terminal_write_line("Usage: fatperf cache <on|off>");
		terminal_write_line("   or: fatperf cache data|fat <on|off>");
		return;
	}

	if (string_equals(tok, "flush"))
	{
		fat32_flush_cache();
		terminal_write_line("fatperf: caches flushed");
		return;
	}

	if (string_equals(tok, "dirbench"))
	{
		unsigned int old_batch = fat32_get_io_batch_sectors();
		unsigned int batches[16] = {1U, 2U, 4U, 8U, 16U};
		unsigned int batch_count = 5U;
		char path[128];
		char list[64];
		char argbuf[196];
		unsigned int i;

		path[0] = '\0';
		list[0] = '\0';
		p = read_token(p, path, sizeof(path));
		if (p == (void *)0)
		{
			terminal_write_line("Usage: fatperf dirbench [path] [1,2,4,8,16]");
			return;
		}
		if (path[0] == '\0')
		{
			path[0] = '.';
			path[1] = '\0';
		}
		else
		{
			p = read_token(p, list, sizeof(list));
			if (p == (void *)0)
			{
				terminal_write_line("Usage: fatperf dirbench [path] [1,2,4,8,16]");
				return;
			}
		}

		if (list[0] != '\0')
		{
			unsigned int parsed[16];
			unsigned int parsed_count = 0;
			unsigned long pos = 0;
			while (list[pos] != '\0')
			{
				char nbuf[12];
				unsigned long n = 0;
				while (list[pos] >= '0' && list[pos] <= '9' && n + 1 < sizeof(nbuf)) nbuf[n++] = list[pos++];
				nbuf[n] = '\0';
				if (n == 0 || parse_dec_u32(nbuf, &v) != 0 || fat32_set_io_batch_sectors(v) != 0 || parsed_count >= 16U)
				{
					fat32_set_io_batch_sectors(old_batch);
					terminal_write_line("fatperf: invalid list (use values 1..16, comma-separated)");
					return;
				}
				parsed[parsed_count++] = v;
				if (list[pos] == ',') pos++;
				else if (list[pos] != '\0')
				{
					fat32_set_io_batch_sectors(old_batch);
					terminal_write_line("fatperf: invalid list format");
					return;
				}
			}
			for (i = 0; i < parsed_count; i++) batches[i] = parsed[i];
			batch_count = parsed_count;
		}

		terminal_write_line("fatperf: dirbench (ticks)");
		for (i = 0; i < batch_count; i++)
		{
			unsigned long t0;
			unsigned long t1;
			char n[16];
			unsigned long w = 0;
			unsigned long pi = 0;

			if (fat32_set_io_batch_sectors(batches[i]) != 0) continue;

			argbuf[w++] = '/';
			argbuf[w++] = 'b';
			argbuf[w++] = ' ';
			while (path[pi] != '\0' && w + 1 < sizeof(argbuf)) { argbuf[w++] = path[pi++]; }
			argbuf[w] = '\0';

			t0 = timer_ticks();
			cmd_dir(argbuf);
			t1 = timer_ticks();

			terminal_write("  batch=");
			uint_to_dec((unsigned long)batches[i], n, sizeof(n));
			terminal_write(n);
			terminal_write(" dt=");
			uint_to_dec(t1 - t0, n, sizeof(n));
			terminal_write_line(n);
		}
		fat32_set_io_batch_sectors(old_batch);
		terminal_write_line("fatperf: dirbench done");
		return;
	}

	terminal_write_line("Usage: fatperf [show|batch <1..16>|cache <on|off>|cache data|fat <on|off>|flush|dirbench [path] [list]]");
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

	if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0)
	{
		terminal_write_line("fatcat: bad path");
		return;
	}

	if (fat32_read_file_path(full_path, data, sizeof(data) - 1, &size) != 0)
	{
		terminal_write_fat_failure("fatcat: read failed");
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
	if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0)
	{
		terminal_write_line("fattouch: bad path");
		return;
	}
	if (fat32_touch_file_path(full_path) != 0)
	{
		terminal_write_fat_failure("fattouch: failed");
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
	if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0)
	{
		terminal_write_line("fatwrite: bad path");
		return;
	}
	if (fat32_write_file_path(full_path, (const unsigned char *)p, len) != 0)
	{
		terminal_write_fat_failure("fatwrite: failed");
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
			terminal_write_fat_failure("fatattr: set failed");
			return;
		}
	}
	else
	{
		if (fat32_get_attr_path(full_path, &attr) != 0)
		{
			terminal_write_fat_failure("fatattr: read failed");
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
	if (fat_resolve_path(path, full_path, sizeof(full_path)) != 0)
	{
		terminal_write_line("fatrm: bad path");
		return;
	}
	if (fat32_remove_path(full_path) != 0)
	{
		terminal_write_fat_failure("fatrm: failed");
		return;
	}
	terminal_write_line("fatrm: done");
}

/* ---- Network shell commands ---- */

static int parse_ip(const char *s, unsigned char out[4])
{
	unsigned long octet = 0;
	int dot_count = 0;
	int digit_count = 0;
	int i = 0;
	while (s[i] != '\0' && s[i] != ' ')
	{
		if (s[i] >= '0' && s[i] <= '9')
		{
			octet = octet * 10 + (unsigned long)(s[i] - '0');
			if (octet > 255) return -1;
			digit_count++;
		}
		else if (s[i] == '.')
		{
			if (digit_count == 0 || dot_count >= 3) return -1;
			out[dot_count] = (unsigned char)octet;
			octet = 0;
			digit_count = 0;
			dot_count++;
		}
		else return -1;
		i++;
	}
	if (dot_count != 3 || digit_count == 0) return -1;
	out[3] = (unsigned char)octet;
	return 0;
}

static void format_ip(const unsigned char ip[4], char *buf, unsigned long buf_sz)
{
	char n[4];
	unsigned long o = 0;
	int i;
	for (i = 0; i < 4; i++)
	{
		uint_to_dec(ip[i], n, sizeof(n));
		for (int j = 0; n[j] != '\0' && o + 1 < buf_sz; j++)
			buf[o++] = n[j];
		if (i < 3 && o + 1 < buf_sz) buf[o++] = '.';
	}
	buf[o] = '\0';
}

static void format_mac(const unsigned char mac[6], char *buf, unsigned long buf_sz)
{
	static const char hex[] = "0123456789AB";
	unsigned long o = 0;
	int i;
	for (i = 0; i < 6; i++)
	{
		if (o + 2 < buf_sz) { buf[o++] = hex[(mac[i] >> 4) & 0xF]; buf[o++] = hex[mac[i] & 0xF]; }
		if (i < 5 && o + 1 < buf_sz) buf[o++] = ':';
	}
	buf[o] = '\0';
}

static void cmd_netinfo(void)
{
	char ip_buf[16], mac_buf[18];
	unsigned char mac[6];
	e1000_get_mac(mac);
	format_mac(mac, mac_buf, sizeof(mac_buf));
	terminal_write("  MAC:     "); terminal_write_line(mac_buf);
	format_ip(net_cfg.ip, ip_buf, sizeof(ip_buf));
	terminal_write("  IP:      "); terminal_write_line(ip_buf);
	format_ip(net_cfg.gateway, ip_buf, sizeof(ip_buf));
	terminal_write("  Gateway: "); terminal_write_line(ip_buf);
	format_ip(net_cfg.netmask, ip_buf, sizeof(ip_buf));
	terminal_write("  Netmask: "); terminal_write_line(ip_buf);
	format_ip(net_cfg.dns, ip_buf, sizeof(ip_buf));
	terminal_write("  DNS:     "); terminal_write_line(ip_buf);
	terminal_write("  Link:    ");
	terminal_write_line(e1000_is_link_up() ? "UP" : "DOWN");
}

static void cmd_netreinit(void)
{
	static const char hx[] = "0123456789ABCDEF";
	char n[16];
	int rc, i;

	terminal_write_line("[net] re-initialising network stack...");

	/* PCI scan */
	terminal_write("[net] scanning PCI bus... ");
	pci_scan();
	uint_to_dec((unsigned long)pci_device_count, n, sizeof(n));
	terminal_write(n);
	terminal_write_line(" device(s) found");

	/* List PCI devices */
	for (i = 0; i < pci_device_count && i < 16; i++)
	{
		unsigned short vid = pci_devices[i].vendor_id;
		unsigned short did = pci_devices[i].device_id;
		char h[5];
		terminal_write("  ");
		h[0] = hx[(vid >> 12) & 0xF]; h[1] = hx[(vid >> 8) & 0xF];
		h[2] = hx[(vid >> 4) & 0xF];  h[3] = hx[vid & 0xF]; h[4] = '\0';
		terminal_write(h);
		terminal_write(":");
		h[0] = hx[(did >> 12) & 0xF]; h[1] = hx[(did >> 8) & 0xF];
		h[2] = hx[(did >> 4) & 0xF];  h[3] = hx[did & 0xF]; h[4] = '\0';
		terminal_write(h);
		terminal_write(" class ");
		h[0] = hx[(pci_devices[i].class_code >> 4) & 0xF];
		h[1] = hx[pci_devices[i].class_code & 0xF];
		h[2] = ':';
		h[3] = hx[(pci_devices[i].subclass >> 4) & 0xF];
		h[4] = '\0';
		terminal_write(h);
		h[0] = hx[pci_devices[i].subclass & 0xF]; h[1] = '\0';
		terminal_write(h);
		if (vid == 0x8086 && did == 0x100E)
			terminal_write("  <-- E1000");
		terminal_write_line("");
	}

	/* Attempt e1000 init */
	terminal_write("[net] initialising E1000 NIC... ");
	rc = net_init();
	if (rc == 0)
	{
		terminal_write_line("OK");
		cmd_netinfo();
	}
	else
	{
		terminal_write_line("FAILED");
		terminal_write_line("[net] hint: QEMU needs  -netdev user,id=net0 -device e1000,netdev=net0");
	}
}

static void cmd_ping(const char *args)
{
	unsigned char dst[4];
	char ip_buf[16], n[16];
	unsigned short seq;
	unsigned long rtt;
	int i, got_reply;
	int count = 4;
	const char *p = args;

	if (p[0] == '\0') { terminal_write_line("Usage: ping [-c count] <ip>"); return; }
	if (p[0] == '-' && p[1] == 'c' && p[2] == ' ')
	{
		p += 3;
		while (*p == ' ') p++;
		count = 0;
		while (*p >= '0' && *p <= '9') count = count * 10 + (*p++ - '0');
		while (*p == ' ') p++;
		if (count < 1) count = 1;
		if (count > 100) count = 100;
	}
	if (p[0] == '\0') { terminal_write_line("Usage: ping [-c count] <ip>"); return; }
	if (parse_ip(p, dst) != 0) { terminal_write_line("ping: bad IP address"); return; }

	format_ip(dst, ip_buf, sizeof(ip_buf));
	terminal_write("PING "); terminal_write_line(ip_buf);

	for (seq = 1; seq <= (unsigned short)count; seq++)
	{
		if (net_ping(dst, seq) != 0)
		{
			terminal_write_line("ping: send failed");
			return;
		}
		/* Wait up to ~3 seconds for reply (300 polls * ~10ms) */
		got_reply = 0;
		for (i = 0; i < 300; i++)
		{
			net_poll();
			if (net_ping_check(seq, &rtt))
			{
				got_reply = 1;
				terminal_write("  reply seq=");
				uint_to_dec(seq, n, sizeof(n)); terminal_write(n);
				terminal_write(" rtt=");
				uint_to_dec(rtt, n, sizeof(n)); terminal_write(n);
				terminal_write_line("ms");
				break;
			}
			/* Small busy-wait: yield to other tasks */
			for (volatile int d = 0; d < 10000; d++) {}
		}
		if (!got_reply)
		{
			terminal_write("  timeout seq=");
			uint_to_dec(seq, n, sizeof(n)); terminal_write_line(n);
		}
	}
}

static void cmd_arp(void)
{
	char ip_buf[16], mac_buf[18];
	unsigned char ip[4], mac[6];
	int found = 0;
	/* Probe the common subnet range */
	for (int i = 1; i < 255; i++)
	{
		ip[0] = net_cfg.ip[0]; ip[1] = net_cfg.ip[1]; ip[2] = net_cfg.ip[2];
		ip[3] = (unsigned char)i;
		if (arp_lookup(ip, mac) == 0)
		{
			format_ip(ip, ip_buf, sizeof(ip_buf));
			format_mac(mac, mac_buf, sizeof(mac_buf));
			terminal_write("  "); terminal_write(ip_buf);
			terminal_write("  ->  "); terminal_write_line(mac_buf);
			found++;
		}
	}
	if (!found) terminal_write_line("  (ARP cache empty)");
}

static void cmd_udpsend(const char *args)
{
	unsigned char dst[4];
	char ip_str[16];
	unsigned long port = 0;
	int i = 0, j;

	/* Parse: ip port message */
	j = 0;
	while (args[i] != '\0' && args[i] != ' ' && j + 1 < (int)sizeof(ip_str))
		ip_str[j++] = args[i++];
	ip_str[j] = '\0';
	if (parse_ip(ip_str, dst) != 0) { terminal_write_line("Usage: udpsend <ip> <port> <message>"); return; }

	while (args[i] == ' ') i++;
	while (args[i] >= '0' && args[i] <= '9')
		port = port * 10 + (unsigned long)(args[i++] - '0');
	if (port == 0 || port > 65535) { terminal_write_line("udpsend: bad port"); return; }

	while (args[i] == ' ') i++;
	if (args[i] == '\0') { terminal_write_line("udpsend: no message"); return; }

	const char *msg = &args[i];
	unsigned long len = 0;
	while (msg[len] != '\0') len++;

	if (net_udp_send(dst, 12345, (unsigned short)port, msg, len) == 0)
		terminal_write_line("udpsend: sent");
	else
		terminal_write_line("udpsend: failed");
}

static void cmd_dhcp(void)
{
	char ip_buf[16];
	terminal_write_line("DHCP: sending discover...");
	if (net_dhcp_request() == 0)
	{
		terminal_write_line("DHCP: lease obtained");
		format_ip(net_cfg.ip, ip_buf, sizeof(ip_buf));
		terminal_write("  IP:      "); terminal_write_line(ip_buf);
		format_ip(net_cfg.gateway, ip_buf, sizeof(ip_buf));
		terminal_write("  Gateway: "); terminal_write_line(ip_buf);
		format_ip(net_cfg.netmask, ip_buf, sizeof(ip_buf));
		terminal_write("  Netmask: "); terminal_write_line(ip_buf);
		format_ip(net_cfg.dns, ip_buf, sizeof(ip_buf));
		terminal_write("  DNS:     "); terminal_write_line(ip_buf);
	}
	else
	{
		terminal_write_line("DHCP: failed (timeout or no server)");
	}
}

static void cmd_nslookup(const char *args)
{
	unsigned char ip[4];
	char ip_buf[16];

	if (args[0] == '\0') { terminal_write_line("Usage: nslookup <hostname>"); return; }

	terminal_write("Resolving "); terminal_write(args); terminal_write_line("...");
	if (net_dns_resolve(args, ip) == 0)
	{
		format_ip(ip, ip_buf, sizeof(ip_buf));
		terminal_write("  "); terminal_write(args);
		terminal_write(" -> "); terminal_write_line(ip_buf);
	}
	else
	{
		terminal_write_line("  DNS resolution failed");
	}
}

static void cmd_wget(const char *args)
{
	/* wget <ip> <port> <path>  — basic HTTP GET via TCP */
	unsigned char dst[4];
	char ip_str[16], n_buf[16];
	unsigned long port = 0;
	int i = 0, j, conn;
	static char http_buf[1500];
	static char recv_buf[NET_TCP_BUF_SIZE];
	int total_recv = 0, bytes;

	/* Parse: ip port [path] */
	j = 0;
	while (args[i] != '\0' && args[i] != ' ' && j + 1 < (int)sizeof(ip_str))
		ip_str[j++] = args[i++];
	ip_str[j] = '\0';
	if (parse_ip(ip_str, dst) != 0) { terminal_write_line("Usage: wget <ip> <port> [path]"); return; }

	while (args[i] == ' ') i++;
	while (args[i] >= '0' && args[i] <= '9')
		port = port * 10 + (unsigned long)(args[i++] - '0');
	if (port == 0 || port > 65535) { terminal_write_line("wget: bad port"); return; }

	while (args[i] == ' ') i++;
	const char *path = (args[i] != '\0') ? &args[i] : "/";

	terminal_write("Connecting to "); terminal_write(ip_str);
	terminal_write(":"); uint_to_dec(port, n_buf, sizeof(n_buf)); terminal_write(n_buf);
	terminal_write_line("...");

	conn = net_tcp_connect(dst, (unsigned short)port);
	if (conn < 0)
	{
		terminal_write_line("wget: connection failed");
		return;
	}
	terminal_write_line("Connected. Sending HTTP GET...");

	/* Build HTTP/1.0 GET request */
	j = 0;
	{
		const char *prefix = "GET ";
		int k = 0;
		while (prefix[k]) http_buf[j++] = prefix[k++];
	}
	{
		int k = 0;
		while (path[k] && j < (int)sizeof(http_buf) - 40) http_buf[j++] = path[k++];
	}
	{
		const char *suffix = " HTTP/1.0\r\nHost: ";
		int k = 0;
		while (suffix[k]) http_buf[j++] = suffix[k++];
	}
	{
		int k = 0;
		while (ip_str[k]) http_buf[j++] = ip_str[k++];
	}
	{
		const char *end = "\r\nConnection: close\r\n\r\n";
		int k = 0;
		while (end[k]) http_buf[j++] = end[k++];
	}

	if (net_tcp_send(conn, http_buf, (unsigned long)j) != 0)
	{
		terminal_write_line("wget: send failed");
		net_tcp_close(conn);
		return;
	}

	/* Receive response */
	while (total_recv < (int)sizeof(recv_buf) - 1)
	{
		bytes = net_tcp_recv(conn, recv_buf + total_recv,
		                     (unsigned long)(sizeof(recv_buf) - 1 - (unsigned long)total_recv), 300);
		if (bytes <= 0) break;
		total_recv += bytes;
	}
	recv_buf[total_recv] = '\0';

	net_tcp_close(conn);

	/* Display response */
	uint_to_dec((unsigned long)total_recv, n_buf, sizeof(n_buf));
	terminal_write("Received "); terminal_write(n_buf); terminal_write_line(" bytes:");
	terminal_write_line("---");
	terminal_write(recv_buf);
	terminal_write_line("");
	terminal_write_line("---");
}

static void run_command_dispatch(void)
{
	last_exit_code = 0;
	if (input_length == 0) return;

	/* time <cmd> — measure execution time */
	if (string_starts_with(input_buffer, "time ") && input_buffer[5] != '\0')
	{
		unsigned long t0, t1, elapsed_ms;
		char tb[32];
		/* shift input to remove "time " prefix */
		unsigned long si = 5, di = 0;
		while (input_buffer[si]) { input_buffer[di++] = input_buffer[si++]; }
		input_buffer[di] = '\0';
		input_length = di;
		t0 = timer_ticks();
		run_command_dispatch();
		t1 = timer_ticks();
		elapsed_ms = (t1 - t0) * 10; /* 100 Hz timer → 10ms per tick */
		terminal_write("real    0m");
		uint_to_dec(elapsed_ms / 1000, tb, sizeof(tb));
		terminal_write(tb);
		terminal_write(".");
		{ unsigned long frac = elapsed_ms % 1000; tb[0]='0'+(char)(frac/100); tb[1]='0'+(char)((frac/10)%10); tb[2]='0'+(char)(frac%10); tb[3]=0; }
		terminal_write(tb);
		terminal_write_line("s");
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
		else terminal_write_line("Usage: dir [/b] [/w] [/s] [/rN] [path]");
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
	else if (string_starts_with(input_buffer, "grep"))
	{
		if (input_buffer[4] == ' ') cmd_grep(input_buffer + 5);
		else terminal_write_line("Usage: grep [-i] [-n] [-v] [-c] <pattern> <file>");
	}
	else if (string_starts_with(input_buffer, "find"))
	{
		if (input_buffer[4] == ' ') cmd_find(input_buffer + 5);
		else terminal_write_line("Usage: find <pattern> [path]");
	}
	else if (string_starts_with(input_buffer, "wc"))
	{
		if (input_buffer[2] == ' ') cmd_wc(input_buffer + 3);
		else terminal_write_line("Usage: wc <file>");
	}
	else if (string_starts_with(input_buffer, "head"))
	{
		if (input_buffer[4] == ' ') cmd_head(input_buffer + 5);
		else terminal_write_line("Usage: head [-N] <file>");
	}
	else if (string_starts_with(input_buffer, "tail"))
	{
		if (input_buffer[4] == ' ') cmd_tail(input_buffer + 5);
		else terminal_write_line("Usage: tail [-N] <file>");
	}
	else if (string_starts_with(input_buffer, "xxd"))
	{
		if (input_buffer[3] == ' ') cmd_xxd(input_buffer + 4);
		else if (input_buffer[3] == '\0') cmd_xxd("");
		else terminal_write_line("Usage: xxd <file>");
	}
	else if (string_starts_with(input_buffer, "sort"))
	{
		if (input_buffer[4] == ' ') cmd_sort(input_buffer + 5);
		else if (input_buffer[4] == '\0') cmd_sort("");
		else terminal_write_line("Usage: sort [-r] [file]");
	}
	else if (string_starts_with(input_buffer, "uniq"))
	{
		if (input_buffer[4] == ' ') cmd_uniq(input_buffer + 5);
		else if (input_buffer[4] == '\0') cmd_uniq("");
		else terminal_write_line("Usage: uniq [file]");
	}
	else if (string_starts_with(input_buffer, "tee"))
	{
		if (input_buffer[3] == ' ') cmd_tee(input_buffer + 4);
		else terminal_write_line("Usage: tee <file>");
	}
	else if (string_starts_with(input_buffer, "tr"))
	{
		if (input_buffer[2] == ' ') cmd_tr(input_buffer + 3);
		else if (input_buffer[2] == '\0') terminal_write_line("Usage: tr <set1> <set2>");
	}
	else if (string_starts_with(input_buffer, "seq"))
	{
		if (input_buffer[3] == ' ') cmd_seq(input_buffer + 4);
		else terminal_write_line("Usage: seq [start] <end> [step]");
	}
	else if (string_starts_with(input_buffer, "diff"))
	{
		if (input_buffer[4] == ' ') cmd_diff(input_buffer + 5);
		else terminal_write_line("Usage: diff <file1> <file2>");
	}
	else if (string_starts_with(input_buffer, "cmp"))
	{
		if (input_buffer[3] == ' ') cmd_cmp(input_buffer + 4);
		else terminal_write_line("Usage: cmp <file1> <file2>");
	}
	else if (string_starts_with(input_buffer, "calc"))
	{
		if (input_buffer[4] == ' ') cmd_calc(input_buffer + 5);
		else terminal_write_line("Usage: calc <expression>");
	}
	else if (string_equals(input_buffer, "whoami"))
	{
		cmd_whoami();
	}
	else if (string_starts_with(input_buffer, "hostname"))
	{
		if (input_buffer[8] == '\0') cmd_hostname("");
		else if (input_buffer[8] == ' ') cmd_hostname(input_buffer + 9);
		else terminal_write_line("Usage: hostname [new_name]");
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
		else terminal_write_line("Usage: run [-x] [-t <1..3600>] <path>");
	}
	else if (string_starts_with(input_buffer, "basic"))
	{
		if (input_buffer[5] == ' ') cmd_basic(input_buffer + 6);
		else terminal_write_line("Usage: basic <path>");
	}
	else if (string_equals(input_buffer, "clear"))
	{
		input_length = 0; input_buffer[0] = '\0';
		if (gui_active) return; /* GUI redraws window after command */
		screen_clear();
		if (!script_mode_active) terminal_prompt();
		return;
	}
	else if (string_equals(input_buffer, "cls"))
	{
		input_length = 0; input_buffer[0] = '\0';
		if (gui_active) return;
		screen_clear();
		if (!script_mode_active) terminal_prompt();
		return;
	}
	else if (string_equals(input_buffer, "pause"))
	{
		cmd_pause("");
	}
	else if (string_starts_with(input_buffer, "wait"))
	{
		if (input_buffer[4] == ' ') cmd_wait(input_buffer + 5);
		else terminal_write_line("Usage: wait <seconds>");
	}
	else if (string_starts_with(input_buffer, "sleep"))
	{
		if (input_buffer[5] == ' ') cmd_wait(input_buffer + 6);
		else terminal_write_line("Usage: sleep <seconds>");
	}
	else if (string_equals(input_buffer, "env"))
	{
		cmd_setvar("");
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
		else terminal_write_line("Usage: pagefault <read|write|exec> [yes]");
	}
	else if (string_starts_with(input_buffer, "gpfault"))
	{
		if (input_buffer[7] == '\0') cmd_gpfault("");
		else if (input_buffer[7] == ' ') cmd_gpfault(input_buffer + 8);
		else terminal_write_line("Usage: gpfault [yes]");
	}
	else if (string_starts_with(input_buffer, "udfault"))
	{
		if (input_buffer[7] == '\0') cmd_udfault("");
		else if (input_buffer[7] == ' ') cmd_udfault(input_buffer + 8);
		else terminal_write_line("Usage: udfault [yes]");
	}
	else if (string_starts_with(input_buffer, "doublefault"))
	{
		if (input_buffer[11] == '\0') cmd_doublefault("");
		else if (input_buffer[11] == ' ') cmd_doublefault(input_buffer + 12);
		else terminal_write_line("Usage: doublefault [yes]");
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
	else if (string_starts_with(input_buffer, "elfsegs"))
	{
		if (input_buffer[7] == ' ') cmd_elfsegs(input_buffer + 8);
		else terminal_write_line("Usage: elfsegs <path>");
	}
	else if (string_starts_with(input_buffer, "elfsects"))
	{
		if (input_buffer[8] == ' ') cmd_elfsects(input_buffer + 9);
		else terminal_write_line("Usage: elfsects <path>");
	}
	else if (string_starts_with(input_buffer, "elfsym"))
	{
		if (input_buffer[6] == ' ') cmd_elfsym(input_buffer + 7);
		else terminal_write_line("Usage: elfsym <path> [filter]");
	}
	else if (string_starts_with(input_buffer, "elfaddr"))
	{
		if (input_buffer[7] == ' ') cmd_elfaddr(input_buffer + 8);
		else terminal_write_line("Usage: elfaddr <path> <hex-address> | elfaddr <hex-address>");
	}
	else if (string_starts_with(input_buffer, "elfcheck"))
	{
		if (input_buffer[8] == ' ') cmd_elfcheck(input_buffer + 9);
		else terminal_write_line("Usage: elfcheck <path>");
	}
	else if (string_starts_with(input_buffer, "execstress"))
	{
		if (input_buffer[10] == ' ') cmd_execstress(input_buffer + 11);
		else terminal_write_line("Usage: execstress <count> <path>");
	}
	else if (string_starts_with(input_buffer, "exectrace"))
	{
		if (input_buffer[9] == '\0') cmd_exectrace("show");
		else if (input_buffer[9] == ' ') cmd_exectrace(input_buffer + 10);
		else terminal_write_line("Usage: exectrace [on|off|show]");
	}
	else if (string_starts_with(input_buffer, "exec"))
	{
		if (input_buffer[4] == ' ') cmd_exec(input_buffer + 5);
		else terminal_write_line("Usage: exec <path> [args...]");
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
	else if (string_starts_with(input_buffer, "preempt"))
	{
		if (input_buffer[7] == '\0') cmd_preempt("");
		else if (input_buffer[7] == ' ') cmd_preempt(input_buffer + 8);
		else terminal_write_line("Usage: preempt [on|off]");
	}
	else if (string_equals(input_buffer, "shellspawn"))
	{
		cmd_shellspawn();
	}
	else if (string_equals(input_buffer, "jobs"))
	{
		cmd_jobs();
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
	else if (string_equals(input_buffer, "date") || string_starts_with(input_buffer, "date "))
	{
		cmd_date(input_buffer[4] == ' ' ? input_buffer + 5 : "");
	}
	else if (string_equals(input_buffer, "uptime"))
	{
		cmd_uptime();
	}
	else if (string_equals(input_buffer, "motd"))
	{
		cmd_motd();
	}
	else if (string_starts_with(input_buffer, "autorun"))
	{
		if (input_buffer[7] == '\0') cmd_autorun("show");
		else if (input_buffer[7] == ' ') cmd_autorun(input_buffer + 8);
		else terminal_write_line("Usage: autorun [show|log|off|always|once|rearm|stop|run|safe <on|off|show>|delay <0..3600>]");
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
		else terminal_write_line("Usage: fatmount <0..3|hd#> [/hd#|A:]");
	}
	else if (string_starts_with(input_buffer, "mtn") || string_starts_with(input_buffer, "mnt") || string_starts_with(input_buffer, "mount"))
	{
		unsigned long cmd_len = string_starts_with(input_buffer, "mount") ? 5UL : 3UL;
		if (input_buffer[cmd_len] == '\0') cmd_fatmount((void *)0);
		else if (input_buffer[cmd_len] == ' ') cmd_fatmount(input_buffer + cmd_len + 1UL);
		else terminal_write_line("Usage: mtn|mnt|mount <hd#> [/hd#|A:]");
	}
	else if (string_equals(input_buffer, "fatwhere"))
	{
		cmd_fatwhere();
	}
	else if (string_equals(input_buffer, "fatunmount"))
	{
		cmd_fatunmount("");
	}
	else if (string_starts_with(input_buffer, "fatunmount"))
	{
		if (input_buffer[10] == '\0') cmd_fatunmount("");
		else if (input_buffer[10] == ' ') cmd_fatunmount(input_buffer + 11);
		else terminal_write_line("Usage: fatunmount [hd#|A:]");
	}
	else if (string_starts_with(input_buffer, "umtn") || string_starts_with(input_buffer, "umnt") || string_starts_with(input_buffer, "umount"))
	{
		unsigned long cmd_len = string_starts_with(input_buffer, "umount") ? 6UL : 4UL;
		if (input_buffer[cmd_len] == '\0') cmd_fatunmount("");
		else if (input_buffer[cmd_len] == ' ') cmd_fatunmount(input_buffer + cmd_len + 1UL);
		else terminal_write_line("Usage: umtn|umnt|umount [hd#|A:]");
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
	else if (string_starts_with(input_buffer, "fatstress"))
	{
		if (input_buffer[9] == '\0') cmd_fatstress((void *)0);
		else if (input_buffer[9] == ' ') cmd_fatstress(input_buffer + 10);
		else terminal_write_line("Usage: fatstress [rounds] [-v] [-p <n>] [-s <bytes>] [-k <slots>] [-d <n>]");
	}
	else if (string_starts_with(input_buffer, "fatperf"))
	{
		if (input_buffer[7] == '\0') cmd_fatperf((void *)0);
		else if (input_buffer[7] == ' ') cmd_fatperf(input_buffer + 8);
		else terminal_write_line("Usage: fatperf [show|batch <1..16>|cache <on|off>|cache data|fat <on|off>|flush|dirbench [path] [list]]");
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
			const char *earg = &input_buffer[5];
			int no_nl = 0;
			if (earg[0] == '-' && earg[1] == 'n' && (earg[2] == ' ' || earg[2] == '\0'))
			{
				no_nl = 1;
				earg += 2;
				while (*earg == ' ') earg++;
			}
			terminal_write_echo_text(earg);
			if (!no_nl) terminal_putc('\n');
		}
		else    terminal_write_line("Unknown command. Type help for a list.");
	}
	else if (string_starts_with(input_buffer, "set"))
	{
		if (input_buffer[3] == '\0') cmd_setvar("");
		else if (input_buffer[3] == ' ') cmd_setvar(input_buffer + 4);
		else terminal_write_line("Usage: set [<name> <value>]");
	}
	else if (string_starts_with(input_buffer, "unset "))
	{
		cmd_unset(input_buffer + 6);
	}
	else if (string_starts_with(input_buffer, "source ") || string_starts_with(input_buffer, ". "))
	{
		cmd_source(string_starts_with(input_buffer, ". ") ? input_buffer + 2 : input_buffer + 7);
	}
	else if (string_starts_with(input_buffer, "read "))
	{
		cmd_read(input_buffer + 5);
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
	else if (string_equals(input_buffer, "netinfo"))
	{
		cmd_netinfo();
	}
	else if (string_equals(input_buffer, "netreinit"))
	{
		cmd_netreinit();
	}
	else if (string_starts_with(input_buffer, "ping "))
	{
		cmd_ping(input_buffer + 5);
	}
	else if (string_equals(input_buffer, "arp"))
	{
		cmd_arp();
	}
	else if (string_starts_with(input_buffer, "udpsend "))
	{
		cmd_udpsend(input_buffer + 8);
	}
	else if (string_equals(input_buffer, "dhcp"))
	{
		cmd_dhcp();
	}
	else if (string_starts_with(input_buffer, "nslookup "))
	{
		cmd_nslookup(input_buffer + 9);
	}
	else if (string_starts_with(input_buffer, "wget "))
	{
		cmd_wget(input_buffer + 5);
	}
	else if (string_equals(input_buffer, "reboot"))
	{
		do_reboot();
	}
	else if (string_starts_with(input_buffer, "panic"))
	{
		if (input_buffer[5] == '\0' || input_buffer[5] == ' ')
		{
			const char *panic_args = (input_buffer[5] == ' ') ? (input_buffer + 6) : "";
			if (!confirm_dangerous_action(panic_args, "panic"))
			{
				/* no-op */
			}
			else
			{
				terminal_write_line("[SYSTEM] deliberate panic requested");
				trigger_forced_panic();
			}
		}
		else terminal_write_line("Usage: panic [yes]");
	}
	else if (string_equals(input_buffer, "quit") ||
	         string_equals(input_buffer, "exit") ||
	         string_equals(input_buffer, "shutdown"))
	{
		terminal_shutdown();
	}
	else if (string_equals(input_buffer, "history") || string_starts_with(input_buffer, "history "))
	{
		cmd_history(input_buffer[7] == ' ' ? input_buffer + 8 : "");
	}
	else if (string_equals(input_buffer, "df"))
	{
		cmd_df();
	}
	else if (string_starts_with(input_buffer, "stat "))
	{
		cmd_stat(input_buffer + 5);
	}
	else if (string_starts_with(input_buffer, "cut"))
	{
		if (input_buffer[3] == ' ') cmd_cut(input_buffer + 4);
		else if (input_buffer[3] == '\0') cmd_cut("");
		else { terminal_write_line("Usage: cut -d<delim> -f<field> [file]"); }
	}
	else if (string_starts_with(input_buffer, "rev"))
	{
		if (input_buffer[3] == ' ') cmd_rev(input_buffer + 4);
		else if (input_buffer[3] == '\0') cmd_rev("");
		else { terminal_write_line("Usage: rev [file]"); }
	}
	else if (string_starts_with(input_buffer, "printf "))
	{
		cmd_printf(input_buffer + 7);
	}
	else if (string_equals(input_buffer, "true"))
	{
		cmd_true();
	}
	else if (string_equals(input_buffer, "false"))
	{
		cmd_false();
	}
	else if (string_starts_with(input_buffer, "test"))
	{
		if (input_buffer[4] == ' ') cmd_test(input_buffer + 5);
		else if (input_buffer[4] == '\0') { last_exit_code = 1; }
		else { terminal_write_line("Usage: test <expression>"); }
	}
	else if (input_buffer[0] == '[' && input_buffer[1] == ' ')
	{
		cmd_test(input_buffer + 2);
	}
	else if (string_starts_with(input_buffer, "which"))
	{
		if (input_buffer[5] == ' ') cmd_which(input_buffer + 6);
		else terminal_write_line("Usage: which <command>");
	}
	else if (string_starts_with(input_buffer, "type"))
	{
		if (input_buffer[4] == ' ') cmd_type(input_buffer + 5);
		else terminal_write_line("Usage: type <command>");
	}
	else if (string_starts_with(input_buffer, "basename"))
	{
		if (input_buffer[8] == ' ') cmd_basename(input_buffer + 9);
		else terminal_write_line("Usage: basename <path>");
	}
	else if (string_starts_with(input_buffer, "dirname"))
	{
		if (input_buffer[7] == ' ') cmd_dirname(input_buffer + 8);
		else terminal_write_line("Usage: dirname <path>");
	}
	else if (string_starts_with(input_buffer, "yes"))
	{
		if (input_buffer[3] == ' ') cmd_yes(input_buffer + 4);
		else if (input_buffer[3] == '\0') cmd_yes("");
		else { terminal_write_line("Usage: yes [string]"); }
	}
	else if (string_starts_with(input_buffer, "nl"))
	{
		if (input_buffer[2] == ' ') cmd_nl(input_buffer + 3);
		else if (input_buffer[2] == '\0') cmd_nl("");
		else { terminal_write_line("Usage: nl [file]"); }
	}
	else if (string_starts_with(input_buffer, "factor"))
	{
		if (input_buffer[6] == ' ') cmd_factor(input_buffer + 7);
		else terminal_write_line("Usage: factor <number>");
	}
	else if (string_starts_with(input_buffer, "du"))
	{
		if (input_buffer[2] == ' ') cmd_du(input_buffer + 3);
		else if (input_buffer[2] == '\0') cmd_du("");
		else { terminal_write_line("Usage: du [path]"); }
	}
	else if (string_starts_with(input_buffer, "xargs"))
	{
		if (input_buffer[5] == ' ') cmd_xargs(input_buffer + 6);
		else terminal_write_line("Usage: xargs <command>");
	}
	else if (string_starts_with(input_buffer, "less "))
	{
		cmd_less(input_buffer + 5);
	}
	else if (string_equals(input_buffer, "less"))
	{
		terminal_write_line("Usage: less <file>");
	}
	else if (string_starts_with(input_buffer, "more "))
	{
		cmd_less(input_buffer + 5);
	}
	else if (string_equals(input_buffer, "more"))
	{
		terminal_write_line("Usage: more <file>");
	}
	else if (string_starts_with(input_buffer, "tac "))
	{
		cmd_tac(input_buffer + 4);
	}
	else if (string_equals(input_buffer, "tac"))
	{
		terminal_write_line("Usage: tac <file>");
	}
	else if (string_starts_with(input_buffer, "expr "))
	{
		cmd_expr(input_buffer + 5);
	}
	else if (string_equals(input_buffer, "expr"))
	{
		terminal_write_line("Usage: expr <expression>");
	}
	else if (string_starts_with(input_buffer, "watch"))
	{
		if (input_buffer[5] == ' ') cmd_watch(input_buffer + 6);
		else terminal_write_line("Usage: watch [-n <sec>] <command>");
	}
	else if (string_starts_with(input_buffer, "paste"))
	{
		if (input_buffer[5] == ' ') cmd_paste(input_buffer + 6);
		else terminal_write_line("Usage: paste <file1> <file2> [-d <delim>]");
	}
	else if (string_starts_with(input_buffer, "column"))
	{
		if (input_buffer[6] == ' ') cmd_column(input_buffer + 7);
		else if (input_buffer[6] == '\0') cmd_column("");
		else terminal_write_line("Usage: column [file]");
	}
	else if (string_starts_with(input_buffer, "strings"))
	{
		if (input_buffer[7] == ' ') cmd_strings(input_buffer + 8);
		else terminal_write_line("Usage: strings [-n <len>] <file>");
	}
	else if (string_starts_with(input_buffer, "rmdir"))
	{
		if (input_buffer[5] == ' ') cmd_rmdir(input_buffer + 6);
		else terminal_write_line("Usage: rmdir <directory>");
	}
	else if (string_equals(input_buffer, "gui"))
	{
		cmd_gui("");
	}
	else
	{
		terminal_write_line("Unknown command. Type help for a list.");
		last_exit_code = 1;
	}
}

/* ── Glob/wildcard expansion ───────────────────────────────────── */

/* Match a pattern with * and ? against a name. */
static int glob_match(const char *pattern, const char *name)
{
	while (*pattern)
	{
		if (*pattern == '*')
		{
			pattern++;
			if (*pattern == '\0') return 1; /* trailing * matches all */
			while (*name)
			{
				if (glob_match(pattern, name)) return 1;
				name++;
			}
			return 0;
		}
		else if (*pattern == '?')
		{
			if (*name == '\0') return 0;
			pattern++;
			name++;
		}
		else
		{
			if (*pattern != *name) return 0;
			pattern++;
			name++;
		}
	}
	return (*name == '\0');
}

/* Expand glob patterns (* and ?) in a command line.
 * Only expands tokens that contain * or ?. Tokens inside quotes are skipped.
 * Result written to out. Returns 0 on success, -1 on overflow. */
static int expand_globs(const char *in, char *out, unsigned long out_size)
{
	unsigned long oi = 0;
	unsigned long ii = 0;
	unsigned long in_len = 0;

	while (in[in_len]) in_len++;

	while (ii < in_len)
	{
		/* Skip leading spaces */
		while (ii < in_len && in[ii] == ' ')
		{
			if (oi + 1 >= out_size) return -1;
			out[oi++] = in[ii++];
		}

		if (ii >= in_len) break;

		/* Extract a token */
		{
			char token[128];
			unsigned long ti = 0;
			int has_glob = 0;
			int in_quotes = 0;

			while (ii < in_len && (in[ii] != ' ' || in_quotes))
			{
				if (in[ii] == '"') in_quotes = !in_quotes;
				if ((in[ii] == '*' || in[ii] == '?') && !in_quotes) has_glob = 1;
				if (ti + 1 < sizeof(token)) token[ti++] = in[ii];
				ii++;
			}
			token[ti] = '\0';

			if (!has_glob || in_quotes)
			{
				/* Copy token as-is */
				unsigned long k;
				for (k = 0; k < ti; k++)
				{
					if (oi + 1 >= out_size) return -1;
					out[oi++] = token[k];
				}
			}
			else
			{
				/* Find directory path and pattern */
				char dir_path[128];
				const char *pattern;
				int last_slash = -1;
				int j;
				char names[FS_MAX_LIST][FS_NAME_MAX + 2];
				int types[FS_MAX_LIST];
				int count = 0;
				int matched = 0;

				for (j = 0; token[j]; j++)
					if (token[j] == '/') last_slash = j;

				if (last_slash >= 0)
				{
					for (j = 0; j < last_slash; j++) dir_path[j] = token[j];
					dir_path[last_slash] = '\0';
					pattern = token + last_slash + 1;
				}
				else
				{
					dir_path[0] = '\0';
					pattern = token;
				}

				if (fs_ls(dir_path[0] ? dir_path : (void *)0, names, types, FS_MAX_LIST, &count) == 0)
				{
					for (j = 0; j < count; j++)
					{
						if (names[j][0] == '.' && pattern[0] != '.') continue; /* skip dotfiles */
						if (glob_match(pattern, names[j]))
						{
							if (matched > 0)
							{
								if (oi + 1 >= out_size) return -1;
								out[oi++] = ' ';
							}
							/* Write full path */
							if (last_slash >= 0)
							{
								int k;
								for (k = 0; k <= last_slash; k++)
								{
									if (oi + 1 >= out_size) return -1;
									out[oi++] = token[k];
								}
							}
							{
								int k = 0;
								while (names[j][k])
								{
									if (oi + 1 >= out_size) return -1;
									out[oi++] = names[j][k++];
								}
							}
							matched++;
						}
					}
				}

				/* If no matches, keep the original token (standard shell behavior) */
				if (matched == 0)
				{
					unsigned long k;
					for (k = 0; k < ti; k++)
					{
						if (oi + 1 >= out_size) return -1;
						out[oi++] = token[k];
					}
				}
			}
		}
	}
	out[oi] = '\0';
	return 0;
}

static void run_command_single(void);

static void run_command(void)
{
	/* Split on unquoted ';', '&&', '||' and run each sub-command */
	char full_copy[INPUT_BUFFER_SIZE];
	unsigned long fi = 0;
	int in_q = 0;
	unsigned long seg_start;
	/* operators: 0 = none/';', 1 = '&&', 2 = '||' */
	int pending_op = 0;

	if (input_length == 0) { terminal_prompt(); return; }

	/* Copy original input */
	while (fi < input_length && fi + 1 < sizeof(full_copy))
	{
		full_copy[fi] = input_buffer[fi];
		fi++;
	}
	full_copy[fi] = '\0';

	/* Check if there's any chaining operator (fast path) */
	{
		unsigned long ci;
		int has_chain = 0;
		int q = 0;
		for (ci = 0; ci < fi; ci++)
		{
			if (full_copy[ci] == '"') q = !q;
			if (!q)
			{
				if (full_copy[ci] == ';') { has_chain = 1; break; }
				if (full_copy[ci] == '&' && ci + 1 < fi && full_copy[ci + 1] == '&') { has_chain = 1; break; }
				if (full_copy[ci] == '|' && ci + 1 < fi && full_copy[ci + 1] == '|') { has_chain = 1; break; }
			}
		}
		if (!has_chain)
		{
			run_command_single();
			return;
		}
	}

	seg_start = 0;
	in_q = 0;
	suppress_prompt = 1;
	{
		unsigned long ci;
		for (ci = 0; ci <= fi; ci++)
		{
			int is_sep = 0;
			int sep_len = 0;
			int sep_op = 0; /* 0=;  1=&&  2=|| */
			if (ci < fi && full_copy[ci] == '"') in_q = !in_q;
			if (ci == fi) { is_sep = 1; sep_len = 0; sep_op = 0; }
			else if (!in_q)
			{
				if (full_copy[ci] == ';') { is_sep = 1; sep_len = 1; sep_op = 0; }
				else if (full_copy[ci] == '&' && ci + 1 < fi && full_copy[ci + 1] == '&') { is_sep = 1; sep_len = 2; sep_op = 1; }
				else if (full_copy[ci] == '|' && ci + 1 < fi && full_copy[ci + 1] == '|') { is_sep = 1; sep_len = 2; sep_op = 2; }
			}
			if (is_sep)
			{
				/* Extract segment [seg_start..ci) */
				unsigned long si2 = seg_start, di = 0;
				while (si2 < ci && full_copy[si2] == ' ') si2++;
				while (si2 < ci && di + 1 < sizeof(input_buffer))
				{
					input_buffer[di++] = full_copy[si2++];
				}
				while (di > 0 && input_buffer[di - 1] == ' ') di--;
				input_buffer[di] = '\0';
				input_length = di;
				cursor_pos = di;

				if (input_length > 0)
				{
					int skip = 0;
					if (pending_op == 1 && last_exit_code != 0) skip = 1; /* && but prev failed */
					if (pending_op == 2 && last_exit_code == 0) skip = 1; /* || but prev succeeded */
					if (!skip)
						run_command_single();
				}

				pending_op = sep_op;
				seg_start = ci + (unsigned long)sep_len;
				if (sep_len == 2) ci++; /* skip second char of && or || */
			}
		}
	}
	suppress_prompt = 0;

	input_length = 0; input_buffer[0] = '\0';
	if (!editor_active && !script_mode_active) terminal_prompt();
}

static void run_command_single(void)
{
	char expanded[INPUT_BUFFER_SIZE];
	char expanded_glyphs[INPUT_BUFFER_SIZE];
	char resolved[INPUT_BUFFER_SIZE];
	char globbed[INPUT_BUFFER_SIZE];
	unsigned long i;

	if (input_length == 0) { return; }

	if (expand_command_substitutions(input_buffer, expanded, sizeof(expanded)) != 0)
	{
		terminal_write_line("substitution: bad $(...) expression");
		input_length = 0; input_buffer[0] = '\0';
		if (!editor_active && !script_mode_active) terminal_prompt();
		return;
	}
	if (expand_cp437_aliases(expanded, expanded_glyphs, sizeof(expanded_glyphs)) != 0)
	{
		terminal_write_line("glyph escape: bad alias or overflow");
		input_length = 0; input_buffer[0] = '\0';
		if (!editor_active && !script_mode_active) terminal_prompt();
		return;
	}
	if (resolve_command_aliases(expanded_glyphs, resolved, sizeof(resolved)) != 0)
	{
		input_length = 0; input_buffer[0] = '\0';
		if (!editor_active && !script_mode_active) terminal_prompt();
		return;
	}

	/* Expand glob patterns (* and ?) */
	if (expand_globs(resolved, globbed, sizeof(globbed)) != 0)
	{
		/* On overflow, use unglobbed version */
		i = 0;
		while (resolved[i] && i + 1 < sizeof(globbed)) { globbed[i] = resolved[i]; i++; }
		globbed[i] = '\0';
	}

	i = 0;
	while (globbed[i] != '\0' && i + 1 < sizeof(input_buffer))
	{
		input_buffer[i] = globbed[i];
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

	/* --- Background job: trailing & --- */
	{
		unsigned long end = input_length;
		while (end > 0 && input_buffer[end - 1] == ' ') end--;
		if (end > 0 && input_buffer[end - 1] == '&')
		{
			int slot = -1, si;
			char n[16];
			/* Strip the '&' and trailing spaces */
			end--;
			while (end > 0 && input_buffer[end - 1] == ' ') end--;
			input_buffer[end] = '\0';
			input_length = end;

			if (input_length == 0)
			{
				terminal_write_line("bg: empty command");
				input_length = 0; input_buffer[0] = '\0';
				if (!editor_active && !script_mode_active) terminal_prompt();
				return;
			}

			/* Find a free slot */
			for (si = 0; si < BG_JOB_MAX; si++)
			{
				if (!bg_jobs[si].active) { slot = si; break; }
			}
			if (slot < 0)
			{
				terminal_write_line("bg: too many background jobs");
				input_length = 0; input_buffer[0] = '\0';
				if (!editor_active && !script_mode_active) terminal_prompt();
				return;
			}

			/* Copy command into the job slot */
			for (si = 0; (unsigned long)si < input_length && si + 1 < (int)sizeof(bg_jobs[slot].cmd); si++)
				bg_jobs[slot].cmd[si] = input_buffer[si];
			bg_jobs[slot].cmd[si] = '\0';
			bg_jobs[slot].active = 1;

			{
				int tid = task_create("bg_job", bg_task_entry, &bg_jobs[slot]);
				if (tid < 0)
				{
					bg_jobs[slot].active = 0;
					terminal_write_line("bg: failed to create task");
					input_length = 0; input_buffer[0] = '\0';
					if (!editor_active && !script_mode_active) terminal_prompt();
					return;
				}
				bg_jobs[slot].task_id = tid;
				bg_job_count++;
			}

			terminal_write("[");
			uint_to_dec((unsigned long)(slot + 1), n, sizeof(n));
			terminal_write(n);
			terminal_write("] ");
			uint_to_dec((unsigned long)bg_jobs[slot].task_id, n, sizeof(n));
			terminal_write_line(n);

			input_length = 0; input_buffer[0] = '\0';
			if (!editor_active && !script_mode_active) terminal_prompt();
			return;
		}
	}

	/* --- Pipe and redirect handling --- */
	{
		unsigned long pi;
		int has_pipe = 0, has_redir = 0, has_append = 0, has_input_redir = 0;
		unsigned long split_pos = 0, input_redir_pos = 0;
		int in_quote = 0;

		for (pi = 0; pi < input_length; pi++)
		{
			if (input_buffer[pi] == '"') in_quote = !in_quote;
			if (!in_quote)
			{
				if (input_buffer[pi] == '|' && !has_pipe && !has_redir)
				{
					has_pipe = 1;
					split_pos = pi;
				}
				if (input_buffer[pi] == '>' && !has_pipe && !has_redir)
				{
					has_redir = 1;
					split_pos = pi;
					if (pi + 1 < input_length && input_buffer[pi + 1] == '>')
						has_append = 1;
				}
				if (input_buffer[pi] == '<' && !has_pipe && !has_redir && !has_input_redir)
				{
					has_input_redir = 1;
					input_redir_pos = pi;
				}
			}
		}

		/* Handle input redirection: cmd < file */
		if (has_input_redir && !has_pipe)
		{
			char infile[128], full_infile[128];
			static char inredir_buf[FS_MAX_FILE_SIZE];
			unsigned long infile_i = 0, infile_size = 0;

			/* Trim command (left of <) */
			input_buffer[input_redir_pos] = '\0';
			{
				unsigned long li = input_redir_pos;
				while (li > 0 && input_buffer[li - 1] == ' ') input_buffer[--li] = '\0';
				input_length = li;
			}

			/* Extract filename */
			pi = input_redir_pos + 1;
			while (pi < i && input_buffer[pi] == ' ') pi++;
			for (; pi < i && infile_i + 1 < sizeof(infile); pi++)
				infile[infile_i++] = input_buffer[pi];
			while (infile_i > 0 && infile[infile_i - 1] == ' ') infile_i--;
			infile[infile_i] = '\0';

			if (infile[0] == '\0')
			{
				terminal_write_line("redirect: missing input filename");
				input_length = 0; input_buffer[0] = '\0';
				if (!editor_active && !script_mode_active) terminal_prompt();
				return;
			}

			/* Read file into buffer */
			if (fat_mode_active())
			{
				if (fat_resolve_path(infile, full_infile, sizeof(full_infile)) != 0 ||
					fat32_read_file_path(full_infile, (unsigned char *)inredir_buf, sizeof(inredir_buf) - 1, &infile_size) != 0)
				{
					terminal_write("redirect: cannot read "); terminal_write_line(infile);
					input_length = 0; input_buffer[0] = '\0';
					if (!editor_active && !script_mode_active) terminal_prompt();
					return;
				}
			}
			else
			{
				if (fs_read_file(infile, (unsigned char *)inredir_buf, sizeof(inredir_buf) - 1, &infile_size) != 0)
				{
					terminal_write("redirect: cannot read "); terminal_write_line(infile);
					input_length = 0; input_buffer[0] = '\0';
					if (!editor_active && !script_mode_active) terminal_prompt();
					return;
				}
			}
			inredir_buf[infile_size] = '\0';

			pipe_stdin_buf = inredir_buf;
			pipe_stdin_len = infile_size;

			run_command_dispatch();

			pipe_stdin_buf = (void *)0;
			pipe_stdin_len = 0;

			input_length = 0; input_buffer[0] = '\0';
			task_reap_zombies();
			if (!editor_active && !script_mode_active) terminal_prompt();
			return;
		}

		if (has_pipe)
		{
			char right_cmd[INPUT_BUFFER_SIZE];
			static char pipe_buf[FS_MAX_FILE_SIZE];
			unsigned long ri = 0;

			/* Trim left command */
			input_buffer[split_pos] = '\0';
			{
				unsigned long li = split_pos;
				while (li > 0 && input_buffer[li - 1] == ' ') input_buffer[--li] = '\0';
				input_length = li;
			}

			/* Extract right command */
			pi = split_pos + 1;
			while (pi < i && input_buffer[pi] == ' ') pi++;
			for (; pi < i && ri + 1 < sizeof(right_cmd); pi++)
				right_cmd[ri++] = input_buffer[pi];
			while (ri > 0 && right_cmd[ri - 1] == ' ') ri--;
			right_cmd[ri] = '\0';

			/* Run left command with output capture */
			terminal_output_capture = 1;
			terminal_output_buf = pipe_buf;
			terminal_output_buf_size = sizeof(pipe_buf);
			terminal_output_buf_len = 0;
			pipe_buf[0] = '\0';

			run_command_dispatch();

			terminal_output_capture = 0;
			terminal_output_buf = (void *)0;

			/* Make captured output available as pipe stdin */
			pipe_stdin_buf = pipe_buf;
			pipe_stdin_len = terminal_output_buf_len;

			/* Set up right command — may contain further pipes */
			for (pi = 0; right_cmd[pi] && pi + 1 < sizeof(input_buffer); pi++)
				input_buffer[pi] = right_cmd[pi];
			input_buffer[pi] = '\0';
			input_length = pi;

			/* Check if right side has another pipe */
			{
				unsigned long pi2;
				int more_pipe = 0, q2 = 0;
				unsigned long sp2 = 0;
				for (pi2 = 0; pi2 < input_length; pi2++)
				{
					if (input_buffer[pi2] == '"') q2 = !q2;
					if (!q2 && input_buffer[pi2] == '|') { more_pipe = 1; sp2 = pi2; break; }
				}
				if (more_pipe)
				{
					/* Multi-pipe: recurse by extracting next segment */
					static char pipe_buf2[FS_MAX_FILE_SIZE];
					char right2[INPUT_BUFFER_SIZE];
					unsigned long r2i = 0;

					/* Trim current left part */
					input_buffer[sp2] = '\0';
					{
						unsigned long li2 = sp2;
						while (li2 > 0 && input_buffer[li2 - 1] == ' ') input_buffer[--li2] = '\0';
						input_length = li2;
					}

					/* Extract the rest of the pipeline */
					pi2 = sp2 + 1;
					while (pi2 < pi && input_buffer[pi2] == ' ') pi2++;
					for (; pi2 < pi && r2i + 1 < sizeof(right2); pi2++)
						right2[r2i++] = input_buffer[pi2];
					while (r2i > 0 && right2[r2i - 1] == ' ') r2i--;
					right2[r2i] = '\0';

					/* Run the current middle command with capture */
					terminal_output_capture = 1;
					terminal_output_buf = pipe_buf2;
					terminal_output_buf_size = sizeof(pipe_buf2);
					terminal_output_buf_len = 0;
					pipe_buf2[0] = '\0';

					run_command_dispatch();

					terminal_output_capture = 0;
					terminal_output_buf = (void *)0;

					/* Update pipe stdin with the new captured output */
					pipe_stdin_buf = pipe_buf2;
					pipe_stdin_len = terminal_output_buf_len;

					/* Set up the final (or next) command */
					for (pi2 = 0; right2[pi2] && pi2 + 1 < sizeof(input_buffer); pi2++)
						input_buffer[pi2] = right2[pi2];
					input_buffer[pi2] = '\0';
					input_length = pi2;
				}
			}

			run_command_dispatch();
			pipe_stdin_buf = (void *)0;
			pipe_stdin_len = 0;

			input_length = 0; input_buffer[0] = '\0';
			task_reap_zombies();
			if (!editor_active && !script_mode_active) terminal_prompt();
			return;
		}

		if (has_redir)
		{
			char redir_file[128];
			static char redir_buf[FS_MAX_FILE_SIZE];
			unsigned long ri2 = 0;

			/* Trim left command */
			input_buffer[split_pos] = '\0';
			{
				unsigned long li = split_pos;
				while (li > 0 && input_buffer[li - 1] == ' ') input_buffer[--li] = '\0';
				input_length = li;
			}

			/* Extract filename */
			pi = split_pos + (has_append ? 2 : 1);
			while (pi < i && input_buffer[pi] == ' ') pi++;
			for (; pi < i && ri2 + 1 < sizeof(redir_file); pi++)
				redir_file[ri2++] = input_buffer[pi];
			while (ri2 > 0 && redir_file[ri2 - 1] == ' ') ri2--;
			redir_file[ri2] = '\0';

			if (redir_file[0] == '\0')
			{
				terminal_write_line("redirect: missing filename");
				input_length = 0; input_buffer[0] = '\0';
				if (!editor_active && !script_mode_active) terminal_prompt();
				return;
			}

			/* Run command with output capture */
			terminal_output_capture = 1;
			terminal_output_buf = redir_buf;
			terminal_output_buf_size = sizeof(redir_buf);
			terminal_output_buf_len = 0;
			redir_buf[0] = '\0';

			run_command_dispatch();

			terminal_output_capture = 0;
			terminal_output_buf = (void *)0;

			/* Write captured output to file */
			if (fat_mode_active())
			{
				char full_redir[128];
				if (fat_resolve_path(redir_file, full_redir, sizeof(full_redir)) != 0)
				{
					terminal_write_line("redirect: cannot resolve FAT path");
				}
				else if (has_append)
				{
					unsigned char existing[FS_MAX_FILE_SIZE];
					unsigned long existing_size = 0;
					fat32_read_file_path(full_redir, existing, sizeof(existing) - terminal_output_buf_len - 1, &existing_size);
					{
						unsigned long k;
						for (k = 0; k < terminal_output_buf_len && existing_size + k < sizeof(existing); k++)
							existing[existing_size + k] = (unsigned char)redir_buf[k];
						fat32_write_file_path(full_redir, existing, existing_size + k);
					}
				}
				else
				{
					fat32_write_file_path(full_redir, (const unsigned char *)redir_buf, terminal_output_buf_len);
				}
			}
			else
			{
				if (has_append)
				{
					unsigned char existing[FS_MAX_FILE_SIZE];
					unsigned long existing_size = 0;
					fs_read_file(redir_file, existing, sizeof(existing) - terminal_output_buf_len - 1, &existing_size);
					{
						unsigned long k;
						for (k = 0; k < terminal_output_buf_len && existing_size + k < sizeof(existing); k++)
							existing[existing_size + k] = (unsigned char)redir_buf[k];
						fs_write_file(redir_file, existing, existing_size + k);
					}
				}
				else
				{
					fs_write_file(redir_file, (const unsigned char *)redir_buf, terminal_output_buf_len);
				}
			}

			input_length = 0; input_buffer[0] = '\0';
			task_reap_zombies();
			if (!editor_active && !script_mode_active) terminal_prompt();
			return;
		}
	}

	run_command_dispatch();

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
	/* History expansion: !! = last command, !N = Nth history entry */
	if (input_buffer[0] == '!')
	{
		if (input_buffer[1] == '!' && (input_buffer[2] == '\0' || input_buffer[2] == ' '))
		{
			if (history_count >= 2)
			{
				int last_idx = (history_start + history_count - 2) % HISTORY_SIZE;
				char suffix[INPUT_BUFFER_SIZE];
				unsigned long si = 0, di = 0;
				if (input_buffer[2] == ' ')
				{
					unsigned long k = 3;
					while (input_buffer[k] != '\0' && si + 1 < sizeof(suffix)) suffix[si++] = input_buffer[k++];
				}
				suffix[si] = '\0';
				for (di = 0; history[last_idx][di] != '\0' && di + 1 < sizeof(input_buffer); di++)
					input_buffer[di] = history[last_idx][di];
				if (si > 0 && di + 1 + si < sizeof(input_buffer))
				{
					input_buffer[di++] = ' ';
					unsigned long k;
					for (k = 0; k < si; k++) input_buffer[di++] = suffix[k];
				}
				input_buffer[di] = '\0';
				input_length = di;
				terminal_write(input_buffer);
				terminal_putc('\n');
			}
			else { terminal_write_line("!!: no previous command"); input_length = 0; input_buffer[0] = '\0'; terminal_prompt(); return; }
		}
		else if (input_buffer[1] >= '1' && input_buffer[1] <= '9')
		{
			int n = 0;
			unsigned long pi = 1;
			while (input_buffer[pi] >= '0' && input_buffer[pi] <= '9') n = n * 10 + (input_buffer[pi++] - '0');
			if (n >= 1 && n <= history_count - 1)
			{
				int idx = (history_start + n - 1) % HISTORY_SIZE;
				unsigned long di = 0;
				for (di = 0; history[idx][di] != '\0' && di + 1 < sizeof(input_buffer); di++)
					input_buffer[di] = history[idx][di];
				input_buffer[di] = '\0';
				input_length = di;
				terminal_write(input_buffer);
				terminal_putc('\n');
			}
			else { terminal_write_line("!: event not found"); input_length = 0; input_buffer[0] = '\0'; terminal_prompt(); return; }
		}
	}
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
	if (scancode == 0x01) { panic_esc_held = 1; update_panic_hotkey(); }
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

	/* --- Reverse-search mode intercept --- */
	if (rsearch_active)
	{
		/* Let modifier keys pass through */
		if (scancode == 0xE0) { extended_key = 1; return; }
		if (scancode == 0x1D || scancode == 0x9D) { ctrl_held = (scancode == 0x1D); return; }
		if (scancode == 0x2A || scancode == 0x36) { shift_held = 1; return; }
		if (scancode == 0xAA || scancode == 0xB6) { shift_held = 0; return; }
		if (scancode >= 0x80) return; /* key release */
		if (extended_key) { extended_key = 0;
			if (scancode == 0x1D) { ctrl_held = 1; return; }
			if (scancode == 0x9D) { ctrl_held = 0; return; }
			return; /* ignore other extended keys in search */
		}

		/* Ctrl+R again: search for next older match */
		if (ctrl_held && scancode == 0x13)
		{
			int from = rsearch_match > 0 ? rsearch_match - 1 : -1;
			if (from >= 0) { rsearch_match = rsearch_find(from); }
			rsearch_redraw();
			return;
		}
		/* Esc: cancel search */
		if (scancode == 0x01)
		{
			rsearch_cancel();
			return;
		}
		/* Enter: accept match and submit */
		if (scancode == 0x1C)
		{
			rsearch_accept();
			submit_current_line();
			return;
		}
		/* Backspace: remove last char from search term */
		if (scancode == 0x0E)
		{
			if (rsearch_len > 0)
			{
				rsearch_len--;
				rsearch_term[rsearch_len] = '\0';
				rsearch_match = rsearch_find(history_count - 1);
			}
			rsearch_redraw();
			return;
		}
		/* Ctrl+C / Ctrl+G: cancel */
		if (ctrl_held && (scancode == 0x2E || scancode == 0x22))
		{
			rsearch_cancel();
			return;
		}
		/* Printable character: append to search term */
		c = translate_scancode(scancode);
		if (c != '\0' && rsearch_len < 62)
		{
			rsearch_term[rsearch_len++] = c;
			rsearch_term[rsearch_len] = '\0';
			rsearch_match = rsearch_find(history_count - 1);
			rsearch_redraw();
			return;
		}
		/* Any other key: accept and let it through */
		rsearch_accept();
		/* fall through to normal processing */
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
		if (scancode == 0x26) /* Ctrl+L — clear screen */
		{
			screen_clear();
			input_buffer[input_length] = '\0';
			terminal_prompt();
			{
				unsigned long ci;
				for (ci = 0; ci < input_length; ci++)
					screen_write_char_at((unsigned short)(prompt_vga_start + ci), input_buffer[ci]);
			}
			cursor_pos = input_length;
			sync_screen_pos();
			screen_set_hw_cursor((unsigned short)(prompt_vga_start + cursor_pos));
			return;
		}
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
		if (scancode == 0x16) /* Ctrl+U — kill line before cursor */
		{
			if (cursor_pos > 0)
				terminal_delete_range(0, cursor_pos);
			return;
		}
		if (scancode == 0x25) /* Ctrl+K — kill line after cursor */
		{
			if (cursor_pos < input_length)
				terminal_delete_range(cursor_pos, input_length);
			return;
		}
		if (scancode == 0x11) /* Ctrl+W — delete word before cursor */
		{
			if (cursor_pos > 0)
			{
				unsigned long wp = cursor_pos;
				while (wp > 0 && input_buffer[wp - 1] == ' ') wp--;
				while (wp > 0 && input_buffer[wp - 1] != ' ') wp--;
				terminal_delete_range(wp, cursor_pos);
			}
			return;
		}
		if (scancode == 0x13) /* Ctrl+R — reverse history search */
		{
			rsearch_active = 1;
			rsearch_len = 0;
			rsearch_term[0] = '\0';
			rsearch_match = -1;
			rsearch_redraw();
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
	terminal_autorun_boot_begin();
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
	/* When the GUI event loop is running, it owns the scancode queue */
	if (!gui_active)
	{
		while (scancode_queue_tail != scancode_queue_head)
		{
			unsigned char sc = scancode_queue[scancode_queue_tail];
			scancode_queue_tail = (scancode_queue_tail + 1) % SCANCODE_QUEUE_SIZE;
			handle_scancode(sc);
		}
	}
	terminal_autorun_boot_heartbeat();
	terminal_poll_boot_autorun();
	net_poll();
}

int terminal_input_available(void)
{
	return scancode_queue_tail != scancode_queue_head;
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

