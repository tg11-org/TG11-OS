#include "screen.h"
#include "arch.h"

static volatile unsigned short *const VGA = (unsigned short *)0xB8000;
static unsigned long vga_width = 80;
static unsigned long vga_height = 25;

static unsigned long screen_row = 0;
static unsigned long screen_col = 0;
static unsigned char screen_color = 0x0F;

static void vga_set_scanline_height(unsigned char max_scanline)
{
	/* CRTC max scanline register controls character cell height-1 */
	arch_outb(0x3D4, 0x09);
	arch_outb(0x3D5, max_scanline);
}

static void screen_scroll(void)
{
	unsigned long y;
	unsigned long x;

	if (screen_row < vga_height)
	{
		return;
	}

	for (y = 1; y < vga_height; y++)
	{
		for (x = 0; x < vga_width; x++)
		{
			VGA[(y - 1) * vga_width + x] = VGA[y * vga_width + x];
		}
	}

	for (x = 0; x < vga_width; x++)
	{
		VGA[(vga_height - 1) * vga_width + x] =
			((unsigned short)screen_color << 8) | ' ';
	}

	screen_row = vga_height - 1;
}

void screen_clear(void)
{
	unsigned long y;
	unsigned long x;

	for (y = 0; y < vga_height; y++)
	{
		for (x = 0; x < vga_width; x++)
		{
			VGA[y * vga_width + x] = ((unsigned short)screen_color << 8) | ' ';
		}
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

	VGA[screen_row * vga_width + screen_col] =
		((unsigned short)screen_color << 8) | (unsigned char)c;

	screen_col++;
	if (screen_col >= vga_width)
	{
		screen_col = 0;
		screen_row++;
		screen_scroll();
	}
}

void screen_write(const char *str)
{
	while (*str != '\0')
	{
		screen_putchar(*str++);
	}
}

void screen_backspace(void)
{
	if (screen_col == 0)
	{
		if (screen_row == 0)
		{
			return;
		}

		screen_row--;
		screen_col = vga_width - 1;
	}
	else
	{
		screen_col--;
	}

	VGA[screen_row * vga_width + screen_col] =
		((unsigned short)screen_color << 8) | ' ';
}

void screen_set_color(unsigned char color)
{
	screen_color = color;
}

unsigned short screen_get_pos(void)
{
	return (unsigned short)(screen_row * vga_width + screen_col);
}

void screen_set_pos(unsigned short offset)
{
	screen_row = offset / vga_width;
	screen_col = offset % vga_width;
	if (screen_row >= vga_height)
	{
		screen_row = vga_height - 1;
		screen_col = 0;
	}
}

void screen_set_hw_cursor(unsigned short offset)
{
	arch_outb(0x3D4, 0x0F);
	arch_outb(0x3D5, (unsigned char)(offset & 0xFF));
	arch_outb(0x3D4, 0x0E);
	arch_outb(0x3D5, (unsigned char)((offset >> 8) & 0xFF));
}

void screen_write_char_at(unsigned short offset, char c)
{
	VGA[offset] = ((unsigned short)screen_color << 8) | (unsigned char)c;
}

unsigned long screen_get_width(void)
{
	return vga_width;
}

unsigned long screen_get_height(void)
{
	return vga_height;
}

void screen_set_text_mode_80x25(void)
{
	vga_width = 80;
	vga_height = 25;
	vga_set_scanline_height(0x0F);
	screen_clear();
	screen_set_hw_cursor(0);
}

void screen_set_text_mode_80x50(void)
{
	vga_width = 80;
	vga_height = 50;
	vga_set_scanline_height(0x07);
	screen_clear();
	screen_set_hw_cursor(0);
}
