#include "memory.h"

#include "arch.h"

#define MB2_TAG_END    0
#define MB2_TAG_MEMMAP 6

#define MB2_MEM_AVAILABLE 1

#define MEMORY_MAX_PHYS 0x100000000ULL
#define MEMORY_MAX_PAGES (MEMORY_MAX_PHYS / MEMORY_PAGE_SIZE)
#define MEMORY_BITMAP_BYTES (MEMORY_MAX_PAGES / 8)
#define KMALLOC_HEADER_MAGIC 0x4B4D414C4C4F435FUL

#define PAGE_TABLE_ENTRIES 512UL
#define PAGE_ADDR_MASK 0x000FFFFFFFFFF000UL
#define PAGE_LARGE_ADDR_MASK 0x000FFFFFFFE00000UL
#define PAGE_FLAGS_MASK 0xFFFUL

#define MEMORY_VIRT_BASE  0xFFFF900000000000UL
#define MEMORY_VIRT_PAGES 16384UL
#define MEMORY_VIRT_LIMIT (MEMORY_VIRT_BASE + MEMORY_VIRT_PAGES * MEMORY_PAGE_SIZE)
#define MEMORY_VIRT_BITMAP_BYTES (MEMORY_VIRT_PAGES / 8)
#define MEMORY_TEMP_PAGE_INDEX (MEMORY_VIRT_PAGES - 1)
#define MEMORY_TEMP_PAGE_VADDR (MEMORY_VIRT_BASE + MEMORY_TEMP_PAGE_INDEX * MEMORY_PAGE_SIZE)

struct mb2_info_header
{
	unsigned int total_size;
	unsigned int reserved;
};

struct mb2_tag_header
{
	unsigned int type;
	unsigned int size;
};

struct mb2_memmap_tag
{
	unsigned int type;
	unsigned int size;
	unsigned int entry_size;
	unsigned int entry_version;
};

struct mb2_memmap_entry
{
	unsigned long long base_addr;
	unsigned long long length;
	unsigned int type;
	unsigned int reserved;
};

extern char __kernel_start;
extern char __kernel_end;

static unsigned char phys_bitmap[MEMORY_BITMAP_BYTES];
static unsigned char virt_bitmap[MEMORY_VIRT_BITMAP_BYTES];
static unsigned long total_page_count = 0;
static unsigned long free_page_count = 0;

struct kmalloc_header
{
	unsigned long magic;
	unsigned long pages;
	unsigned long size;
	unsigned long reserved;
};

static void mem_fill(unsigned char *dst, unsigned char value, unsigned long len)
{
	unsigned long i;
	for (i = 0; i < len; i++) dst[i] = value;
}

static void mem_zero(void *dst, unsigned long len)
{
	mem_fill((unsigned char *)dst, 0, len);
}

static unsigned long align_up(unsigned long value, unsigned long align)
{
	return (value + align - 1) & ~(align - 1);
}

static unsigned long align_down(unsigned long value, unsigned long align)
{
	return value & ~(align - 1);
}

static int page_is_free(unsigned long page_index)
{
	return (phys_bitmap[page_index >> 3] & (unsigned char)(1u << (page_index & 7))) == 0;
}

static int virt_page_is_free(unsigned long page_index)
{
	return (virt_bitmap[page_index >> 3] & (unsigned char)(1u << (page_index & 7))) == 0;
}

static void page_mark_used(unsigned long page_index)
{
	phys_bitmap[page_index >> 3] |= (unsigned char)(1u << (page_index & 7));
}

static void page_mark_free(unsigned long page_index)
{
	phys_bitmap[page_index >> 3] &= (unsigned char)~(1u << (page_index & 7));
}

static void virt_page_mark_used(unsigned long page_index)
{
	virt_bitmap[page_index >> 3] |= (unsigned char)(1u << (page_index & 7));
}

static void virt_page_mark_free(unsigned long page_index)
{
	virt_bitmap[page_index >> 3] &= (unsigned char)~(1u << (page_index & 7));
}

static unsigned long pml4_index(unsigned long addr)
{
	return (addr >> 39) & 0x1FFUL;
}

static unsigned long pdpt_index(unsigned long addr)
{
	return (addr >> 30) & 0x1FFUL;
}

static unsigned long pd_index(unsigned long addr)
{
	return (addr >> 21) & 0x1FFUL;
}

static unsigned long pt_index(unsigned long addr)
{
	return (addr >> 12) & 0x1FFUL;
}

static unsigned long *page_table_from_entry(unsigned long entry)
{
	return (unsigned long *)(entry & PAGE_ADDR_MASK);
}

static unsigned long *root_page_table(void)
{
	return (unsigned long *)(arch_read_cr3() & PAGE_ADDR_MASK);
}

static unsigned long *alloc_page_table(void)
{
	unsigned long phys = phys_alloc_page();
	unsigned long *table;
	if (phys == 0) return (void *)0;
	table = (unsigned long *)phys;
	mem_zero(table, MEMORY_PAGE_SIZE);
	return table;
}

static unsigned long *ensure_next_table(unsigned long *entry)
{
	if ((*entry & PAGE_FLAG_PRESENT) != 0)
	{
		if ((*entry & PAGE_FLAG_HUGE) != 0) return (void *)0;
		return page_table_from_entry(*entry);
	}
	{
		unsigned long *table = alloc_page_table();
		if (table == (void *)0) return (void *)0;
		*entry = ((unsigned long)table & PAGE_ADDR_MASK) | PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE;
		return table;
	}
}

static unsigned long find_virtual_run(unsigned long count)
{
	unsigned long run = 0;
	unsigned long start = 0;
	unsigned long page;
	for (page = 0; page < MEMORY_VIRT_PAGES; page++)
	{
		if (virt_page_is_free(page))
		{
			if (run == 0) start = page;
			run++;
			if (run == count) return start;
		}
		else run = 0;
	}
	return MEMORY_VIRT_PAGES;
}

static void mark_virtual_run(unsigned long start, unsigned long count, int used)
{
	unsigned long i;
	for (i = 0; i < count; i++)
	{
		if (used) virt_page_mark_used(start + i);
		else virt_page_mark_free(start + i);
	}
}

static void release_region(unsigned long long start_addr, unsigned long long end_addr)
{
	unsigned long start;
	unsigned long end;
	unsigned long addr;
	if (start_addr >= MEMORY_MAX_PHYS) return;
	if (end_addr > MEMORY_MAX_PHYS) end_addr = MEMORY_MAX_PHYS;
	start = align_up((unsigned long)start_addr, MEMORY_PAGE_SIZE);
	end = align_down((unsigned long)end_addr, MEMORY_PAGE_SIZE);
	for (addr = start; addr < end; addr += MEMORY_PAGE_SIZE)
	{
		unsigned long page = addr / MEMORY_PAGE_SIZE;
		if (!page_is_free(page))
		{
			page_mark_free(page);
			total_page_count++;
			free_page_count++;
		}
	}
}

static void reserve_region(unsigned long start_addr, unsigned long end_addr)
{
	unsigned long start = align_down(start_addr, MEMORY_PAGE_SIZE);
	unsigned long end = align_up(end_addr, MEMORY_PAGE_SIZE);
	unsigned long addr;
	if (end > (unsigned long)MEMORY_MAX_PHYS) end = (unsigned long)MEMORY_MAX_PHYS;
	for (addr = start; addr < end; addr += MEMORY_PAGE_SIZE)
	{
		unsigned long page = addr / MEMORY_PAGE_SIZE;
		if (page < MEMORY_MAX_PAGES && page_is_free(page))
		{
			page_mark_used(page);
			free_page_count--;
		}
	}
}

void memory_init(unsigned long mb2_info_addr)
{
	const struct mb2_info_header *info = (const struct mb2_info_header *)mb2_info_addr;
	const struct mb2_tag_header *tag;
	unsigned long offset;

	mem_fill(phys_bitmap, 0xFF, sizeof(phys_bitmap));
	mem_zero(virt_bitmap, sizeof(virt_bitmap));
	virt_page_mark_used(MEMORY_TEMP_PAGE_INDEX);
	total_page_count = 0;
	free_page_count = 0;

	if (mb2_info_addr == 0) return;

	offset = mb2_info_addr + 8;
	while (1)
	{
		tag = (const struct mb2_tag_header *)offset;
		if (tag->type == MB2_TAG_END) break;
		if (tag->type == MB2_TAG_MEMMAP)
		{
			const struct mb2_memmap_tag *memmap = (const struct mb2_memmap_tag *)tag;
			unsigned long entry_off = offset + sizeof(struct mb2_memmap_tag);
			unsigned long end_off = offset + memmap->size;
			while (entry_off + memmap->entry_size <= end_off)
			{
				const struct mb2_memmap_entry *entry = (const struct mb2_memmap_entry *)entry_off;
				if (entry->type == MB2_MEM_AVAILABLE)
					release_region(entry->base_addr, entry->base_addr + entry->length);
				entry_off += memmap->entry_size;
			}
		}
		offset += (tag->size + 7) & ~7u;
	}

	reserve_region(0, 0x100000UL);
	reserve_region((unsigned long)&__kernel_start, (unsigned long)&__kernel_end);
	if (info->total_size != 0)
		reserve_region(mb2_info_addr, mb2_info_addr + info->total_size);
}

unsigned long phys_alloc_pages(unsigned long count)
{
	unsigned long run = 0;
	unsigned long start = 0;
	unsigned long page;
	if (count == 0) return 0;
	for (page = 0; page < MEMORY_MAX_PAGES; page++)
	{
		if (page_is_free(page))
		{
			if (run == 0) start = page;
			run++;
			if (run == count)
			{
				unsigned long i;
				for (i = 0; i < count; i++) page_mark_used(start + i);
				free_page_count -= count;
				return start * MEMORY_PAGE_SIZE;
			}
		}
		else run = 0;
	}
	return 0;
}

unsigned long phys_alloc_page(void)
{
	return phys_alloc_pages(1);
}

void phys_free_pages(unsigned long phys_addr, unsigned long count)
{
	unsigned long page = phys_addr / MEMORY_PAGE_SIZE;
	unsigned long i;
	for (i = 0; i < count; i++)
	{
		if (page + i < MEMORY_MAX_PAGES && !page_is_free(page + i))
		{
			page_mark_free(page + i);
			free_page_count++;
		}
	}
}

void phys_free_page(unsigned long phys_addr)
{
	phys_free_pages(phys_addr, 1);
}

int paging_map_page(unsigned long virt_addr, unsigned long phys_addr, unsigned long flags)
{
	unsigned long *pml4;
	unsigned long *pdpt;
	unsigned long *pd;
	unsigned long *pt;
	unsigned long *pml4e;
	unsigned long *pdpte;
	unsigned long *pde;
	unsigned long *entry;
	if ((virt_addr & (MEMORY_PAGE_SIZE - 1)) != 0 || (phys_addr & (MEMORY_PAGE_SIZE - 1)) != 0) return -1;
	pml4 = root_page_table();
	pml4e = &pml4[pml4_index(virt_addr)];
	pdpt = ensure_next_table(pml4e);
	if (pdpt == (void *)0) return -2;
	if ((flags & PAGE_FLAG_USER) != 0) *pml4e |= PAGE_FLAG_USER;
	if ((flags & PAGE_FLAG_WRITABLE) != 0) *pml4e |= PAGE_FLAG_WRITABLE;
	pdpte = &pdpt[pdpt_index(virt_addr)];
	pd = ensure_next_table(pdpte);
	if (pd == (void *)0) return -3;
	if ((flags & PAGE_FLAG_USER) != 0) *pdpte |= PAGE_FLAG_USER;
	if ((flags & PAGE_FLAG_WRITABLE) != 0) *pdpte |= PAGE_FLAG_WRITABLE;
	pde = &pd[pd_index(virt_addr)];
	pt = ensure_next_table(pde);
	if (pt == (void *)0) return -4;
	if ((flags & PAGE_FLAG_USER) != 0) *pde |= PAGE_FLAG_USER;
	if ((flags & PAGE_FLAG_WRITABLE) != 0) *pde |= PAGE_FLAG_WRITABLE;
	entry = &pt[pt_index(virt_addr)];
	if ((*entry & PAGE_FLAG_PRESENT) != 0) return -5;
	*entry = (phys_addr & PAGE_ADDR_MASK) | (flags & (PAGE_FLAGS_MASK | PAGE_FLAG_NO_EXECUTE)) | PAGE_FLAG_PRESENT;
	arch_invlpg((const void *)virt_addr);
	return 0;
}

int paging_set_page_flags(unsigned long virt_addr, unsigned long flags)
{
	unsigned long *pml4;
	unsigned long *pdpt;
	unsigned long *pd;
	unsigned long *pt;
	unsigned long *entry;
	unsigned long phys;
	if ((virt_addr & (MEMORY_PAGE_SIZE - 1)) != 0) return -1;
	pml4 = root_page_table();
	entry = &pml4[pml4_index(virt_addr)];
	if ((*entry & PAGE_FLAG_PRESENT) == 0) return -1;
	pdpt = page_table_from_entry(*entry);
	entry = &pdpt[pdpt_index(virt_addr)];
	if ((*entry & PAGE_FLAG_PRESENT) == 0) return -1;
	pd = page_table_from_entry(*entry);
	entry = &pd[pd_index(virt_addr)];
	if ((*entry & PAGE_FLAG_PRESENT) == 0 || (*entry & PAGE_FLAG_HUGE) != 0) return -1;
	pt = page_table_from_entry(*entry);
	entry = &pt[pt_index(virt_addr)];
	if ((*entry & PAGE_FLAG_PRESENT) == 0) return -1;
	phys = *entry & PAGE_ADDR_MASK;
	*entry = phys | (flags & (PAGE_FLAGS_MASK | PAGE_FLAG_NO_EXECUTE)) | PAGE_FLAG_PRESENT;
	arch_invlpg((const void *)virt_addr);
	return 0;
}

int paging_split_huge_page(unsigned long virt_addr)
{
	unsigned long *pml4;
	unsigned long *pdpt;
	unsigned long *pd;
	unsigned long *pde;
	unsigned long huge_phys;
	unsigned long huge_flags;
	unsigned long *new_pt;
	int i;

	if ((virt_addr & (MEMORY_PAGE_SIZE - 1)) != 0) return -1;

	pml4 = root_page_table();
	if ((pml4[pml4_index(virt_addr)] & PAGE_FLAG_PRESENT) == 0) return 0;
	pdpt = page_table_from_entry(pml4[pml4_index(virt_addr)]);
	if ((pdpt[pdpt_index(virt_addr)] & PAGE_FLAG_PRESENT) == 0) return 0;
	pd = page_table_from_entry(pdpt[pdpt_index(virt_addr)]);
	pde = &pd[pd_index(virt_addr)];

	if ((*pde & PAGE_FLAG_PRESENT) == 0) return 0;
	if ((*pde & PAGE_FLAG_HUGE) == 0) return 0; /* already 4K pages */

	huge_phys  = *pde & PAGE_LARGE_ADDR_MASK;
	huge_flags = *pde & (PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE);

	new_pt = alloc_page_table();
	if (new_pt == (void *)0) return -1;

	for (i = 0; i < 512; i++)
		new_pt[i] = (huge_phys + (unsigned long)i * MEMORY_PAGE_SIZE) | huge_flags;

	/* Replace PD entry: point to new page table, remove HUGE flag */
	*pde = ((unsigned long)new_pt & PAGE_ADDR_MASK) | PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE;

	/* Flush TLB for all 512 pages in the former huge page */
	{
		unsigned long base = virt_addr & ~(0x200000UL - 1);
		for (i = 0; i < 512; i++)
			arch_invlpg((const void *)(base + (unsigned long)i * MEMORY_PAGE_SIZE));
	}

	return 0;
}

void paging_unmap_page(unsigned long virt_addr)
{
	unsigned long *pml4;
	unsigned long *pdpt;
	unsigned long *pd;
	unsigned long *pt;
	unsigned long *entry;
	if ((virt_addr & (MEMORY_PAGE_SIZE - 1)) != 0) return;
	pml4 = root_page_table();
	entry = &pml4[pml4_index(virt_addr)];
	if ((*entry & PAGE_FLAG_PRESENT) == 0) return;
	pdpt = page_table_from_entry(*entry);
	entry = &pdpt[pdpt_index(virt_addr)];
	if ((*entry & PAGE_FLAG_PRESENT) == 0) return;
	pd = page_table_from_entry(*entry);
	entry = &pd[pd_index(virt_addr)];
	if ((*entry & PAGE_FLAG_PRESENT) == 0 || (*entry & PAGE_FLAG_HUGE) != 0) return;
	pt = page_table_from_entry(*entry);
	entry = &pt[pt_index(virt_addr)];
	if ((*entry & PAGE_FLAG_PRESENT) == 0) return;
	*entry = 0;
	arch_invlpg((const void *)virt_addr);
}

unsigned long paging_get_phys(unsigned long virt_addr)
{
	unsigned long *pml4;
	unsigned long *pdpt;
	unsigned long *pd;
	unsigned long *pt;
	unsigned long entry;
	pml4 = root_page_table();
	entry = pml4[pml4_index(virt_addr)];
	if ((entry & PAGE_FLAG_PRESENT) == 0) return 0;
	pdpt = page_table_from_entry(entry);
	entry = pdpt[pdpt_index(virt_addr)];
	if ((entry & PAGE_FLAG_PRESENT) == 0) return 0;
	pd = page_table_from_entry(entry);
	entry = pd[pd_index(virt_addr)];
	if ((entry & PAGE_FLAG_PRESENT) == 0) return 0;
	if ((entry & PAGE_FLAG_HUGE) != 0)
		return (entry & PAGE_LARGE_ADDR_MASK) | (virt_addr & 0x1FFFFFUL);
	pt = page_table_from_entry(entry);
	entry = pt[pt_index(virt_addr)];
	if ((entry & PAGE_FLAG_PRESENT) == 0) return 0;
	return (entry & PAGE_ADDR_MASK) | (virt_addr & (MEMORY_PAGE_SIZE - 1));
}

int paging_get_walk(unsigned long virt_addr,
	unsigned long *out_pml4e,
	unsigned long *out_pdpte,
	unsigned long *out_pde,
	unsigned long *out_pte)
{
	unsigned long *pml4;
	unsigned long entry;
	unsigned long *pdpt;
	unsigned long *pd;
	unsigned long *pt;

	if (out_pml4e != (void *)0) *out_pml4e = 0;
	if (out_pdpte != (void *)0) *out_pdpte = 0;
	if (out_pde != (void *)0) *out_pde = 0;
	if (out_pte != (void *)0) *out_pte = 0;

	pml4 = root_page_table();
	entry = pml4[pml4_index(virt_addr)];
	if (out_pml4e != (void *)0) *out_pml4e = entry;
	if ((entry & PAGE_FLAG_PRESENT) == 0) return -1;

	pdpt = page_table_from_entry(entry);
	entry = pdpt[pdpt_index(virt_addr)];
	if (out_pdpte != (void *)0) *out_pdpte = entry;
	if ((entry & PAGE_FLAG_PRESENT) == 0) return -1;

	pd = page_table_from_entry(entry);
	entry = pd[pd_index(virt_addr)];
	if (out_pde != (void *)0) *out_pde = entry;
	if ((entry & PAGE_FLAG_PRESENT) == 0) return -1;
	if ((entry & PAGE_FLAG_HUGE) != 0) return 1;

	pt = page_table_from_entry(entry);
	entry = pt[pt_index(virt_addr)];
	if (out_pte != (void *)0) *out_pte = entry;
	if ((entry & PAGE_FLAG_PRESENT) == 0) return -1;
	return 0;
}

void *virt_reserve_pages(unsigned long count)
{
	unsigned long virt_page;
	if (count == 0) return (void *)0;
	virt_page = find_virtual_run(count);
	if (virt_page >= MEMORY_VIRT_PAGES) return (void *)0;
	mark_virtual_run(virt_page, count, 1);
	return (void *)(MEMORY_VIRT_BASE + virt_page * MEMORY_PAGE_SIZE);
}

void *virt_alloc_pages(unsigned long count)
{
	unsigned long phys;
	unsigned long virt_page;
	unsigned long i;
	if (count == 0) return (void *)0;
	virt_page = find_virtual_run(count);
	if (virt_page >= MEMORY_VIRT_PAGES) return (void *)0;
	phys = phys_alloc_pages(count);
	if (phys == 0) return (void *)0;
	mark_virtual_run(virt_page, count, 1);
	for (i = 0; i < count; i++)
	{
		unsigned long virt = MEMORY_VIRT_BASE + (virt_page + i) * MEMORY_PAGE_SIZE;
		if (paging_map_page(virt, phys + i * MEMORY_PAGE_SIZE, PAGE_FLAG_WRITABLE) != 0)
		{
			while (i > 0)
			{
				i--;
				paging_unmap_page(MEMORY_VIRT_BASE + (virt_page + i) * MEMORY_PAGE_SIZE);
			}
			mark_virtual_run(virt_page, count, 0);
			phys_free_pages(phys, count);
			return (void *)0;
		}
	}
	return (void *)(MEMORY_VIRT_BASE + virt_page * MEMORY_PAGE_SIZE);
}

void virt_free_pages(void *addr, unsigned long count)
{
	unsigned long virt_addr = (unsigned long)addr;
	unsigned long virt_page;
	unsigned long i;
	if (addr == (void *)0 || count == 0) return;
	if (virt_addr < MEMORY_VIRT_BASE || virt_addr >= MEMORY_VIRT_LIMIT) return;
	if ((virt_addr & (MEMORY_PAGE_SIZE - 1)) != 0) return;
	virt_page = (virt_addr - MEMORY_VIRT_BASE) / MEMORY_PAGE_SIZE;
	for (i = 0; i < count && virt_page + i < MEMORY_VIRT_PAGES; i++)
	{
		unsigned long page_virt = virt_addr + i * MEMORY_PAGE_SIZE;
		unsigned long phys = paging_get_phys(page_virt);
		if (phys != 0)
		{
			paging_unmap_page(page_virt);
			phys_free_page(phys & PAGE_ADDR_MASK);
		}
		virt_page_mark_free(virt_page + i);
	}
}

void *kmalloc(unsigned long size)
{
	unsigned long header_size;
	unsigned long payload_size;
	unsigned long total_bytes;
	unsigned long pages;
	void *base;
	struct kmalloc_header *hdr;
	if (size == 0) return (void *)0;
	header_size = align_up((unsigned long)sizeof(struct kmalloc_header), 16);
	payload_size = align_up(size, 16);
	total_bytes = header_size + payload_size;
	pages = (total_bytes + MEMORY_PAGE_SIZE - 1) / MEMORY_PAGE_SIZE;
	base = virt_alloc_pages(pages);
	if (base == (void *)0) return (void *)0;
	hdr = (struct kmalloc_header *)base;
	hdr->magic = KMALLOC_HEADER_MAGIC;
	hdr->pages = pages;
	hdr->size = size;
	hdr->reserved = 0;
	return (void *)((unsigned long)base + header_size);
}

void kfree(void *ptr)
{
	unsigned long header_size;
	unsigned long ptr_addr;
	unsigned long header_addr;
	struct kmalloc_header *hdr;
	if (ptr == (void *)0) return;
	ptr_addr = (unsigned long)ptr;
	header_size = align_up((unsigned long)sizeof(struct kmalloc_header), 16);
	if (ptr_addr < MEMORY_VIRT_BASE + header_size || ptr_addr >= MEMORY_VIRT_LIMIT) return;
	header_addr = ptr_addr - header_size;
	hdr = (struct kmalloc_header *)header_addr;
	if (hdr->magic != KMALLOC_HEADER_MAGIC || hdr->pages == 0) return;
	hdr->magic = 0;
	virt_free_pages((void *)header_addr, hdr->pages);
}

void memory_zero_phys_page(unsigned long phys_addr)
{
	if ((phys_addr & (MEMORY_PAGE_SIZE - 1)) != 0) return;
	if (paging_map_page(MEMORY_TEMP_PAGE_VADDR, phys_addr, PAGE_FLAG_WRITABLE) != 0) return;
	mem_zero((void *)MEMORY_TEMP_PAGE_VADDR, MEMORY_PAGE_SIZE);
	paging_unmap_page(MEMORY_TEMP_PAGE_VADDR);
	arch_invlpg((const void *)MEMORY_TEMP_PAGE_VADDR);
}

unsigned long memory_total_pages(void)
{
	return total_page_count;
}

unsigned long memory_free_pages(void)
{
	return free_page_count;
}

unsigned long memory_virtual_base(void)
{
	return MEMORY_VIRT_BASE;
}

unsigned long memory_virtual_limit(void)
{
	return MEMORY_VIRT_LIMIT;
}