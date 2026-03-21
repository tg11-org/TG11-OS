#include "screen.h"
#include "arch.h"
#include "framebuffer.h"

static volatile unsigned short *const VGA = (unsigned short *)0xB8000;
static volatile unsigned char *const VGA_FONT = (unsigned char *)0xA0000;

#define FB_FONT_W    8
#define FB_MAX_COLS 160
#define FB_MAX_ROWS 80
#define FB_GLYPH_FIRST 32
#define FB_GLYPH_LAST 126
#define FB_GLYPH_COUNT (FB_GLYPH_LAST - FB_GLYPH_FIRST + 1)

#define FB_FONT_STYLE_CLASSIC 0
#define FB_FONT_STYLE_BLOCKY  1

enum screen_backend
{
	SCREEN_BACKEND_VGA = 0,
	SCREEN_BACKEND_FB = 1
};

static enum screen_backend backend = SCREEN_BACKEND_VGA;
static unsigned long vga_width = 80;
static unsigned long vga_height = 25;
static unsigned long fb_cols = 0;
static unsigned long fb_rows = 0;

static unsigned long screen_row = 0;
static unsigned long screen_col = 0;
static unsigned char screen_color = 0x0F;

static int vga_font_saved = 0;
static unsigned char vga_font_16[256][16];
static unsigned char vga_font_8[256][8];

static volatile unsigned char *fb_base = (void *)0;
static unsigned int fb_pitch = 0;
static unsigned int fb_w = 0;
static unsigned int fb_h = 0;
static unsigned int fb_bpp = 0;
static unsigned int fb_cell_h = 14;
static unsigned int fb_cursor_offset = 0xFFFFFFFFU;
static char fb_chars[FB_MAX_COLS * FB_MAX_ROWS];
static unsigned char fb_attrs[FB_MAX_COLS * FB_MAX_ROWS];
static int fb_font_style = FB_FONT_STYLE_CLASSIC;
static unsigned char fb_custom_valid[FB_GLYPH_COUNT];
static unsigned char fb_custom_rows[FB_GLYPH_COUNT][7];

static const unsigned int vga_palette_rgb[16] = {
	0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
	0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
	0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
	0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

static unsigned long active_width(void)
{
	return backend == SCREEN_BACKEND_FB ? fb_cols : vga_width;
}

static unsigned long active_height(void)
{
	return backend == SCREEN_BACKEND_FB ? fb_rows : vga_height;
}

static void vga_seq_write(unsigned char index, unsigned char value)
{
	arch_outb(0x3C4, index);
	arch_outb(0x3C5, value);
}

static void vga_gc_write(unsigned char index, unsigned char value)
{
	arch_outb(0x3CE, index);
	arch_outb(0x3CF, value);
}

static void vga_crtc_write(unsigned char index, unsigned char value)
{
	arch_outb(0x3D4, index);
	arch_outb(0x3D5, value);
}

static void vga_font_access_begin(void)
{
	vga_seq_write(0x00, 0x01);
	vga_seq_write(0x02, 0x04);
	vga_seq_write(0x04, 0x07);
	vga_gc_write(0x04, 0x02);
	vga_gc_write(0x05, 0x00);
	vga_gc_write(0x06, 0x00);
	vga_seq_write(0x00, 0x03);
}

static void vga_font_access_end(void)
{
	vga_seq_write(0x00, 0x01);
	vga_seq_write(0x02, 0x03);
	vga_seq_write(0x04, 0x03);
	vga_gc_write(0x04, 0x00);
	vga_gc_write(0x05, 0x10);
	vga_gc_write(0x06, 0x0E);
	vga_seq_write(0x00, 0x03);
}

static void vga_capture_font_16(void)
{
	unsigned long ch;
	unsigned long row;
	vga_font_access_begin();
	for (ch = 0; ch < 256; ch++)
	{
		for (row = 0; row < 16; row++) vga_font_16[ch][row] = VGA_FONT[ch * 32 + row];
	}
	vga_font_access_end();
	vga_font_saved = 1;
}

static void vga_build_font_8_from_16(void)
{
	unsigned long ch;
	unsigned long row;
	for (ch = 0; ch < 256; ch++)
	{
		for (row = 0; row < 8; row++) vga_font_8[ch][row] = (unsigned char)(vga_font_16[ch][row * 2] | vga_font_16[ch][row * 2 + 1]);
	}
}

static void vga_load_font_8(void)
{
	unsigned long ch;
	unsigned long row;
	vga_font_access_begin();
	for (ch = 0; ch < 256; ch++)
	{
		for (row = 0; row < 8; row++) VGA_FONT[ch * 32 + row] = vga_font_8[ch][row];
		for (row = 8; row < 32; row++) VGA_FONT[ch * 32 + row] = 0;
	}
	vga_font_access_end();
}

static void vga_restore_font_16(void)
{
	unsigned long ch;
	unsigned long row;
	if (!vga_font_saved) return;
	vga_font_access_begin();
	for (ch = 0; ch < 256; ch++)
	{
		for (row = 0; row < 16; row++) VGA_FONT[ch * 32 + row] = vga_font_16[ch][row];
		for (row = 16; row < 32; row++) VGA_FONT[ch * 32 + row] = 0;
	}
	vga_font_access_end();
}

static void vga_set_cursor_shape(unsigned char start, unsigned char end)
{
	vga_crtc_write(0x0A, start);
	vga_crtc_write(0x0B, end);
}

static void vga_hide_cursor(void)
{
	vga_crtc_write(0x0A, 0x20);
}

static void vga_set_scanline_height(unsigned char max_scanline)
{
	vga_crtc_write(0x09, max_scanline);
}

static void fb_plot(unsigned int x, unsigned int y, unsigned int rgb)
{
	volatile unsigned char *p;
	if (fb_base == (void *)0 || x >= fb_w || y >= fb_h) return;
	p = fb_base + y * fb_pitch;
	if (fb_bpp == 32)
	{
		((volatile unsigned int *)p)[x] = rgb;
	}
	else if (fb_bpp == 24)
	{
		p += x * 3;
		p[0] = (unsigned char)(rgb & 0xFF);
		p[1] = (unsigned char)((rgb >> 8) & 0xFF);
		p[2] = (unsigned char)((rgb >> 16) & 0xFF);
	}
}

static void fb_fill_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int rgb)
{
	unsigned int yy;
	unsigned int xx;
	for (yy = 0; yy < h; yy++)
	{
		for (xx = 0; xx < w; xx++) fb_plot(x + xx, y + yy, rgb);
	}
}

static char fb_font_char_for_display(char ch)
{
	if (ch < 32 || ch > 126) return '?';
	return ch;
}

static int fb_char_index(char ch)
{
	if ((unsigned char)ch < FB_GLYPH_FIRST || (unsigned char)ch > FB_GLYPH_LAST) return -1;
	return (int)((unsigned char)ch - FB_GLYPH_FIRST);
}

static unsigned char fb_builtin_pattern_row(char ch, unsigned int row)
{
	static const unsigned char qmark[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
	if (row >= 7) return 0;
	/* CP437 box-drawing, block, shade, and arrow characters (bypass ASCII filter) */
	{
		unsigned char uch = (unsigned char)ch;
		switch (uch)
		{
		/* Box-drawing singles */
		case 0xC4: { static const unsigned char g[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}; return g[row]; } /* ─ boxh  */
		case 0xB3: { static const unsigned char g[7] = {0x04,0x04,0x04,0x04,0x04,0x04,0x04}; return g[row]; } /* │ boxv  */
		case 0xDA: { static const unsigned char g[7] = {0x00,0x00,0x00,0x07,0x04,0x04,0x04}; return g[row]; } /* ┌ boxul */
		case 0xBF: { static const unsigned char g[7] = {0x00,0x00,0x00,0x1C,0x04,0x04,0x04}; return g[row]; } /* ┐ boxur */
		case 0xC0: { static const unsigned char g[7] = {0x04,0x04,0x04,0x07,0x00,0x00,0x00}; return g[row]; } /* └ boxll */
		case 0xD9: { static const unsigned char g[7] = {0x04,0x04,0x04,0x1C,0x00,0x00,0x00}; return g[row]; } /* ┘ boxlr */
		case 0xC2: { static const unsigned char g[7] = {0x00,0x00,0x00,0x1F,0x04,0x04,0x04}; return g[row]; } /* ┬ boxt  */
		case 0xC1: { static const unsigned char g[7] = {0x04,0x04,0x04,0x1F,0x00,0x00,0x00}; return g[row]; } /* ┴ boxb  */
		case 0xC3: { static const unsigned char g[7] = {0x04,0x04,0x04,0x07,0x04,0x04,0x04}; return g[row]; } /* ├ boxl  */
		case 0xB4: { static const unsigned char g[7] = {0x04,0x04,0x04,0x1C,0x04,0x04,0x04}; return g[row]; } /* ┤ boxr  */
		case 0xC5: { static const unsigned char g[7] = {0x04,0x04,0x04,0x1F,0x04,0x04,0x04}; return g[row]; } /* ┼ boxx  */
		/* Block elements */
		case 0xDB: { static const unsigned char g[7] = {0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F}; return g[row]; } /* █ blk   */
		case 0xDF: { static const unsigned char g[7] = {0x1F,0x1F,0x1F,0x00,0x00,0x00,0x00}; return g[row]; } /* ▀ blkup */
		case 0xDC: { static const unsigned char g[7] = {0x00,0x00,0x00,0x1F,0x1F,0x1F,0x1F}; return g[row]; } /* ▄ blkdn */
		case 0xDD: { static const unsigned char g[7] = {0x1C,0x1C,0x1C,0x1C,0x1C,0x1C,0x1C}; return g[row]; } /* ▌ blkl  */
		case 0xDE: { static const unsigned char g[7] = {0x07,0x07,0x07,0x07,0x07,0x07,0x07}; return g[row]; } /* ▐ blkr  */
		/* Box-drawing doubles */
		case 0xCD: { static const unsigned char g[7] = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}; return g[row]; } /* ═ dboxh  */
		case 0xBA: { static const unsigned char g[7] = {0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0A}; return g[row]; } /* ║ dboxv  */
		case 0xC9: { static const unsigned char g[7] = {0x00,0x00,0x1E,0x18,0x18,0x18,0x18}; return g[row]; } /* ╔ dboxul */
		case 0xBB: { static const unsigned char g[7] = {0x00,0x00,0x0F,0x03,0x03,0x03,0x03}; return g[row]; } /* ╗ dboxur */
		case 0xC8: { static const unsigned char g[7] = {0x18,0x18,0x18,0x18,0x1E,0x00,0x00}; return g[row]; } /* ╚ dboxll */
		case 0xBC: { static const unsigned char g[7] = {0x03,0x03,0x03,0x03,0x0F,0x00,0x00}; return g[row]; } /* ╝ dboxlr */
		/* Shade elements */
		case 0xB0: { static const unsigned char g[7] = {0x15,0x0A,0x15,0x0A,0x15,0x0A,0x15}; return g[row]; } /* ░ shade1 */
		case 0xB1: { static const unsigned char g[7] = {0x1F,0x0A,0x1F,0x0A,0x1F,0x0A,0x1F}; return g[row]; } /* ▒ shade2 */
		case 0xB2: { static const unsigned char g[7] = {0x1F,0x1B,0x1F,0x1B,0x1F,0x1B,0x1F}; return g[row]; } /* ▓ shade3 */
		/* Utility symbols */
		case 0xF8: { static const unsigned char g[7] = {0x06,0x09,0x09,0x06,0x00,0x00,0x00}; return g[row]; } /* ° deg */
		case 0xF1: { static const unsigned char g[7] = {0x04,0x04,0x1F,0x04,0x1F,0x00,0x00}; return g[row]; } /* ± pm  */
		case 0xFA: { static const unsigned char g[7] = {0x00,0x00,0x00,0x00,0x00,0x04,0x00}; return g[row]; } /* · dot */
		/* Arrow/triangle characters (< 32, also bypassed by ASCII filter) */
		case 0x18: { static const unsigned char g[7] = {0x04,0x0E,0x1F,0x04,0x04,0x04,0x04}; return g[row]; } /* ↑ arru  */
		case 0x19: { static const unsigned char g[7] = {0x04,0x04,0x04,0x04,0x1F,0x0E,0x04}; return g[row]; } /* ↓ arrd  */
		case 0x1B: { static const unsigned char g[7] = {0x00,0x10,0x18,0x1F,0x18,0x10,0x00}; return g[row]; } /* ← arrl  */
		case 0x1A: { static const unsigned char g[7] = {0x00,0x01,0x03,0x1F,0x03,0x01,0x00}; return g[row]; } /* → arrr  */
		case 0x1E: { static const unsigned char g[7] = {0x00,0x04,0x0E,0x1F,0x00,0x00,0x00}; return g[row]; } /* ▲ tri   */
		default: break;
		}
	}
	ch = fb_font_char_for_display(ch);
	{
		int idx = fb_char_index(ch);
		if (idx >= 0 && fb_custom_valid[idx]) return (unsigned char)(fb_custom_rows[idx][row] & 0x1F);
	}
	switch (ch)
	{
	case ' ':  { static const unsigned char g[7] = {0,0,0,0,0,0,0}; return g[row]; }
	case '!':  { static const unsigned char g[7] = {0x04,0x04,0x04,0x04,0x04,0x00,0x04}; return g[row]; }
	case '"':  { static const unsigned char g[7] = {0x0A,0x0A,0x04,0x00,0x00,0x00,0x00}; return g[row]; }
	case '`':  { static const unsigned char g[7] = {0x00,0x04,0x04,0x02,0x00,0x00,0x00}; return g[row]; }
	case '#':  { static const unsigned char g[7] = {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00}; return g[row]; }
	case '$':  { static const unsigned char g[7] = {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}; return g[row]; }
	case '%':  { static const unsigned char g[7] = {0x18,0x19,0x02,0x04,0x08,0x13,0x03}; return g[row]; }
	case '&':  { static const unsigned char g[7] = {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}; return g[row]; }
	case '\'': { static const unsigned char g[7] = {0x04,0x04,0x02,0x00,0x00,0x00,0x00}; return g[row]; }
	case '(':  { static const unsigned char g[7] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02}; return g[row]; }
	case ')':  { static const unsigned char g[7] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08}; return g[row]; }
	case '*':  { static const unsigned char g[7] = {0x00,0x15,0x0E,0x1F,0x0E,0x15,0x00}; return g[row]; }
	case '+':  { static const unsigned char g[7] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}; return g[row]; }
	case ',':  { static const unsigned char g[7] = {0x00,0x00,0x00,0x00,0x04,0x04,0x08}; return g[row]; }
	case '-':  { static const unsigned char g[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}; return g[row]; }
	case '~':  { static const unsigned char g[7] = {0x00,0x00,0x0C,0x11,0x06,0x00,0x00}; return g[row]; }
	case '.':  { static const unsigned char g[7] = {0x00,0x00,0x00,0x00,0x00,0x04,0x04}; return g[row]; }
	case '/':  { static const unsigned char g[7] = {0x01,0x02,0x04,0x04,0x08,0x10,0x00}; return g[row]; }
	case '0':  { static const unsigned char g[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; return g[row]; }
	case '1':  { static const unsigned char g[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; return g[row]; }
	case '2':  { static const unsigned char g[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; return g[row]; }
	case '3':  { static const unsigned char g[7] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}; return g[row]; }
	case '4':  { static const unsigned char g[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; return g[row]; }
	case '5':  { static const unsigned char g[7] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}; return g[row]; }
	case '6':  { static const unsigned char g[7] = {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}; return g[row]; }
	case '7':  { static const unsigned char g[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; return g[row]; }
	case '8':  { static const unsigned char g[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; return g[row]; }
	case '9':  { static const unsigned char g[7] = {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}; return g[row]; }
	case ':':  { static const unsigned char g[7] = {0x00,0x04,0x04,0x00,0x04,0x04,0x00}; return g[row]; }
	case ';':  { static const unsigned char g[7] = {0x00,0x04,0x04,0x00,0x04,0x04,0x08}; return g[row]; }
	case '<':  { static const unsigned char g[7] = {0x02,0x04,0x08,0x10,0x08,0x04,0x02}; return g[row]; }
	case '=':  { static const unsigned char g[7] = {0x00,0x1F,0x00,0x1F,0x00,0x00,0x00}; return g[row]; }
	case '>':  { static const unsigned char g[7] = {0x08,0x04,0x02,0x01,0x02,0x04,0x08}; return g[row]; }
	case '?':  return qmark[row];
	case '@':  { static const unsigned char g[7] = {0x0E,0x11,0x17,0x15,0x17,0x10,0x0F}; return g[row]; }
	case '[':  { static const unsigned char g[7] = {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}; return g[row]; }
	case '\\': { static const unsigned char g[7] = {0x10,0x08,0x04,0x04,0x02,0x01,0x00}; return g[row]; }
	case ']':  { static const unsigned char g[7] = {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}; return g[row]; }
	case '^':  { static const unsigned char g[7] = {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}; return g[row]; }
	case '_':  { static const unsigned char g[7] = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}; return g[row]; }
	case '|':  { static const unsigned char g[7] = {0x04,0x04,0x04,0x04,0x04,0x04,0x04}; return g[row]; }
	case '{':  { static const unsigned char g[7] = {0x02,0x04,0x04,0x08,0x04,0x04,0x02}; return g[row]; }
	case '}':  { static const unsigned char g[7] = {0x08,0x04,0x04,0x02,0x04,0x04,0x08}; return g[row]; }
	case 'a':  { static const unsigned char g[7] = {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}; return g[row]; }
	case 'b':  { static const unsigned char g[7] = {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}; return g[row]; }
	case 'c':  { static const unsigned char g[7] = {0x00,0x00,0x0E,0x11,0x10,0x11,0x0E}; return g[row]; }
	case 'd':  { static const unsigned char g[7] = {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}; return g[row]; }
	case 'e':  { static const unsigned char g[7] = {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}; return g[row]; }
	case 'f':  { static const unsigned char g[7] = {0x06,0x08,0x1E,0x08,0x08,0x08,0x08}; return g[row]; }
	case 'g':  { static const unsigned char g[7] = {0x00,0x00,0x0F,0x11,0x11,0x0F,0x01}; return g[row]; }
	case 'h':  { static const unsigned char g[7] = {0x10,0x10,0x1E,0x11,0x11,0x11,0x11}; return g[row]; }
	case 'i':  { static const unsigned char g[7] = {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}; return g[row]; }
	case 'j':  { static const unsigned char g[7] = {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}; return g[row]; }
	case 'k':  { static const unsigned char g[7] = {0x10,0x10,0x12,0x14,0x18,0x14,0x12}; return g[row]; }
	case 'l':  { static const unsigned char g[7] = {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}; return g[row]; }
	case 'm':  { static const unsigned char g[7] = {0x00,0x00,0x1A,0x15,0x15,0x15,0x15}; return g[row]; }
	case 'n':  { static const unsigned char g[7] = {0x00,0x00,0x1E,0x11,0x11,0x11,0x11}; return g[row]; }
	case 'o':  { static const unsigned char g[7] = {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}; return g[row]; }
	case 'p':  { static const unsigned char g[7] = {0x00,0x00,0x1E,0x11,0x11,0x1E,0x10}; return g[row]; }
	case 'q':  { static const unsigned char g[7] = {0x00,0x00,0x0F,0x11,0x11,0x0F,0x01}; return g[row]; }
	case 'r':  { static const unsigned char g[7] = {0x00,0x00,0x16,0x19,0x10,0x10,0x10}; return g[row]; }
	case 's':  { static const unsigned char g[7] = {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}; return g[row]; }
	case 't':  { static const unsigned char g[7] = {0x08,0x08,0x1E,0x08,0x08,0x08,0x06}; return g[row]; }
	case 'u':  { static const unsigned char g[7] = {0x00,0x00,0x11,0x11,0x11,0x13,0x0D}; return g[row]; }
	case 'v':  { static const unsigned char g[7] = {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}; return g[row]; }
	case 'w':  { static const unsigned char g[7] = {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}; return g[row]; }
	case 'x':  { static const unsigned char g[7] = {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}; return g[row]; }
	case 'y':  { static const unsigned char g[7] = {0x00,0x00,0x11,0x11,0x11,0x0F,0x01}; return g[row]; }
	case 'z':  { static const unsigned char g[7] = {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}; return g[row]; }
	case 'A':  { static const unsigned char g[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; return g[row]; }
	case 'B':  { static const unsigned char g[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; return g[row]; }
	case 'C':  { static const unsigned char g[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}; return g[row]; }
	case 'D':  { static const unsigned char g[7] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}; return g[row]; }
	case 'E':  { static const unsigned char g[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; return g[row]; }
	case 'F':  { static const unsigned char g[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; return g[row]; }
	case 'G':  { static const unsigned char g[7] = {0x0E,0x11,0x10,0x10,0x13,0x11,0x0E}; return g[row]; }
	case 'H':  { static const unsigned char g[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; return g[row]; }
	case 'I':  { static const unsigned char g[7] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}; return g[row]; }
	case 'J':  { static const unsigned char g[7] = {0x01,0x01,0x01,0x01,0x11,0x11,0x0E}; return g[row]; }
	case 'K':  { static const unsigned char g[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11}; return g[row]; }
	case 'L':  { static const unsigned char g[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; return g[row]; }
	case 'M':  { static const unsigned char g[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; return g[row]; }
	case 'N':  { static const unsigned char g[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11}; return g[row]; }
	case 'O':  { static const unsigned char g[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; return g[row]; }
	case 'P':  { static const unsigned char g[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; return g[row]; }
	case 'Q':  { static const unsigned char g[7] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}; return g[row]; }
	case 'R':  { static const unsigned char g[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; return g[row]; }
	case 'S':  { static const unsigned char g[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; return g[row]; }
	case 'T':  { static const unsigned char g[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; return g[row]; }
	case 'U':  { static const unsigned char g[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; return g[row]; }
	case 'V':  { static const unsigned char g[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}; return g[row]; }
	case 'W':  { static const unsigned char g[7] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}; return g[row]; }
	case 'X':  { static const unsigned char g[7] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}; return g[row]; }
	case 'Y':  { static const unsigned char g[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; return g[row]; }
	case 'Z':  { static const unsigned char g[7] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}; return g[row]; }
	default: return qmark[row];
	}
}

static unsigned char fb_expand_row_5_to_8(unsigned char bits5)
{
	unsigned char out = 0;
	if (fb_font_style == FB_FONT_STYLE_BLOCKY)
	{
		if (bits5 & 0x10) out |= 0xE0;
		if (bits5 & 0x08) out |= 0x38;
		if (bits5 & 0x04) out |= 0x1C;
		if (bits5 & 0x02) out |= 0x0E;
		if (bits5 & 0x01) out |= 0x07;
	}
	else
	{
		if (bits5 & 0x10) out |= 0xC0;
		if (bits5 & 0x08) out |= 0x20;
		if (bits5 & 0x04) out |= 0x18;
		if (bits5 & 0x02) out |= 0x04;
		if (bits5 & 0x01) out |= 0x03;
	}
	return out;
}

static unsigned char fb_builtin_font_bits(char ch, unsigned long gy)
{
	unsigned int top_pad = 1;
	unsigned int text_rows;
	unsigned int row;
	unsigned char bits = 0;
	if (fb_cell_h <= 2) return 0;
	text_rows = fb_cell_h - 2;
	if (gy < top_pad || gy >= top_pad + text_rows) return 0;
	row = (unsigned int)(((gy - top_pad) * 7UL) / text_rows);
	if (row < 7) bits = fb_builtin_pattern_row(ch, row);
	return fb_expand_row_5_to_8(bits);
}

static void fb_draw_cursor_overlay(unsigned long offset)
{
	unsigned long row;
	unsigned long col;
	unsigned int fg;
	unsigned long gy;
	unsigned long gx;
	unsigned char attr;

	if (offset >= fb_cols * fb_rows) return;
	row = offset / fb_cols;
	col = offset % fb_cols;
	attr = fb_attrs[offset];
	fg = vga_palette_rgb[attr & 0x0F];

	for (gy = fb_cell_h - 2; gy < fb_cell_h; gy++)
	{
		for (gx = 0; gx < FB_FONT_W; gx++)
		{
			fb_plot((unsigned int)(col * FB_FONT_W + gx), (unsigned int)(row * fb_cell_h + gy), fg);
		}
	}
}

static void fb_render_cell(unsigned long offset)
{
	unsigned long row;
	unsigned long col;
	unsigned int fg;
	unsigned int bg;
	unsigned long gy;
	unsigned long gx;
	unsigned char bits;
	unsigned char ch;
	unsigned char attr;

	if (offset >= fb_cols * fb_rows) return;
	row = offset / fb_cols;
	col = offset % fb_cols;
	ch = (unsigned char)fb_chars[offset];
	attr = fb_attrs[offset];
	fg = vga_palette_rgb[attr & 0x0F];
	bg = vga_palette_rgb[(attr >> 4) & 0x0F];

	for (gy = 0; gy < fb_cell_h; gy++)
	{
		bits = fb_builtin_font_bits((char)ch, gy);
		for (gx = 0; gx < FB_FONT_W; gx++)
		{
			unsigned int color = (bits & (0x80 >> gx)) ? fg : bg;
			fb_plot((unsigned int)(col * FB_FONT_W + gx), (unsigned int)(row * fb_cell_h + gy), color);
		}
	}
}

static void fb_redraw_all(void)
{
	unsigned long i;
	for (i = 0; i < fb_cols * fb_rows; i++) fb_render_cell(i);
	if (fb_cursor_offset < fb_cols * fb_rows) fb_draw_cursor_overlay(fb_cursor_offset);
}

static void fb_clear_shadow(void)
{
	unsigned long i;
	unsigned long total = fb_cols * fb_rows;
	for (i = 0; i < total; i++)
	{
		fb_chars[i] = ' ';
		fb_attrs[i] = screen_color;
	}
	fb_cursor_offset = 0xFFFFFFFFU;
	if (fb_cols > 0 && fb_rows > 0) fb_fill_rect(0, 0, fb_w, fb_h, vga_palette_rgb[(screen_color >> 4) & 0x0F]);
}

static void fb_scroll(void)
{
	unsigned long row;
	unsigned long col;
	if (screen_row < fb_rows) return;
	for (row = 1; row < fb_rows; row++)
	{
		for (col = 0; col < fb_cols; col++)
		{
			unsigned long dst = (row - 1) * fb_cols + col;
			unsigned long src = row * fb_cols + col;
			fb_chars[dst] = fb_chars[src];
			fb_attrs[dst] = fb_attrs[src];
		}
	}
	for (col = 0; col < fb_cols; col++)
	{
		unsigned long last = (fb_rows - 1) * fb_cols + col;
		fb_chars[last] = ' ';
		fb_attrs[last] = screen_color;
	}
	screen_row = fb_rows - 1;
	fb_redraw_all();
}

static void screen_scroll(void)
{
	unsigned long y;
	unsigned long x;
	if (backend == SCREEN_BACKEND_FB)
	{
		fb_scroll();
		return;
	}
	if (screen_row < vga_height) return;
	for (y = 1; y < vga_height; y++)
	{
		for (x = 0; x < vga_width; x++) VGA[(y - 1) * vga_width + x] = VGA[y * vga_width + x];
	}
	for (x = 0; x < vga_width; x++) VGA[(vga_height - 1) * vga_width + x] = ((unsigned short)screen_color << 8) | ' ';
	screen_row = vga_height - 1;
}

void screen_clear(void)
{
	unsigned long y;
	unsigned long x;
	if (backend == SCREEN_BACKEND_FB)
	{
		fb_clear_shadow();
		screen_row = 0;
		screen_col = 0;
		fb_cursor_offset = 0;
		fb_render_cell(0);
		fb_draw_cursor_overlay(0);
		return;
	}
	for (y = 0; y < vga_height; y++)
	{
		for (x = 0; x < vga_width; x++) VGA[y * vga_width + x] = ((unsigned short)screen_color << 8) | ' ';
	}
	screen_row = 0;
	screen_col = 0;
}

void screen_putchar(char c)
{
	if (c == '\n')
	{
		screen_col = 0;
		screen_row++;
		screen_scroll();
		return;
	}
	screen_write_char_at((unsigned short)(screen_row * active_width() + screen_col), c);
	screen_col++;
	if (screen_col >= active_width())
	{
		screen_col = 0;
		screen_row++;
		screen_scroll();
	}
}

void screen_write(const char *str)
{
	while (*str != '\0') screen_putchar(*str++);
}

void screen_backspace(void)
{
	if (screen_col == 0)
	{
		if (screen_row == 0) return;
		screen_row--;
		screen_col = active_width() - 1;
	}
	else
	{
		screen_col--;
	}
	screen_write_char_at((unsigned short)(screen_row * active_width() + screen_col), ' ');
}

void screen_set_color(unsigned char color)
{
	screen_color = color;
}

unsigned short screen_get_pos(void)
{
	return (unsigned short)(screen_row * active_width() + screen_col);
}

void screen_set_pos(unsigned short offset)
{
	unsigned long w = active_width();
	unsigned long h = active_height();
	screen_row = offset / w;
	screen_col = offset % w;
	if (screen_row >= h)
	{
		screen_row = h - 1;
		screen_col = 0;
	}
	if (backend == SCREEN_BACKEND_FB) screen_set_hw_cursor((unsigned short)(screen_row * w + screen_col));
}

void screen_set_hw_cursor(unsigned short offset)
{
	if (backend == SCREEN_BACKEND_FB)
	{
		unsigned long total = fb_cols * fb_rows;
		if (fb_cursor_offset < total) fb_render_cell(fb_cursor_offset);
		fb_cursor_offset = offset;
		if (fb_cursor_offset < total)
		{
			fb_render_cell(fb_cursor_offset);
			fb_draw_cursor_overlay(fb_cursor_offset);
		}
		return;
	}
	arch_outb(0x3D4, 0x0F);
	arch_outb(0x3D5, (unsigned char)(offset & 0xFF));
	arch_outb(0x3D4, 0x0E);
	arch_outb(0x3D5, (unsigned char)((offset >> 8) & 0xFF));
}

void screen_write_char_at(unsigned short offset, char c)
{
	if (backend == SCREEN_BACKEND_FB)
	{
		unsigned long total = fb_cols * fb_rows;
		if (offset >= total) return;
		fb_chars[offset] = c;
		fb_attrs[offset] = screen_color;
		fb_render_cell(offset);
		if (offset == fb_cursor_offset) fb_draw_cursor_overlay(offset);
		return;
	}
	VGA[offset] = ((unsigned short)screen_color << 8) | (unsigned char)c;
}

unsigned long screen_get_width(void)
{
	return active_width();
}

unsigned long screen_get_height(void)
{
	return active_height();
}

void screen_set_text_mode_80x25(void)
{
	backend = SCREEN_BACKEND_VGA;
	if (vga_font_saved) vga_restore_font_16();
	vga_width = 80;
	vga_height = 25;
	vga_set_scanline_height(0x0F);
	vga_set_cursor_shape(0x0E, 0x0F);
	screen_clear();
	screen_set_hw_cursor(0);
}

void screen_set_text_mode_80x50(void)
{
	backend = SCREEN_BACKEND_VGA;
	if (!vga_font_saved)
	{
		vga_capture_font_16();
		vga_build_font_8_from_16();
	}
	vga_load_font_8();
	vga_width = 80;
	vga_height = 50;
	vga_set_scanline_height(0x07);
	vga_set_cursor_shape(0x06, 0x07);
	screen_clear();
	screen_set_hw_cursor(0);
}

int screen_set_framebuffer_text_mode(void)
{
	if (!framebuffer_available()) return 0;
	if (framebuffer_type() != 1) return 0;
	if (!(framebuffer_bpp() == 32 || framebuffer_bpp() == 24)) return 0;
	
	fb_base = (volatile unsigned char *)(unsigned long)framebuffer_addr();
	fb_pitch = framebuffer_pitch();
	fb_w = framebuffer_width();
	fb_h = framebuffer_height();
	fb_bpp = framebuffer_bpp();
	fb_cols = fb_w / FB_FONT_W;
	fb_rows = fb_h / fb_cell_h;
	if (fb_cols == 0 || fb_rows == 0) return 0;
	if (fb_cols > FB_MAX_COLS) fb_cols = FB_MAX_COLS;
	if (fb_rows > FB_MAX_ROWS) fb_rows = FB_MAX_ROWS;

	backend = SCREEN_BACKEND_FB;
	vga_hide_cursor();
	screen_clear();
	screen_set_hw_cursor(0);
	return 1;
}

int screen_fbfont_get_style(void)
{
	return fb_font_style;
}

int screen_fbfont_set_style(int style)
{
	if (style != FB_FONT_STYLE_CLASSIC && style != FB_FONT_STYLE_BLOCKY) return 0;
	fb_font_style = style;
	if (backend == SCREEN_BACKEND_FB) fb_redraw_all();
	return 1;
}

unsigned int screen_fbfont_get_size(void)
{
	return fb_cell_h;
}

int screen_fbfont_set_size(unsigned int size)
{
	if (!(size == 12 || size == 14 || size == 16)) return 0;
	fb_cell_h = size;
	if (backend == SCREEN_BACKEND_FB)
	{
		fb_rows = fb_h / fb_cell_h;
		if (fb_rows == 0) fb_rows = 1;
		if (fb_rows > FB_MAX_ROWS) fb_rows = FB_MAX_ROWS;
		if (screen_row >= fb_rows) screen_row = fb_rows - 1;
		if (screen_col >= fb_cols) screen_col = fb_cols - 1;
		fb_redraw_all();
	}
	return 1;
}

void screen_fbfont_reset_custom(void)
{
	unsigned long i;
	for (i = 0; i < FB_GLYPH_COUNT; i++) fb_custom_valid[i] = 0;
	if (backend == SCREEN_BACKEND_FB) fb_redraw_all();
}

int screen_fbfont_set_custom_glyph(char ch, const unsigned char rows[7])
{
	int idx;
	unsigned long i;
	if (rows == (void *)0) return 0;
	idx = fb_char_index(ch);
	if (idx < 0) return 0;
	for (i = 0; i < 7; i++) fb_custom_rows[idx][i] = (unsigned char)(rows[i] & 0x1F);
	fb_custom_valid[idx] = 1;
	if (backend == SCREEN_BACKEND_FB) fb_redraw_all();
	return 1;
}

int screen_fbfont_get_custom_glyph(char ch, unsigned char rows[7], int *is_custom)
{
	int idx = fb_char_index(ch);
	unsigned long i;
	if (idx < 0 || rows == (void *)0) return 0;
	for (i = 0; i < 7; i++) rows[i] = fb_custom_rows[idx][i];
	if (is_custom != (void *)0) *is_custom = fb_custom_valid[idx] ? 1 : 0;
	return 1;
}
