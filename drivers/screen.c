#include "screen.h"
#include "arch.h"

static volatile unsigned short *const VGA = (unsigned short *)0xB8000;
static const unsigned long VGA_WIDTH = 80;
static const unsigned long VGA_HEIGHT = 25;

static unsigned long screen_row = 0;
static unsigned long screen_col = 0;
static unsigned char screen_color = 0x0F;

static void screen_scroll(void)
{
	unsigned long y;
	unsigned long x;

	if (screen_row < VGA_HEIGHT)
	{
		return;
	}

	for (y = 1; y < VGA_HEIGHT; y++)
	{
		for (x = 0; x < VGA_WIDTH; x++)
		{
			VGA[(y - 1) * VGA_WIDTH + x] = VGA[y * VGA_WIDTH + x];
		}
	}

	for (x = 0; x < VGA_WIDTH; x++)
	{
		VGA[(VGA_HEIGHT - 1) * VGA_WIDTH + x] =
			((unsigned short)screen_color << 8) | ' ';
	}

	screen_row = VGA_HEIGHT - 1;
}

void screen_clear(void)
{
	unsigned long y;
	unsigned long x;

	for (y = 0; y < VGA_HEIGHT; y++)
	{
		for (x = 0; x < VGA_WIDTH; x++)
		{
			VGA[y * VGA_WIDTH + x] = ((unsigned short)screen_color << 8) | ' ';
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

	VGA[screen_row * VGA_WIDTH + screen_col] =
		((unsigned short)screen_color << 8) | (unsigned char)c;

	screen_col++;
	if (screen_col >= VGA_WIDTH)
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
		screen_col = VGA_WIDTH - 1;
	}
	else
	{
		screen_col--;
	}

	VGA[screen_row * VGA_WIDTH + screen_col] =
		((unsigned short)screen_color << 8) | ' ';
}

void screen_set_color(unsigned char color)
{
	screen_color = color;
}

unsigned short screen_get_pos(void)
{
	return (unsigned short)(screen_row * VGA_WIDTH + screen_col);
}

void screen_set_pos(unsigned short offset)
{
	screen_row = offset / VGA_WIDTH;
	screen_col = offset % VGA_WIDTH;
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
