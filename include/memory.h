#ifndef TG11_MEMORY_H
#define TG11_MEMORY_H

#define MEMORY_PAGE_SIZE 4096UL

#define PAGE_FLAG_PRESENT       0x001UL
#define PAGE_FLAG_WRITABLE      0x002UL
#define PAGE_FLAG_USER          0x004UL
#define PAGE_FLAG_WRITE_THROUGH 0x008UL
#define PAGE_FLAG_CACHE_DISABLE 0x010UL
#define PAGE_FLAG_HUGE          0x080UL
#define PAGE_FLAG_GLOBAL        0x100UL
#define PAGE_FLAG_NO_EXECUTE    (1UL << 63)

void memory_init(unsigned long mb2_info_addr);
unsigned long phys_alloc_page(void);
unsigned long phys_alloc_pages(unsigned long count);
void phys_free_page(unsigned long phys_addr);
void phys_free_pages(unsigned long phys_addr, unsigned long count);
int paging_map_page(unsigned long virt_addr, unsigned long phys_addr, unsigned long flags);
int paging_set_page_flags(unsigned long virt_addr, unsigned long flags);
void paging_unmap_page(unsigned long virt_addr);
unsigned long paging_get_phys(unsigned long virt_addr);
void *virt_alloc_pages(unsigned long count);
void virt_free_pages(void *addr, unsigned long count);
void *kmalloc(unsigned long size);
void kfree(void *ptr);
unsigned long memory_total_pages(void);
unsigned long memory_free_pages(void);
unsigned long memory_virtual_base(void);
unsigned long memory_virtual_limit(void);

#endif