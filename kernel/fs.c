#include "fs.h"

#define FS_MAX_NODES      256

struct fs_node
{
	int used;
	int is_dir;
	int parent;
	int first_child;
	int next_sibling;
	char name[FS_NAME_MAX + 1];
	unsigned long size;
	char data[FS_MAX_FILE_SIZE];
};

static struct fs_node nodes[FS_MAX_NODES];
static int cwd_node = 0;

static void wr16_le(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)(v & 0xFFu);
	p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void wr32_le(unsigned char *p, unsigned long v)
{
	p[0] = (unsigned char)(v & 0xFFu);
	p[1] = (unsigned char)((v >> 8) & 0xFFu);
	p[2] = (unsigned char)((v >> 16) & 0xFFu);
	p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static void wr64_le(unsigned char *p, unsigned long v)
{
	unsigned long i;
	for (i = 0; i < 8; i++) p[i] = (unsigned char)((v >> (i * 8)) & 0xFFu);
}

static unsigned long align_up_value(unsigned long v, unsigned long align)
{
	if (align == 0) return v;
	return (v + align - 1) & ~(align - 1);
}

static int seed_builtin_elf_type(unsigned short elf_type, const char *path, unsigned long base_vaddr, unsigned int p_flags,
	const unsigned char *code, unsigned long code_size, unsigned long seg_filesz, unsigned long seg_memsz)
{
	unsigned char buf[FS_MAX_FILE_SIZE];
	const unsigned long payload_off = 0x100UL;
	unsigned long image_size;
	unsigned long i;

	if (path == (void *)0 || code == (void *)0 || code_size == 0) return -1;
	if (seg_filesz < code_size) seg_filesz = code_size;
	if (seg_memsz < seg_filesz) seg_memsz = seg_filesz;
	if (seg_filesz == 0 || seg_memsz == 0) return -1;
	if (seg_filesz > (FS_MAX_FILE_SIZE - payload_off)) return -1;

	image_size = payload_off + seg_filesz;
	for (i = 0; i < image_size; i++) buf[i] = 0;

	/* ELF64 header */
	buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
	buf[4] = 2;    /* EI_CLASS: ELF64 */
	buf[5] = 1;    /* EI_DATA: little-endian */
	buf[6] = 1;    /* EI_VERSION */
	wr16_le(&buf[16], elf_type);
	wr16_le(&buf[18], 62);     /* EM_X86_64 */
	wr32_le(&buf[20], 1);      /* EV_CURRENT */
	wr64_le(&buf[24], base_vaddr); /* e_entry */
	wr64_le(&buf[32], 0x40);   /* e_phoff */
	wr16_le(&buf[52], 0x40);   /* e_ehsize */
	wr16_le(&buf[54], 0x38);   /* e_phentsize */
	wr16_le(&buf[56], 1);      /* e_phnum */

	/* One PT_LOAD program header at offset 0x40 */
	wr32_le(&buf[0x40], 1);            /* PT_LOAD */
	wr32_le(&buf[0x44], p_flags);      /* PF_R/PF_W/PF_X */
	wr64_le(&buf[0x48], payload_off);  /* p_offset */
	wr64_le(&buf[0x50], base_vaddr);   /* p_vaddr */
	wr64_le(&buf[0x58], 0);            /* p_paddr (unused) */
	wr64_le(&buf[0x60], seg_filesz);   /* p_filesz */
	wr64_le(&buf[0x68], seg_memsz);    /* p_memsz */
	wr64_le(&buf[0x70], 0x100);        /* p_align */

	for (i = 0; i < code_size; i++) buf[payload_off + i] = code[i];

	return fs_write_file(path, buf, image_size);
}

static int seed_builtin_elf(const char *path, unsigned long base_vaddr, unsigned int p_flags,
	const unsigned char *code, unsigned long code_size, unsigned long seg_filesz, unsigned long seg_memsz)
{
	return seed_builtin_elf_type(2u, path, base_vaddr, p_flags, code, code_size, seg_filesz, seg_memsz);
}

static int seed_builtin_elf_with_symbols_type(unsigned short elf_type, const char *path, unsigned long base_vaddr, unsigned int p_flags,
	const unsigned char *code, unsigned long code_size, unsigned long seg_filesz, unsigned long seg_memsz,
	const char *symbol_name)
{
	unsigned char buf[FS_MAX_FILE_SIZE];
	static const char shstrtab[] = "\0.shstrtab\0.text\0.strtab\0.symtab\0";
	const unsigned long payload_off = 0x100UL;
	unsigned long shstrtab_off;
	unsigned long shstrtab_size = sizeof(shstrtab);
	unsigned long strtab_off;
	unsigned long strtab_size;
	unsigned long symtab_off;
	unsigned long symtab_size = 3UL * 24UL;
	unsigned long shoff;
	unsigned long image_size;
	unsigned long sym_name_off = 1;
	unsigned long i;

	if (path == (void *)0 || code == (void *)0 || symbol_name == (void *)0 || symbol_name[0] == '\0') return -1;
	if (seg_filesz < code_size) seg_filesz = code_size;
	if (seg_memsz < seg_filesz) seg_memsz = seg_filesz;
	if (seg_filesz == 0 || seg_memsz == 0) return -1;

	strtab_size = 2;
	while (symbol_name[strtab_size - 1] != '\0') strtab_size++;
	/* Include both leading and trailing NUL in .strtab size. */
	strtab_size++;

	shstrtab_off = align_up_value(payload_off + seg_filesz, 8UL);
	strtab_off = align_up_value(shstrtab_off + shstrtab_size, 8UL);
	symtab_off = align_up_value(strtab_off + strtab_size, 8UL);
	shoff = align_up_value(symtab_off + symtab_size, 8UL);
	image_size = shoff + (5UL * 64UL);

	if (image_size > FS_MAX_FILE_SIZE) return -1;

	for (i = 0; i < image_size; i++) buf[i] = 0;

	buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
	buf[4] = 2;
	buf[5] = 1;
	buf[6] = 1;
	wr16_le(&buf[16], elf_type);
	wr16_le(&buf[18], 62);
	wr32_le(&buf[20], 1);
	wr64_le(&buf[24], base_vaddr);
	wr64_le(&buf[32], 0x40);
	wr64_le(&buf[40], shoff);
	wr16_le(&buf[52], 0x40);
	wr16_le(&buf[54], 0x38);
	wr16_le(&buf[56], 1);
	wr16_le(&buf[58], 0x40);
	wr16_le(&buf[60], 5);
	wr16_le(&buf[62], 1);

	wr32_le(&buf[0x40], 1);
	wr32_le(&buf[0x44], p_flags);
	wr64_le(&buf[0x48], payload_off);
	wr64_le(&buf[0x50], base_vaddr);
	wr64_le(&buf[0x58], 0);
	wr64_le(&buf[0x60], seg_filesz);
	wr64_le(&buf[0x68], seg_memsz);
	wr64_le(&buf[0x70], 0x100);

	for (i = 0; i < code_size; i++) buf[payload_off + i] = code[i];
	for (i = 0; i < shstrtab_size; i++) buf[shstrtab_off + i] = (unsigned char)shstrtab[i];
	buf[strtab_off] = '\0';
	for (i = 0; symbol_name[i] != '\0'; i++) buf[strtab_off + sym_name_off + i] = (unsigned char)symbol_name[i];
	buf[strtab_off + sym_name_off + i] = '\0';

	buf[symtab_off + 24UL + 4] = 0x03;
	wr16_le(&buf[symtab_off + 24UL + 6], 2);
	wr64_le(&buf[symtab_off + 24UL + 8], base_vaddr);
	wr64_le(&buf[symtab_off + 24UL + 16], code_size);

	wr32_le(&buf[symtab_off + 48UL], sym_name_off);
	buf[symtab_off + 48UL + 4] = 0x12;
	wr16_le(&buf[symtab_off + 48UL + 6], 2);
	wr64_le(&buf[symtab_off + 48UL + 8], base_vaddr);
	wr64_le(&buf[symtab_off + 48UL + 16], code_size);

	wr32_le(&buf[shoff + 64UL + 0], 1);
	wr32_le(&buf[shoff + 64UL + 4], 3);
	wr64_le(&buf[shoff + 64UL + 24], shstrtab_off);
	wr64_le(&buf[shoff + 64UL + 32], shstrtab_size);
	wr64_le(&buf[shoff + 64UL + 48], 1);

	wr32_le(&buf[shoff + 128UL + 0], 11);
	wr32_le(&buf[shoff + 128UL + 4], 1);
	wr64_le(&buf[shoff + 128UL + 8], (p_flags & 0x1u) ? 0x6UL : 0x2UL);
	wr64_le(&buf[shoff + 128UL + 16], base_vaddr);
	wr64_le(&buf[shoff + 128UL + 24], payload_off);
	wr64_le(&buf[shoff + 128UL + 32], code_size);
	wr64_le(&buf[shoff + 128UL + 48], 16);

	wr32_le(&buf[shoff + 192UL + 0], 17);
	wr32_le(&buf[shoff + 192UL + 4], 3);
	wr64_le(&buf[shoff + 192UL + 24], strtab_off);
	wr64_le(&buf[shoff + 192UL + 32], strtab_size);
	wr64_le(&buf[shoff + 192UL + 48], 1);

	wr32_le(&buf[shoff + 256UL + 0], 25);
	wr32_le(&buf[shoff + 256UL + 4], 2);
	wr64_le(&buf[shoff + 256UL + 24], symtab_off);
	wr64_le(&buf[shoff + 256UL + 32], symtab_size);
	wr32_le(&buf[shoff + 256UL + 40], 3);
	wr32_le(&buf[shoff + 256UL + 44], 2);
	wr64_le(&buf[shoff + 256UL + 48], 8);
	wr64_le(&buf[shoff + 256UL + 56], 24);

	return fs_write_file(path, buf, image_size);
}

static int seed_builtin_elf_with_symbols(const char *path, unsigned long base_vaddr, unsigned int p_flags,
	const unsigned char *code, unsigned long code_size, unsigned long seg_filesz, unsigned long seg_memsz,
	const char *symbol_name)
{
	return seed_builtin_elf_with_symbols_type(2u, path, base_vaddr, p_flags, code, code_size, seg_filesz, seg_memsz, symbol_name);
}

static unsigned long str_len(const char *s)
{
	unsigned long n = 0;
	while (s[n] != '\0') n++;
	return n;
}

static int str_eq(const char *a, const char *b)
{
	unsigned long i = 0;
	while (a[i] != '\0' && b[i] != '\0')
	{
		if (a[i] != b[i]) return 0;
		i++;
	}
	return a[i] == '\0' && b[i] == '\0';
}

static void str_copy(char *dst, const char *src, unsigned long max_len)
{
	unsigned long i = 0;
	if (max_len == 0) return;
	while (src[i] != '\0' && i < (max_len - 1))
	{
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

static int alloc_node(void)
{
	int i;
	for (i = 1; i < FS_MAX_NODES; i++)
	{
		if (!nodes[i].used)
		{
			nodes[i].used = 1;
			nodes[i].is_dir = 0;
			nodes[i].parent = -1;
			nodes[i].first_child = -1;
			nodes[i].next_sibling = -1;
			nodes[i].name[0] = '\0';
			nodes[i].size = 0;
			nodes[i].data[0] = '\0';
			return i;
		}
	}
	return -1;
}

static int find_child(int parent, const char *name)
{
	int n = nodes[parent].first_child;
	while (n != -1)
	{
		if (str_eq(nodes[n].name, name)) return n;
		n = nodes[n].next_sibling;
	}
	return -1;
}

static int add_child(int parent, const char *name, int is_dir)
{
	int idx;
	unsigned long name_len;

	if (parent < 0 || !nodes[parent].used || !nodes[parent].is_dir) return -1;
	if (name[0] == '\0') return -1;
	name_len = str_len(name);
	if (name_len > FS_NAME_MAX) return -1;
	if (str_eq(name, ".") || str_eq(name, "..")) return -1;
	if (find_child(parent, name) != -1) return -1;

	idx = alloc_node();
	if (idx < 0) return -1;

	nodes[idx].is_dir = is_dir;
	nodes[idx].parent = parent;
	nodes[idx].first_child = -1;
	nodes[idx].next_sibling = nodes[parent].first_child;
	str_copy(nodes[idx].name, name, sizeof(nodes[idx].name));
	nodes[parent].first_child = idx;
	return idx;
}

static int split_parent_and_name(const char *path, int *out_parent, char *out_name)
{
	int cur;
	const char *p = path;
	char part[FS_NAME_MAX + 1];
	unsigned long i;

	if (path == (void *)0 || path[0] == '\0') return -1;
	cur = (path[0] == '/') ? 0 : cwd_node;

	while (*p == '/') p++;
	if (*p == '\0') return -1;

	while (*p != '\0')
	{
		i = 0;
		while (*p != '\0' && *p != '/')
		{
			if (i >= FS_NAME_MAX) return -1;
			part[i++] = *p++;
		}
		part[i] = '\0';

		while (*p == '/') p++;

		if (*p == '\0')
		{
			if (str_eq(part, ".") || str_eq(part, "..")) return -1;
			*out_parent = cur;
			str_copy(out_name, part, FS_NAME_MAX + 1);
			return 0;
		}

		if (str_eq(part, "."))
		{
			continue;
		}
		if (str_eq(part, ".."))
		{
			if (nodes[cur].parent != -1) cur = nodes[cur].parent;
			continue;
		}

		cur = find_child(cur, part);
		if (cur < 0 || !nodes[cur].is_dir) return -1;
	}

	return -1;
}

static int resolve_path(const char *path)
{
	int cur;
	const char *p;
	char part[FS_NAME_MAX + 1];
	unsigned long i;

	if (path == (void *)0 || path[0] == '\0') return cwd_node;
	cur = (path[0] == '/') ? 0 : cwd_node;
	p = path;

	while (*p == '/') p++;
	if (*p == '\0') return 0;

	while (*p != '\0')
	{
		i = 0;
		while (*p != '\0' && *p != '/')
		{
			if (i >= FS_NAME_MAX) return -1;
			part[i++] = *p++;
		}
		part[i] = '\0';
		while (*p == '/') p++;

		if (str_eq(part, ".")) continue;
		if (str_eq(part, ".."))
		{
			if (nodes[cur].parent != -1) cur = nodes[cur].parent;
			continue;
		}

		cur = find_child(cur, part);
		if (cur < 0) return -1;
	}

	return cur;
}

static int unlink_node_from_parent(int idx)
{
	int parent;
	int cur;
	int prev;

	if (idx <= 0 || !nodes[idx].used) return -1;
	parent = nodes[idx].parent;
	if (parent < 0) return -1;

	cur = nodes[parent].first_child;
	prev = -1;
	while (cur != -1)
	{
		if (cur == idx)
		{
			if (prev == -1) nodes[parent].first_child = nodes[cur].next_sibling;
			else nodes[prev].next_sibling = nodes[cur].next_sibling;
			return 0;
		}
		prev = cur;
		cur = nodes[cur].next_sibling;
	}

	return -1;
}

void fs_init(void)
{
	int i;
	for (i = 0; i < FS_MAX_NODES; i++)
	{
		nodes[i].used = 0;
		nodes[i].is_dir = 0;
		nodes[i].parent = -1;
		nodes[i].first_child = -1;
		nodes[i].next_sibling = -1;
		nodes[i].name[0] = '\0';
		nodes[i].size = 0;
		nodes[i].data[0] = '\0';
	}

	nodes[0].used = 1;
	nodes[0].is_dir = 1;
	nodes[0].parent = -1;
	nodes[0].first_child = -1;
	nodes[0].next_sibling = -1;
	nodes[0].name[0] = '/';
	nodes[0].name[1] = '\0';
	cwd_node = 0;

	/* Seed built-in ELF fixtures for exec/execstress/elfselftest. */
	{
		static const unsigned char app_code[] = {
			/* mov eax,42; ret */
			0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3
		};
		static const unsigned char appw_code[] = {
			/* mov eax,7; mov [rip+6],eax; mov eax,[rip+0]; ret; dd 0 */
			0xB8,0x07,0x00,0x00,0x00,
			/* store to data dword after ret: target offset 18 from next RIP(11) => +7 */
			0x89,0x05,0x07,0x00,0x00,0x00,
			/* load from same dword: target offset 18 from next RIP(17) => +1 */
			0x8B,0x05,0x01,0x00,0x00,0x00,
			0xC3,
			0x00,0x00,0x00,0x00
		};
		static const unsigned char app2p_code[] = {
			/* mov eax,99; ret */
			0xB8, 0x63, 0x00, 0x00, 0x00, 0xC3
		};
		static const unsigned char panic_code[] = {
			/* push rbp; mov rbp,rsp; ud2 */
			0x55,
			0x48, 0x89, 0xE5,
			0x0F, 0x0B
		};
		static const unsigned char bss_code[] = {
			/* mov eax,[rip+16]; add eax,13; mov [rip+7],eax; mov eax,[rip+1]; ret */
			0x8B,0x05,0x10,0x00,0x00,0x00,
			0x83,0xC0,0x0D,
			0x89,0x05,0x07,0x00,0x00,0x00,
			0x8B,0x05,0x01,0x00,0x00,0x00,
			0xC3
		};
		static const unsigned char pie_code[] = {
			/* mov eax,123; ret */
			0xB8, 0x7B, 0x00, 0x00, 0x00, 0xC3
		};
		static const unsigned char hello_code[] = {
			/* write("hello\n", 6); exit(0); */
			0xB8,0x01,0x00,0x00,0x00,
			0x48,0x8D,0x3D,0x10,0x00,0x00,0x00,
			0xBE,0x06,0x00,0x00,0x00,
			0x0F,0x05,
			0xB8,0x3C,0x00,0x00,0x00,
			0x31,0xFF,
			0x0F,0x05,
			'h','e','l','l','o','\n'
		};
		static const unsigned char argc_code[] = {
			/* argc in EDI -> clamp to 9 -> print one digit + newline -> exit(0) */
			0x89,0xF8,
			0x83,0xF8,0x09,
			0x76,0x05,
			0xB8,0x09,0x00,0x00,0x00,
			0x83,0xC0,0x30,
			0x88,0x05,0x21,0x00,0x00,0x00,
			0xB8,0x01,0x00,0x00,0x00,
			0xBF,0x01,0x00,0x00,0x00,
			0x48,0x8D,0x35,0x10,0x00,0x00,0x00,
			0xBA,0x02,0x00,0x00,0x00,
			0x0F,0x05,
			0xB8,0x3C,0x00,0x00,0x00,
			0x31,0xFF,
			0x0F,0x05,
			'?', '\n'
		};
		static const unsigned char brk_code[] = {
			/* brk(0x1800010000), verify return, then brk(0x1800000000), print OK */
			0xB8,0x0C,0x00,0x00,0x00,
			0x48,0xBF,0x00,0x00,0x01,0x00,0x18,0x00,0x00,0x00,
			0x0F,0x05,
			0x48,0xBB,0x00,0x00,0x01,0x00,0x18,0x00,0x00,0x00,
			0x48,0x39,0xD8,
			0x75,0x35,
			0x48,0xBB,0x00,0x00,0x00,0x00,0x18,0x00,0x00,0x00,
			0x48,0x89,0xDF,
			0xB8,0x0C,0x00,0x00,0x00,
			0x0F,0x05,
			0xB8,0x01,0x00,0x00,0x00,
			0xBF,0x01,0x00,0x00,0x00,
			0x48,0x8D,0x35,0x34,0x00,0x00,0x00,
			0xBA,0x03,0x00,0x00,0x00,
			0x0F,0x05,
			0xB8,0x3C,0x00,0x00,0x00,
			0x31,0xFF,
			0x0F,0x05,
			/* fail: print "E\n" and exit(1) */
			0xB8,0x01,0x00,0x00,0x00,
			0xBF,0x01,0x00,0x00,0x00,
			0x48,0x8D,0x35,0x16,0x00,0x00,0x00,
			0xBA,0x02,0x00,0x00,0x00,
			0x0F,0x05,
			0xB8,0x3C,0x00,0x00,0x00,
			0xBF,0x01,0x00,0x00,0x00,
			0x0F,0x05,
			'O','K','\n',
			'E','\n'
		};

		(void)seed_builtin_elf_with_symbols("/app.elf",  0xFFFF900003000000UL, 0x5u, app_code,  sizeof(app_code),  sizeof(app_code),  sizeof(app_code), "_start");
		(void)seed_builtin_elf("/appw.elf", 0xFFFF900003010000UL, 0x7u, appw_code, sizeof(appw_code), sizeof(appw_code), sizeof(appw_code));
		(void)seed_builtin_elf("/app2p.elf",0xFFFF900003020000UL, 0x5u, app2p_code, sizeof(app2p_code), 0x1100UL, 0x1800UL);
		(void)seed_builtin_elf_with_symbols("/panic.elf",0xFFFF900003030000UL, 0x5u, panic_code, sizeof(panic_code), sizeof(panic_code), sizeof(panic_code), "_crash");
		(void)seed_builtin_elf_with_symbols("/bss.elf",  0xFFFF900003040000UL, 0x7u, bss_code, sizeof(bss_code), sizeof(bss_code), sizeof(bss_code) + 4UL, "_bsscheck");
		(void)seed_builtin_elf_with_symbols_type(3u, "/pie.elf", 0x0000000000000000UL, 0x5u, pie_code, sizeof(pie_code), sizeof(pie_code), sizeof(pie_code), "_start");
		(void)seed_builtin_elf_with_symbols("/hello.elf",0xFFFF900003060000UL, 0x5u, hello_code, sizeof(hello_code), sizeof(hello_code), sizeof(hello_code), "_start");
		(void)seed_builtin_elf_with_symbols("/argc.elf", 0xFFFF900003070000UL, 0x7u, argc_code, sizeof(argc_code), sizeof(argc_code), sizeof(argc_code), "_start");
		(void)seed_builtin_elf_with_symbols("/brk.elf",  0xFFFF900003080000UL, 0x7u, brk_code, sizeof(brk_code), sizeof(brk_code), sizeof(brk_code), "_start");
		{
			/* /argv.elf — print argv[1] (or nothing if argc < 2), then '\n', then exit */
			static const unsigned char argv_code[] = {
				/* if (argc < 2) jmp .exit */
				0x83,0xFF,0x02,
				0x7C,0x3A,               /* jl .exit (adjusted offset) */
				/* RSI already contains argv[1] from kernel entry setup */
				/* rcx = strlen(rsi) */
				0x48,0x31,0xC9,
				/* .loop: cmp byte [rsi+rcx], 0 */
				0x80,0x3C,0x0E,0x00,
				0x74,0x05,               /* je .done */
				0x48,0xFF,0xC1,          /* inc rcx */
				0xEB,0xF5,               /* jmp .loop */
				/* .done: test rcx,rcx */
				0x48,0x85,0xC9,
				0x74,0x27,               /* jz .exit (offset to SYS_EXIT at +39) */
				/* SYS_WRITE(1, argv[1], strlen) */
				0xB8,0x01,0x00,0x00,0x00,
				0xBF,0x01,0x00,0x00,0x00,
				0x48,0x89,0xCA,
				0x0F,0x05,
				/* SYS_WRITE(1, "\n", 1) */
				0xB8,0x01,0x00,0x00,0x00,
				0xBF,0x01,0x00,0x00,0x00,
				0x48,0x8D,0x35,0x10,0x00,0x00,0x00,
				0xBA,0x01,0x00,0x00,0x00,
				0x0F,0x05,
				/* .exit: SYS_EXIT(0) */
				0xB8,0x3C,0x00,0x00,0x00,
				0x31,0xFF,
				0x0F,0x05,
				'\n'
			};
			(void)seed_builtin_elf_with_symbols("/argv.elf", 0x0000000100900000UL, 0x5u, argv_code, sizeof(argv_code), sizeof(argv_code), sizeof(argv_code), "_start");
		}
	}
}

int fs_mkdir(const char *path)
{
	int parent;
	char name[FS_NAME_MAX + 1];
	return split_parent_and_name(path, &parent, name) == 0 ? (add_child(parent, name, 1) >= 0 ? 0 : -1) : -1;
}

int fs_touch(const char *path)
{
	int parent;
	char name[FS_NAME_MAX + 1];
	if (split_parent_and_name(path, &parent, name) != 0) return -1;
	if (add_child(parent, name, 0) >= 0) return 0;

	/* If file already exists, treat touch as success */
	{
		int existing = find_child(parent, name);
		if (existing >= 0 && !nodes[existing].is_dir) return 0;
	}
	return -1;
}

int fs_write_file(const char *path, const unsigned char *data, unsigned long size)
{
	int node = resolve_path(path);
	unsigned long n;
	unsigned long i;

	if (node < 0)
	{
		if (fs_touch(path) != 0) return -1;
		node = resolve_path(path);
		if (node < 0) return -1;
	}

	if (nodes[node].is_dir) return -1;
	if (data == (void *)0 && size != 0) return -1;

	n = size;
	if (n >= FS_MAX_FILE_SIZE) n = FS_MAX_FILE_SIZE - 1;
	for (i = 0; i < n; i++) nodes[node].data[i] = (char)data[i];
	nodes[node].data[n] = '\0';
	nodes[node].size = n;
	return 0;
}

int fs_read_file(const char *path, unsigned char *out_data, unsigned long out_capacity, unsigned long *out_size)
{
	int node = resolve_path(path);
	unsigned long i;
	unsigned long n;
	if (node < 0 || nodes[node].is_dir || out_size == (void *)0) return -1;
	n = nodes[node].size;
	if (out_data != (void *)0)
	{
		unsigned long copy_n = (n < out_capacity) ? n : out_capacity;
		for (i = 0; i < copy_n; i++) out_data[i] = (unsigned char)nodes[node].data[i];
	}
	*out_size = (n < out_capacity) ? n : out_capacity;
	return 0;
}

int fs_write_text(const char *path, const char *text)
{
	return fs_write_file(path, (const unsigned char *)text, str_len(text));
}

int fs_read_text(const char *path, const char **out_text)
{
	int node = resolve_path(path);
	if (node < 0 || nodes[node].is_dir) return -1;
	*out_text = nodes[node].data;
	return 0;
}

int fs_rm(const char *path)
{
	int node = resolve_path(path);
	if (node <= 0) return -1;
	if (nodes[node].is_dir && nodes[node].first_child != -1) return -1;
	if (unlink_node_from_parent(node) != 0) return -1;
	nodes[node].used = 0;
	nodes[node].first_child = -1;
	nodes[node].next_sibling = -1;
	nodes[node].size = 0;
	nodes[node].name[0] = '\0';
	return 0;
}

int fs_cd(const char *path)
{
	int node = resolve_path(path);
	if (node < 0 || !nodes[node].is_dir) return -1;
	cwd_node = node;
	return 0;
}

int fs_cp(const char *src_path, const char *dst_path)
{
	int src = resolve_path(src_path);
	if (src < 0 || nodes[src].is_dir) return -1;
	return fs_write_file(dst_path, (const unsigned char *)nodes[src].data, nodes[src].size);
}

int fs_mv(const char *src_path, const char *dst_path)
{
	int src;
	int dst_parent;
	char dst_name[FS_NAME_MAX + 1];
	int old_parent;

	src = resolve_path(src_path);
	if (src <= 0) return -1;
	if (split_parent_and_name(dst_path, &dst_parent, dst_name) != 0) return -1;
	if (!nodes[dst_parent].is_dir) return -1;
	if (find_child(dst_parent, dst_name) != -1) return -1;
	if (nodes[src].is_dir)
	{
		int p = dst_parent;
		while (p != -1)
		{
			if (p == src) return -1;
			p = nodes[p].parent;
		}
	}

	old_parent = nodes[src].parent;
	if (unlink_node_from_parent(src) != 0) return -1;
	nodes[src].parent = dst_parent;
	nodes[src].next_sibling = nodes[dst_parent].first_child;
	nodes[dst_parent].first_child = src;
	str_copy(nodes[src].name, dst_name, sizeof(nodes[src].name));

	if (cwd_node == old_parent || cwd_node == src || old_parent != dst_parent)
	{
		/* no-op, path-based cwd stays valid by node index */
	}

	return 0;
}

void fs_get_pwd(char *buffer, unsigned long buffer_size)
{
	int stack[FS_MAX_NODES];
	int depth = 0;
	int n = cwd_node;
	unsigned long out = 0;
	int i;

	if (buffer_size == 0) return;

	if (cwd_node == 0)
	{
		if (buffer_size >= 2)
		{
			buffer[0] = '/';
			buffer[1] = '\0';
		}
		else
		{
			buffer[0] = '\0';
		}
		return;
	}

	while (n > 0 && depth < FS_MAX_NODES)
	{
		stack[depth++] = n;
		n = nodes[n].parent;
	}

	for (i = depth - 1; i >= 0; i--)
	{
		unsigned long j;
		if (out + 1 >= buffer_size) break;
		buffer[out++] = '/';
		for (j = 0; nodes[stack[i]].name[j] != '\0'; j++)
		{
			if (out + 1 >= buffer_size) break;
			buffer[out++] = nodes[stack[i]].name[j];
		}
	}

	buffer[out] = '\0';
}

int fs_ls(const char *path, char names[][FS_NAME_MAX + 2], int types[], int max_entries, int *out_count)
{
	int dir;
	int child;
	int count = 0;

	dir = (path == (void *)0 || path[0] == '\0') ? cwd_node : resolve_path(path);
	if (dir < 0 || !nodes[dir].is_dir) return -1;

	child = nodes[dir].first_child;
	while (child != -1 && count < max_entries)
	{
		int i = 0;
		while (nodes[child].name[i] != '\0' && i < FS_NAME_MAX)
		{
			names[count][i] = nodes[child].name[i];
			i++;
		}
		if (nodes[child].is_dir && i < FS_NAME_MAX + 1)
		{
			names[count][i++] = '/';
		}
		names[count][i] = '\0';
		types[count] = nodes[child].is_dir;
		count++;
		child = nodes[child].next_sibling;
	}

	*out_count = count;
	return 0;
}

int fs_stat(const char *path, int *out_is_dir, unsigned long *out_size)
{
	int node = resolve_path(path);
	if (node < 0) return -1;
	if (out_is_dir != (void *)0) *out_is_dir = nodes[node].is_dir;
	if (out_size != (void *)0) *out_size = nodes[node].is_dir ? 0 : nodes[node].size;
	return 0;
}

unsigned long fs_free_bytes(void)
{
	unsigned long i;
	unsigned long dir_nodes = 0;
	unsigned long used_file_bytes = 0;
	unsigned long per_file_cap = (unsigned long)(FS_MAX_FILE_SIZE - 1);
	unsigned long total_slots = (unsigned long)(FS_MAX_NODES - 1);

	for (i = 1; i < FS_MAX_NODES; i++)
	{
		if (!nodes[i].used) continue;
		if (nodes[i].is_dir) dir_nodes++;
		else used_file_bytes += nodes[i].size;
	}

	if (dir_nodes >= total_slots) return 0;
	{
		unsigned long total_file_capacity = (total_slots - dir_nodes) * per_file_cap;
		if (used_file_bytes >= total_file_capacity) return 0;
		return total_file_capacity - used_file_bytes;
	}
}
