#include "serial.h"

#define COM1_PORT 0x3F8

static void outb(unsigned short port, unsigned char value)
{
	__asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static unsigned char inb(unsigned short port)
{
	unsigned char value;
	__asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static int serial_is_transmit_empty(void)
{
	return (inb(COM1_PORT + 5) & 0x20) != 0;
}

int serial_can_read(void)
{
	return (inb(COM1_PORT + 5) & 0x01) != 0;
}

int serial_try_read(char *out_char)
{
	if (out_char == (void *)0) return 0;
	if (!serial_can_read()) return 0;
	*out_char = (char)inb(COM1_PORT + 0);
	return 1;
}

int serial_init(void)
{
	outb(COM1_PORT + 1, 0x00);
	outb(COM1_PORT + 3, 0x80);
	outb(COM1_PORT + 0, 0x03);
	outb(COM1_PORT + 1, 0x00);
	outb(COM1_PORT + 3, 0x03);
	outb(COM1_PORT + 2, 0xC7);
	outb(COM1_PORT + 4, 0x0B);
	outb(COM1_PORT + 4, 0x1E);
	outb(COM1_PORT + 0, 0xAE);

	if (inb(COM1_PORT + 0) != 0xAE)
	{
		outb(COM1_PORT + 4, 0x0F);
		return 0;
	}

	outb(COM1_PORT + 4, 0x0F);
	return 1;
}

void serial_putchar(char c)
{
	while (!serial_is_transmit_empty())
	{
	}

	outb(COM1_PORT, (unsigned char)c);
}

void serial_write(const char *str)
{
	while (*str != '\0')
	{
		if (*str == '\n')
		{
			serial_putchar('\r');
		}

		serial_putchar(*str++);
	}
}
// Copyright (C) 2026 TG11
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
// 
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

