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

/* ── GDT segment descriptor (8 bytes) ─────────────────────────────── */
struct gdt_descriptor {
	unsigned short limit_low;
	unsigned short base_low;
	unsigned char  base_mid;
	unsigned char  access;
	unsigned char  granularity;
	unsigned char  base_high;
} __attribute__((packed));

/* GDT pointer (10 bytes) for LGDT instruction */
struct gdt_ptr {
	unsigned short limit;
	unsigned long  base;
} __attribute__((packed));

typedef char gdt_descriptor_size_must_be_8[(sizeof(struct gdt_descriptor) == 8) ? 1 : -1];
typedef char gdt_ptr_size_must_be_10[(sizeof(struct gdt_ptr) == 10) ? 1 : -1];

/*
 * GDT layout (selectors):
 *  [0]  0x00  null
 *  [1]  0x08  kernel code   (ring 0, 64-bit)
 *  [2]  0x10  kernel data   (ring 0)
 *  [3]  0x18  user data pad (needed as STAR arithmetic base; also valid data seg)
 *  [4]  0x20  user data     (ring 3)  → selector used with RPL=3 → 0x23
 *  [5]  0x28  user code     (ring 3)  → selector used with RPL=3 → 0x2B
 *  [6]  0x30  TSS low  (64-bit system descriptor, spans [6]+[7])
 *  [7]  0x38  TSS high
 *
 * STAR[47:32] = 0x08 → SYSCALL sets CS=0x08, SS=0x10
 * STAR[63:48] = 0x18 → SYSRETQ sets CS = 0x18+16|3 = 0x2B, SS = 0x18+8|3 = 0x23
 */
#define GDT_ENTRIES 8
static struct gdt_descriptor gdt[GDT_ENTRIES];
static struct gdt_ptr gdt_ptr_val;

/* ── Access byte flags ─────────────────────────────────────────────── */
#define GDT_ACCESS_PRESENT    0x80
#define GDT_ACCESS_RING0      0x00
#define GDT_ACCESS_RING3      0x60   /* DPL = 3 */
#define GDT_ACCESS_CODE_DATA  0x10
#define GDT_ACCESS_CODE       0x08
#define GDT_ACCESS_READ_WRITE 0x02

/* ── Granularity byte flags ────────────────────────────────────────── */
#define GDT_GRAN_4K   0x80
#define GDT_GRAN_64BIT 0x20

/* ── 64-bit TSS (x86-64 volume 3, figure 7-11) ─────────────────────── */
struct tss64 {
	unsigned int   reserved0;
	unsigned long  rsp0;          /* kernel RSP when interrupting ring 3 */
	unsigned long  rsp1;
	unsigned long  rsp2;
	unsigned long  reserved1;
	unsigned long  ist[7];        /* interrupt stack table entries 1-7 */
	unsigned long  reserved2;
	unsigned short reserved3;
	unsigned short iopb_offset;   /* I/O permission bitmap offset */
} __attribute__((packed));

static struct tss64 tss;

/* ── Helpers ────────────────────────────────────────────────────────── */

static void gdt_set_descriptor(unsigned int index, unsigned long base, unsigned long limit,
                                unsigned char access, unsigned char granularity)
{
	if (index >= GDT_ENTRIES) return;
	gdt[index].limit_low  = (unsigned short)(limit & 0xFFFF);
	gdt[index].base_low   = (unsigned short)(base  & 0xFFFF);
	gdt[index].base_mid   = (unsigned char)((base  >> 16) & 0xFF);
	gdt[index].access     = access;
	gdt[index].granularity = (unsigned char)(((limit >> 16) & 0x0F) | granularity);
	gdt[index].base_high  = (unsigned char)((base  >> 24) & 0xFF);
}

/*
 * Write a 16-byte 64-bit system descriptor (TSS) across gdt[6]+gdt[7].
 * The CPU treats the following 8 bytes as the upper half of the descriptor.
 *
 * Format of the 16-byte descriptor:
 *  Bytes  0- 1 : limit[15:0]
 *  Bytes  2- 3 : base[15:0]
 *  Byte   4    : base[23:16]
 *  Byte   5    : access  (0x89 = Present | Type=9 = 64-bit TSS Available)
 *  Byte   6    : gran[7:4]=0, limit[19:16]
 *  Byte   7    : base[31:24]
 *  Bytes  8-11 : base[63:32]
 *  Bytes 12-15 : reserved (zero)
 */
static void gdt_set_tss(void)
{
	unsigned long  base  = (unsigned long)&tss;
	unsigned long  limit = sizeof(struct tss64) - 1;
	unsigned char *d     = (unsigned char *)&gdt[6]; /* 16-byte window */

	/* Low 8 bytes */
	d[0] = (unsigned char)(limit & 0xFF);
	d[1] = (unsigned char)((limit >> 8) & 0xFF);
	d[2] = (unsigned char)(base & 0xFF);
	d[3] = (unsigned char)((base >> 8) & 0xFF);
	d[4] = (unsigned char)((base >> 16) & 0xFF);
	d[5] = 0x89; /* Present | Type=9 (64-bit TSS Available) */
	d[6] = (unsigned char)(((limit >> 16) & 0x0F)); /* no gran flags for TSS */
	d[7] = (unsigned char)((base >> 24) & 0xFF);

	/* High 8 bytes */
	d[8]  = (unsigned char)((base >> 32) & 0xFF);
	d[9]  = (unsigned char)((base >> 40) & 0xFF);
	d[10] = (unsigned char)((base >> 48) & 0xFF);
	d[11] = (unsigned char)((base >> 56) & 0xFF);
	d[12] = 0;
	d[13] = 0;
	d[14] = 0;
	d[15] = 0;

	/* I/O permission bitmap points past the TSS (no I/O access from ring 3) */
	tss.iopb_offset = (unsigned short)sizeof(struct tss64);
}

void gdt_tss_set_rsp0(unsigned long rsp0)
{
	tss.rsp0 = rsp0;
}

/* ── gdt_init ───────────────────────────────────────────────────────── */

void gdt_init(void)
{
	/* [0] Null */
	gdt_set_descriptor(0, 0, 0, 0, 0);

	/* [1] 0x08 Kernel code (64-bit, ring 0) */
	gdt_set_descriptor(1, 0, 0xFFFFF,
	                   GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | GDT_ACCESS_CODE_DATA |
	                   GDT_ACCESS_CODE | GDT_ACCESS_READ_WRITE,
	                   GDT_GRAN_4K | GDT_GRAN_64BIT);

	/* [2] 0x10 Kernel data (ring 0) */
	gdt_set_descriptor(2, 0, 0xFFFFF,
	                   GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | GDT_ACCESS_CODE_DATA |
	                   GDT_ACCESS_READ_WRITE,
	                   GDT_GRAN_4K | GDT_GRAN_64BIT);

	/* [3] 0x18 User data pad — STAR arithmetic base; DPL=3 so CPU accepts it */
	gdt_set_descriptor(3, 0, 0xFFFFF,
	                   GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | GDT_ACCESS_CODE_DATA |
	                   GDT_ACCESS_READ_WRITE,
	                   GDT_GRAN_4K | GDT_GRAN_64BIT);

	/* [4] 0x20 User data (ring 3)  — selector in use: 0x23 (0x20 | RPL=3) */
	gdt_set_descriptor(4, 0, 0xFFFFF,
	                   GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | GDT_ACCESS_CODE_DATA |
	                   GDT_ACCESS_READ_WRITE,
	                   GDT_GRAN_4K | GDT_GRAN_64BIT);

	/* [5] 0x28 User code (ring 3, 64-bit) — selector in use: 0x2B (0x28 | RPL=3) */
	gdt_set_descriptor(5, 0, 0xFFFFF,
	                   GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | GDT_ACCESS_CODE_DATA |
	                   GDT_ACCESS_CODE | GDT_ACCESS_READ_WRITE,
	                   GDT_GRAN_4K | GDT_GRAN_64BIT);

	/* [6]+[7] TSS system descriptor (16 bytes total) */
	gdt_set_tss();

	/* Load the new GDT */
	gdt_ptr_val.limit = (GDT_ENTRIES * sizeof(struct gdt_descriptor)) - 1;
	gdt_ptr_val.base  = (unsigned long)&gdt[0];
	arch_lgdt(&gdt_ptr_val);

	/* Load the task register with the TSS selector (0x30) */
	arch_ltr(0x30);
}

