#include "elf.h"
#include "memory.h"

/* ------------------------------------------------------------------ */
/* ELF64 type definitions (freestanding, no system headers)           */
/* ------------------------------------------------------------------ */

typedef unsigned long  Elf64_Addr;
typedef unsigned long  Elf64_Off;
typedef unsigned int   Elf64_Word;
typedef int            Elf64_Sword;
typedef unsigned long  Elf64_Xword;
typedef long           Elf64_Sxword;
typedef unsigned short Elf64_Half;

#define EI_NIDENT  16
#define EI_CLASS   4
#define EI_DATA    5
#define EI_VERSION 6
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ET_EXEC    2
#define ET_DYN     3
#define EM_X86_64  62

#define PT_LOAD    1

#define PF_X       0x1
#define PF_W       0x2
#define PF_R       0x4

typedef struct
{
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half    e_type;
    Elf64_Half    e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;
    Elf64_Off     e_phoff;
    Elf64_Off     e_shoff;
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize;
    Elf64_Half    e_phentsize;
    Elf64_Half    e_phnum;
    Elf64_Half    e_shentsize;
    Elf64_Half    e_shnum;
    Elf64_Half    e_shstrndx;
} Elf64_Ehdr;

typedef struct
{
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

/* ------------------------------------------------------------------ */
/* Private helpers                                                     */
/* ------------------------------------------------------------------ */

static void mem_copy(unsigned char *dst, const unsigned char *src, unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n; i++) dst[i] = src[i];
}

static void mem_zero_range(unsigned char *dst, unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n; i++) dst[i] = 0;
}

static unsigned long align_up_page(unsigned long v)
{
    return (v + MEMORY_PAGE_SIZE - 1) & ~(MEMORY_PAGE_SIZE - 1);
}

static int check_magic(const unsigned char *e_ident)
{
    return  e_ident[0] == 0x7F &&
            e_ident[1] == 'E'  &&
            e_ident[2] == 'L'  &&
            e_ident[3] == 'F';
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int elf_load(const unsigned char *data, unsigned long len, elf_exec_t *out)
{
    const Elf64_Ehdr *ehdr;
    unsigned long i;

    if (data == (void *)0 || out == (void *)0 || len < sizeof(Elf64_Ehdr))
        return ELF_ERR_NULL;

    ehdr = (const Elf64_Ehdr *)data;

    if (!check_magic(ehdr->e_ident))          return ELF_ERR_MAGIC;
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) return ELF_ERR_CLASS;
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
        return ELF_ERR_TYPE;
    if (ehdr->e_machine != EM_X86_64)          return ELF_ERR_ARCH;

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0 ||
        ehdr->e_phentsize < sizeof(Elf64_Phdr))
        return ELF_ERR_PHDR;

    /* Iterate PT_LOAD segments */
    for (i = 0; i < (unsigned long)ehdr->e_phnum; i++)
    {
        const Elf64_Phdr *phdr;
        unsigned long vaddr_start;
        unsigned long vaddr_end;
        unsigned long page;
        unsigned long flags;

        phdr = (const Elf64_Phdr *)(data + ehdr->e_phoff +
               i * (unsigned long)ehdr->e_phentsize);

        if (phdr->p_type != PT_LOAD) continue;

        /* Basic bounds check */
        if (ehdr->e_phoff + i * (unsigned long)ehdr->e_phentsize +
                sizeof(Elf64_Phdr) > len)
            return ELF_ERR_PHDR;

        if (phdr->p_offset + phdr->p_filesz > len)
            return ELF_ERR_RANGE;

        vaddr_start = phdr->p_vaddr & ~(MEMORY_PAGE_SIZE - 1);
        vaddr_end   = align_up_page(phdr->p_vaddr + phdr->p_memsz);

        /* Build page flags */
        flags = PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE;
        if (!(phdr->p_flags & PF_X))
            flags |= PAGE_FLAG_NO_EXECUTE;

        /* Map one page at a time, copy or zero fill as needed */
        for (page = vaddr_start; page < vaddr_end; page += MEMORY_PAGE_SIZE)
        {
            unsigned long phys;
            unsigned char *dest;
            unsigned long page_off;
            unsigned long copy_off;
            unsigned long copy_end;
            unsigned long copy_len;

            phys = phys_alloc_page();
            if (phys == 0) return ELF_ERR_MAP;

            if (paging_map_page(page, phys, flags) != 0)
            {
                phys_free_page(phys);
                return ELF_ERR_MAP;
            }

            dest = (unsigned char *)page;
            mem_zero_range(dest, MEMORY_PAGE_SIZE);

            /* Copy whatever file data falls in this page */
            page_off  = page - vaddr_start;
            copy_off  = page_off;
            copy_end  = page_off + MEMORY_PAGE_SIZE;

            /* Segment file data range relative to vaddr_start */
            {
                unsigned long seg_file_start = phdr->p_vaddr - vaddr_start;
                unsigned long seg_file_end   = seg_file_start + phdr->p_filesz;
                unsigned long src_base;
                unsigned long dst_off;
                unsigned long clamp_start;
                unsigned long clamp_end;

                if (copy_end <= seg_file_start || copy_off >= seg_file_end)
                    continue; /* pure zero page */

                clamp_start = copy_off < seg_file_start ? seg_file_start : copy_off;
                clamp_end   = copy_end > seg_file_end   ? seg_file_end   : copy_end;

                src_base = (unsigned long)(data + phdr->p_offset) -
                           (unsigned long)seg_file_start;

                dst_off  = clamp_start - page_off;
                copy_len = clamp_end - clamp_start;

                mem_copy(dest + dst_off,
                         (const unsigned char *)(src_base + clamp_start),
                         copy_len);
            }
        }
    }

    out->entry = (unsigned long)ehdr->e_entry;
    return ELF_OK;
}

long elf_call(const elf_exec_t *out)
{
    if (out == (void *)0) return -1;
    {
        typedef long (*entry_fn_t)(void);
        entry_fn_t fn = (entry_fn_t)out->entry;
        return fn();
    }
}

void elf_unload(const unsigned char *data, unsigned long len)
{
    const Elf64_Ehdr *ehdr;
    unsigned long i;

    if (data == (void *)0 || len < sizeof(Elf64_Ehdr)) return;
    ehdr = (const Elf64_Ehdr *)data;

    if (!check_magic(ehdr->e_ident) ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr->e_phoff == 0 || ehdr->e_phnum == 0 ||
        ehdr->e_phentsize < sizeof(Elf64_Phdr))
        return;

    for (i = 0; i < (unsigned long)ehdr->e_phnum; i++)
    {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(data + ehdr->e_phoff +
                                   i * (unsigned long)ehdr->e_phentsize);
        unsigned long vaddr_start;
        unsigned long vaddr_end;
        unsigned long page;

        if (phdr->p_type != PT_LOAD) continue;

        vaddr_start = phdr->p_vaddr & ~(MEMORY_PAGE_SIZE - 1);
        vaddr_end   = align_up_page(phdr->p_vaddr + phdr->p_memsz);

        for (page = vaddr_start; page < vaddr_end; page += MEMORY_PAGE_SIZE)
        {
            unsigned long phys = paging_get_phys(page);
            if (phys != 0)
            {
                paging_unmap_page(page);
                phys_free_page(phys);
            }
        }
    }
}
