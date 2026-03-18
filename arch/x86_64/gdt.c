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

#include "arch.h"

/* GDT descriptor entry (8 bytes) */
struct gdt_descriptor {
	unsigned short limit_low;
	unsigned short base_low;
	unsigned char base_mid;
	unsigned char access;
	unsigned char granularity;
	unsigned char base_high;
} __attribute__((packed));

/* GDT pointer (10 bytes) for LGDT instruction */
struct gdt_ptr {
	unsigned short limit;
	unsigned long base;
} __attribute__((packed));

typedef char gdt_descriptor_size_must_be_8[(sizeof(struct gdt_descriptor) == 8) ? 1 : -1];
typedef char gdt_ptr_size_must_be_10[(sizeof(struct gdt_ptr) == 10) ? 1 : -1];

#define GDT_ENTRIES 3
static struct gdt_descriptor gdt[GDT_ENTRIES];
static struct gdt_ptr gdt_ptr_val;

/* GDT descriptor access byte flags */
#define GDT_ACCESS_PRESENT       0x80
#define GDT_ACCESS_RING0         0x00
#define GDT_ACCESS_CODE_DATA     0x10
#define GDT_ACCESS_CODE          0x08
#define GDT_ACCESS_READ_WRITE    0x02

/* GDT descriptor granularity flags */
#define GDT_GRAN_4K              0x80
#define GDT_GRAN_32BIT           0x40
#define GDT_GRAN_64BIT           0x20

static void gdt_set_descriptor(unsigned int index, unsigned long base, unsigned long limit,
								unsigned char access, unsigned char granularity)
{
	if (index >= GDT_ENTRIES) return;

	gdt[index].limit_low = (unsigned short)(limit & 0xFFFF);
	gdt[index].base_low = (unsigned short)(base & 0xFFFF);
	gdt[index].base_mid = (unsigned char)((base >> 16) & 0xFF);
	gdt[index].access = access;
	gdt[index].granularity = (unsigned char)(((limit >> 16) & 0x0F) | granularity);
	gdt[index].base_high = (unsigned char)((base >> 24) & 0xFF);
}

void gdt_init(void)
{
	/* Null descriptor */
	gdt_set_descriptor(0, 0, 0, 0, 0);

	/* Kernel code segment (64-bit) */
	gdt_set_descriptor(1, 0, 0xFFFFF,
	                   GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | GDT_ACCESS_CODE_DATA | GDT_ACCESS_CODE | GDT_ACCESS_READ_WRITE,
	                   GDT_GRAN_4K | GDT_GRAN_64BIT);

	/* Kernel data segment */
	gdt_set_descriptor(2, 0, 0xFFFFF,
	                   GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | GDT_ACCESS_CODE_DATA | GDT_ACCESS_READ_WRITE,
	                   GDT_GRAN_4K | GDT_GRAN_64BIT);

	/* Set up GDT pointer */
	gdt_ptr_val.limit = (GDT_ENTRIES * sizeof(struct gdt_descriptor)) - 1;
	gdt_ptr_val.base = (unsigned long)&gdt[0];

	/* Load GDT */
	arch_lgdt(&gdt_ptr_val);
}

