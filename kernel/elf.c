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

typedef struct
{
    Elf64_Word  sh_name;
    Elf64_Word  sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr  sh_addr;
    Elf64_Off   sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word  sh_link;
    Elf64_Word  sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
} Elf64_Shdr;

typedef struct
{
    Elf64_Word  st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Half  st_shndx;
    Elf64_Addr  st_value;
    Elf64_Xword st_size;
} Elf64_Sym;

#define SHT_SYMTAB  2
#define SHT_STRTAB  3
#define SHT_DYNSYM 11

#define SHN_UNDEF   0

#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2

#define ELF_ACTIVE_IMAGE_MAX 8

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

static int add_overflows_ulong(unsigned long a, unsigned long b)
{
    return a + b < a;
}

static int is_power_of_two_ulong(unsigned long v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

static void rollback_mapped_pages(const unsigned long *virt_pages, const unsigned long *phys_pages, unsigned long count)
{
    unsigned long i;
    for (i = count; i > 0; i--)
    {
        unsigned long idx = i - 1;
        paging_unmap_page(virt_pages[idx]);
        phys_free_page(phys_pages[idx]);
    }
}

static int check_magic(const unsigned char *e_ident)
{
    return  e_ident[0] == 0x7F &&
            e_ident[1] == 'E'  &&
            e_ident[2] == 'L'  &&
            e_ident[3] == 'F';
}

struct elf_active_image
{
    int used;
    const unsigned char *data;
    unsigned long len;
    unsigned long load_base;
    unsigned long load_end;
};

static struct elf_active_image elf_active_images[ELF_ACTIVE_IMAGE_MAX];

static int validate_elf64_image(const unsigned char *data, unsigned long len, const Elf64_Ehdr **out_ehdr)
{
    const Elf64_Ehdr *ehdr;

    if (data == (void *)0 || out_ehdr == (void *)0 || len < sizeof(Elf64_Ehdr))
        return ELF_ERR_NULL;

    ehdr = (const Elf64_Ehdr *)data;
    if (!check_magic(ehdr->e_ident)) return ELF_ERR_MAGIC;
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) return ELF_ERR_CLASS;
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) return ELF_ERR_CLASS;
    if (ehdr->e_machine != EM_X86_64) return ELF_ERR_ARCH;

    *out_ehdr = ehdr;
    return ELF_OK;
}

static int validate_section_headers(const Elf64_Ehdr *ehdr, unsigned long len)
{
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0 || ehdr->e_shentsize < sizeof(Elf64_Shdr))
        return ELF_ERR_SHDR;
    if (ehdr->e_shoff >= len) return ELF_ERR_SHDR;
    if ((unsigned long)ehdr->e_shnum > ((len - ehdr->e_shoff) / (unsigned long)ehdr->e_shentsize))
        return ELF_ERR_SHDR;
    return ELF_OK;
}

static const Elf64_Shdr *section_header_at(const unsigned char *data, const Elf64_Ehdr *ehdr, unsigned long index)
{
    return (const Elf64_Shdr *)(data + ehdr->e_shoff + index * (unsigned long)ehdr->e_shentsize);
}

static const char *string_at(const unsigned char *base, unsigned long size, unsigned long offset)
{
    unsigned long i;
    if (offset >= size) return "";
    for (i = offset; i < size; i++)
    {
        if (base[i] == '\0') return (const char *)(base + offset);
    }
    return "";
}

static int count_symbols(const unsigned char *data, unsigned long len, const Elf64_Ehdr *ehdr, unsigned long *out_count)
{
    unsigned long i;
    unsigned long count = 0;

    if (out_count == (void *)0) return ELF_ERR_NULL;
    *out_count = 0;

    if (validate_section_headers(ehdr, len) != ELF_OK)
        return ELF_ERR_SHDR;

    for (i = 0; i < (unsigned long)ehdr->e_shnum; i++)
    {
        const Elf64_Shdr *symtab = section_header_at(data, ehdr, i);
        const Elf64_Shdr *strtab;
        const unsigned char *str_base;
        unsigned long j;
        unsigned long sym_count;

        if (symtab->sh_type != SHT_SYMTAB && symtab->sh_type != SHT_DYNSYM) continue;
        if (symtab->sh_entsize < sizeof(Elf64_Sym) || symtab->sh_size < symtab->sh_entsize) continue;
        if (symtab->sh_offset + symtab->sh_size > len) return ELF_ERR_SHDR;
        if (symtab->sh_link >= ehdr->e_shnum) return ELF_ERR_SHDR;

        strtab = section_header_at(data, ehdr, symtab->sh_link);
        if (strtab->sh_type != SHT_STRTAB) return ELF_ERR_SHDR;
        if (strtab->sh_offset + strtab->sh_size > len) return ELF_ERR_SHDR;

        str_base = data + strtab->sh_offset;
        sym_count = (unsigned long)(symtab->sh_size / symtab->sh_entsize);
        for (j = 0; j < sym_count; j++)
        {
            const Elf64_Sym *sym = (const Elf64_Sym *)(data + symtab->sh_offset + j * (unsigned long)symtab->sh_entsize);
            const char *name = string_at(str_base, (unsigned long)strtab->sh_size, sym->st_name);
            if (name[0] == '\0') continue;
            if (sym->st_shndx == SHN_UNDEF) continue;
            count++;
        }
    }

    if (count == 0) return ELF_ERR_NOSYM;
    *out_count = count;
    return ELF_OK;
}

static int get_program_header_window(const Elf64_Ehdr *ehdr, unsigned long len)
{
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0 || ehdr->e_phentsize < sizeof(Elf64_Phdr))
        return ELF_ERR_PHDR;
    if (ehdr->e_phoff >= len) return ELF_ERR_PHDR;
    if ((unsigned long)ehdr->e_phnum > ((len - ehdr->e_phoff) / (unsigned long)ehdr->e_phentsize))
        return ELF_ERR_PHDR;
    return ELF_OK;
}

static void collect_load_range(const unsigned char *data, const Elf64_Ehdr *ehdr, unsigned long *out_base, unsigned long *out_end)
{
    unsigned long i;
    unsigned long base = 0;
    unsigned long end = 0;

    for (i = 0; i < (unsigned long)ehdr->e_phnum; i++)
    {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(data + ehdr->e_phoff + i * (unsigned long)ehdr->e_phentsize);
        unsigned long seg_end;

        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) continue;
        seg_end = phdr->p_vaddr + phdr->p_memsz;
        if (base == 0 || phdr->p_vaddr < base) base = phdr->p_vaddr;
        if (seg_end > end) end = seg_end;
    }

    if (out_base != (void *)0) *out_base = base;
    if (out_end != (void *)0) *out_end = end;
}

static int validate_load_segments(const unsigned char *data, unsigned long len, const Elf64_Ehdr *ehdr)
{
    unsigned long i;
    int load_count = 0;

    for (i = 0; i < (unsigned long)ehdr->e_phnum; i++)
    {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(data + ehdr->e_phoff + i * (unsigned long)ehdr->e_phentsize);
        unsigned long seg_end;
        unsigned long page_start;
        unsigned long page_end;
        unsigned long j;

        if (phdr->p_type != PT_LOAD) continue;
        load_count++;

        if (phdr->p_offset + phdr->p_filesz > len)
            return ELF_ERR_RANGE;
        if (phdr->p_memsz < phdr->p_filesz)
            return ELF_ERR_RANGE;
        if (phdr->p_memsz == 0) continue;
        if (add_overflows_ulong(phdr->p_vaddr, phdr->p_memsz))
            return ELF_ERR_RANGE;
        if (add_overflows_ulong(phdr->p_offset, phdr->p_filesz))
            return ELF_ERR_RANGE;
        if (phdr->p_align > 1)
        {
            if (!is_power_of_two_ulong(phdr->p_align)) return ELF_ERR_PHDR;
            if (((unsigned long)phdr->p_vaddr & (phdr->p_align - 1)) != ((unsigned long)phdr->p_offset & (phdr->p_align - 1)))
                return ELF_ERR_PHDR;
        }

        seg_end = phdr->p_vaddr + phdr->p_memsz;
        page_start = phdr->p_vaddr & ~(MEMORY_PAGE_SIZE - 1);
        page_end = align_up_page(seg_end);

        for (j = i + 1; j < (unsigned long)ehdr->e_phnum; j++)
        {
            const Elf64_Phdr *other = (const Elf64_Phdr *)(data + ehdr->e_phoff + j * (unsigned long)ehdr->e_phentsize);
            unsigned long other_end;
            unsigned long other_page_start;
            unsigned long other_page_end;

            if (other->p_type != PT_LOAD || other->p_memsz == 0) continue;
            if (add_overflows_ulong(other->p_vaddr, other->p_memsz))
                return ELF_ERR_RANGE;

            other_end = other->p_vaddr + other->p_memsz;
            other_page_start = other->p_vaddr & ~(MEMORY_PAGE_SIZE - 1);
            other_page_end = align_up_page(other_end);

            if (page_start < other_page_end && other_page_start < page_end)
                return ELF_ERR_OVERLAP;
        }
    }

    if (load_count == 0) return ELF_ERR_PHDR;

    for (i = 0; i < (unsigned long)ehdr->e_phnum; i++)
    {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(data + ehdr->e_phoff + i * (unsigned long)ehdr->e_phentsize);
        unsigned long seg_end;

        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) continue;
        seg_end = phdr->p_vaddr + phdr->p_memsz;
        if ((unsigned long)ehdr->e_entry >= phdr->p_vaddr && (unsigned long)ehdr->e_entry < seg_end)
            return ELF_OK;
    }

    return ELF_ERR_ENTRY;
}

static void register_active_image(const unsigned char *data, unsigned long len, unsigned long load_base, unsigned long load_end)
{
    unsigned long i;
    unsigned long free_slot = ELF_ACTIVE_IMAGE_MAX;

    if (data == (void *)0 || len == 0 || load_end <= load_base) return;

    for (i = 0; i < ELF_ACTIVE_IMAGE_MAX; i++)
    {
        if (elf_active_images[i].used && elf_active_images[i].data == data && elf_active_images[i].len == len)
        {
            elf_active_images[i].load_base = load_base;
            elf_active_images[i].load_end = load_end;
            return;
        }
        if (!elf_active_images[i].used && free_slot == ELF_ACTIVE_IMAGE_MAX) free_slot = i;
    }

    if (free_slot == ELF_ACTIVE_IMAGE_MAX) return;
    elf_active_images[free_slot].used = 1;
    elf_active_images[free_slot].data = data;
    elf_active_images[free_slot].len = len;
    elf_active_images[free_slot].load_base = load_base;
    elf_active_images[free_slot].load_end = load_end;
}

static void unregister_active_image(const unsigned char *data, unsigned long len)
{
    unsigned long i;
    for (i = 0; i < ELF_ACTIVE_IMAGE_MAX; i++)
    {
        if (elf_active_images[i].used && elf_active_images[i].data == data && elf_active_images[i].len == len)
        {
            elf_active_images[i].used = 0;
            elf_active_images[i].data = (void *)0;
            elf_active_images[i].len = 0;
            elf_active_images[i].load_base = 0;
            elf_active_images[i].load_end = 0;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int elf_get_info(const unsigned char *data, unsigned long len, elf_info_t *out)
{
    const Elf64_Ehdr *ehdr;
    int rc;

    if (out == (void *)0) return ELF_ERR_NULL;
    rc = validate_elf64_image(data, len, &ehdr);
    if (rc != ELF_OK) return rc;

    out->type = ehdr->e_type;
    out->machine = ehdr->e_machine;
    out->phnum = ehdr->e_phnum;
    out->shnum = ehdr->e_shnum;
    out->entry = ehdr->e_entry;
    out->load_base = 0;
    out->load_end = 0;
    out->symbol_count = 0;

    if (ehdr->e_phoff != 0 && ehdr->e_phnum != 0)
    {
        rc = get_program_header_window(ehdr, len);
        if (rc != ELF_OK) return rc;
        collect_load_range(data, ehdr, &out->load_base, &out->load_end);
    }

    rc = count_symbols(data, len, ehdr, &out->symbol_count);
    if (rc != ELF_OK && rc != ELF_ERR_NOSYM) return rc;
    if (rc == ELF_ERR_NOSYM) out->symbol_count = 0;

    return ELF_OK;
}

int elf_visit_program_headers(const unsigned char *data, unsigned long len,
                              elf_program_header_visit_fn visit, void *ctx)
{
    const Elf64_Ehdr *ehdr;
    unsigned long i;
    int rc;

    if (visit == (void *)0) return ELF_ERR_NULL;
    rc = validate_elf64_image(data, len, &ehdr);
    if (rc != ELF_OK) return rc;
    rc = get_program_header_window(ehdr, len);
    if (rc != ELF_OK) return rc;

    for (i = 0; i < (unsigned long)ehdr->e_phnum; i++)
    {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(data + ehdr->e_phoff + i * (unsigned long)ehdr->e_phentsize);
        elf_program_header_info_t out_ph;

        out_ph.index = (unsigned int)i;
        out_ph.type = phdr->p_type;
        out_ph.flags = phdr->p_flags;
        out_ph.offset = phdr->p_offset;
        out_ph.vaddr = phdr->p_vaddr;
        out_ph.filesz = phdr->p_filesz;
        out_ph.memsz = phdr->p_memsz;
        out_ph.align = phdr->p_align;

        if (visit(&out_ph, ctx) != 0) return ELF_OK;
    }

    return ELF_OK;
}

int elf_visit_sections(const unsigned char *data, unsigned long len,
                       elf_section_visit_fn visit, void *ctx)
{
    const Elf64_Ehdr *ehdr;
    const Elf64_Shdr *shstr;
    const unsigned char *shstr_base = (void *)0;
    unsigned long shstr_size = 0;
    unsigned long i;
    int rc;

    if (visit == (void *)0) return ELF_ERR_NULL;
    rc = validate_elf64_image(data, len, &ehdr);
    if (rc != ELF_OK) return rc;
    rc = validate_section_headers(ehdr, len);
    if (rc != ELF_OK) return rc;

    if (ehdr->e_shstrndx < ehdr->e_shnum)
    {
        shstr = section_header_at(data, ehdr, ehdr->e_shstrndx);
        if (shstr->sh_type != SHT_STRTAB) return ELF_ERR_SHDR;
        if (shstr->sh_offset + shstr->sh_size > len) return ELF_ERR_SHDR;
        shstr_base = data + shstr->sh_offset;
        shstr_size = (unsigned long)shstr->sh_size;
    }

    for (i = 0; i < (unsigned long)ehdr->e_shnum; i++)
    {
        const Elf64_Shdr *shdr = section_header_at(data, ehdr, i);
        elf_section_info_t out_sh;

        if (shdr->sh_offset + shdr->sh_size > len && shdr->sh_type != 8 /* SHT_NOBITS */)
            return ELF_ERR_SHDR;

        out_sh.index = (unsigned int)i;
        out_sh.name = (shstr_base == (void *)0) ? "" : string_at(shstr_base, shstr_size, shdr->sh_name);
        out_sh.type = shdr->sh_type;
        out_sh.flags = shdr->sh_flags;
        out_sh.addr = shdr->sh_addr;
        out_sh.offset = shdr->sh_offset;
        out_sh.size = shdr->sh_size;
        out_sh.entsize = shdr->sh_entsize;
        out_sh.link = shdr->sh_link;
        out_sh.info = shdr->sh_info;

        if (visit(&out_sh, ctx) != 0) return ELF_OK;
    }

    return ELF_OK;
}

int elf_symbolize_active_addr(unsigned long addr, elf_symbol_t *out_symbol,
                              unsigned long *out_offset,
                              unsigned long *out_image_base,
                              unsigned long *out_image_end)
{
    unsigned long i;

    if (out_symbol == (void *)0) return ELF_ERR_NULL;

    for (i = 0; i < ELF_ACTIVE_IMAGE_MAX; i++)
    {
        if (!elf_active_images[i].used) continue;
        if (addr < elf_active_images[i].load_base || addr >= elf_active_images[i].load_end) continue;

        if (elf_find_symbol_by_addr(elf_active_images[i].data, elf_active_images[i].len, addr, out_symbol, out_offset) == ELF_OK)
        {
            if (out_image_base != (void *)0) *out_image_base = elf_active_images[i].load_base;
            if (out_image_end != (void *)0) *out_image_end = elf_active_images[i].load_end;
            return ELF_OK;
        }
    }

    return ELF_ERR_NOSYM;
}

int elf_visit_symbols(const unsigned char *data, unsigned long len, elf_symbol_visit_fn visit, void *ctx)
{
    const Elf64_Ehdr *ehdr;
    unsigned long i;
    int rc;
    unsigned long visited = 0;

    if (visit == (void *)0) return ELF_ERR_NULL;
    rc = validate_elf64_image(data, len, &ehdr);
    if (rc != ELF_OK) return rc;
    rc = validate_section_headers(ehdr, len);
    if (rc != ELF_OK) return rc;

    for (i = 0; i < (unsigned long)ehdr->e_shnum; i++)
    {
        const Elf64_Shdr *symtab = section_header_at(data, ehdr, i);
        const Elf64_Shdr *strtab;
        const unsigned char *str_base;
        unsigned long j;
        unsigned long sym_count;

        if (symtab->sh_type != SHT_SYMTAB && symtab->sh_type != SHT_DYNSYM) continue;
        if (symtab->sh_entsize < sizeof(Elf64_Sym) || symtab->sh_size < symtab->sh_entsize) continue;
        if (symtab->sh_offset + symtab->sh_size > len) return ELF_ERR_SHDR;
        if (symtab->sh_link >= ehdr->e_shnum) return ELF_ERR_SHDR;

        strtab = section_header_at(data, ehdr, symtab->sh_link);
        if (strtab->sh_offset + strtab->sh_size > len) return ELF_ERR_SHDR;
        str_base = data + strtab->sh_offset;
        sym_count = (unsigned long)(symtab->sh_size / symtab->sh_entsize);

        for (j = 0; j < sym_count; j++)
        {
            const Elf64_Sym *sym = (const Elf64_Sym *)(data + symtab->sh_offset + j * (unsigned long)symtab->sh_entsize);
            const char *name = string_at(str_base, (unsigned long)strtab->sh_size, sym->st_name);
            elf_symbol_t out_sym;

            if (name[0] == '\0') continue;
            if (sym->st_shndx == SHN_UNDEF) continue;

            out_sym.name = name;
            out_sym.value = sym->st_value;
            out_sym.size = sym->st_size;
            out_sym.info = sym->st_info;
            out_sym.shndx = sym->st_shndx;
            visited++;

            if (visit(&out_sym, ctx) != 0)
                return ELF_OK;
        }
    }

    return visited == 0 ? ELF_ERR_NOSYM : ELF_OK;
}

int elf_find_symbol_by_addr(const unsigned char *data, unsigned long len, unsigned long addr, elf_symbol_t *out_symbol,
                            unsigned long *out_offset)
{
    const Elf64_Ehdr *ehdr;
    unsigned long i;
    int rc;
    int found_containing = 0;
    int found_preceding = 0;
    elf_symbol_t best_sym;
    unsigned long best_offset = 0;

    if (out_symbol == (void *)0) return ELF_ERR_NULL;
    rc = validate_elf64_image(data, len, &ehdr);
    if (rc != ELF_OK) return rc;
    rc = validate_section_headers(ehdr, len);
    if (rc != ELF_OK) return rc;

    for (i = 0; i < (unsigned long)ehdr->e_shnum; i++)
    {
        const Elf64_Shdr *symtab = section_header_at(data, ehdr, i);
        const Elf64_Shdr *strtab;
        const unsigned char *str_base;
        unsigned long j;
        unsigned long sym_count;

        if (symtab->sh_type != SHT_SYMTAB && symtab->sh_type != SHT_DYNSYM) continue;
        if (symtab->sh_entsize < sizeof(Elf64_Sym) || symtab->sh_size < symtab->sh_entsize) continue;
        if (symtab->sh_offset + symtab->sh_size > len) return ELF_ERR_SHDR;
        if (symtab->sh_link >= ehdr->e_shnum) return ELF_ERR_SHDR;

        strtab = section_header_at(data, ehdr, symtab->sh_link);
        if (strtab->sh_offset + strtab->sh_size > len) return ELF_ERR_SHDR;
        str_base = data + strtab->sh_offset;
        sym_count = (unsigned long)(symtab->sh_size / symtab->sh_entsize);

        for (j = 0; j < sym_count; j++)
        {
            const Elf64_Sym *sym = (const Elf64_Sym *)(data + symtab->sh_offset + j * (unsigned long)symtab->sh_entsize);
            const char *name = string_at(str_base, (unsigned long)strtab->sh_size, sym->st_name);
            unsigned char type;

            if (name[0] == '\0') continue;
            if (sym->st_shndx == SHN_UNDEF) continue;
            if (addr < sym->st_value) continue;

            type = (unsigned char)(sym->st_info & 0x0Fu);
            if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE) continue;

            if (sym->st_size != 0 && addr < (sym->st_value + sym->st_size))
            {
                if (!found_containing || sym->st_value > best_sym.value)
                {
                    best_sym.name = name;
                    best_sym.value = sym->st_value;
                    best_sym.size = sym->st_size;
                    best_sym.info = sym->st_info;
                    best_sym.shndx = sym->st_shndx;
                    best_offset = addr - sym->st_value;
                    found_containing = 1;
                }
            }
            else if (!found_containing && (!found_preceding || sym->st_value > best_sym.value))
            {
                best_sym.name = name;
                best_sym.value = sym->st_value;
                best_sym.size = sym->st_size;
                best_sym.info = sym->st_info;
                best_sym.shndx = sym->st_shndx;
                best_offset = addr - sym->st_value;
                found_preceding = 1;
            }
        }
    }

    if (!found_containing && !found_preceding) return ELF_ERR_NOSYM;
    *out_symbol = best_sym;
    if (out_offset != (void *)0) *out_offset = best_offset;
    return ELF_OK;
}

int elf_load(const unsigned char *data, unsigned long len, elf_exec_t *out)
{
    const Elf64_Ehdr *ehdr;
    unsigned long i;
    unsigned long mapped_count = 0;
    unsigned long mapped_virt[1024];
    unsigned long mapped_phys[1024];

    if (data == (void *)0 || out == (void *)0 || len < sizeof(Elf64_Ehdr))
        return ELF_ERR_NULL;

    ehdr = (const Elf64_Ehdr *)data;

    if (!check_magic(ehdr->e_ident))          return ELF_ERR_MAGIC;
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) return ELF_ERR_CLASS;
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
        return ELF_ERR_TYPE;
    if (ehdr->e_machine != EM_X86_64)          return ELF_ERR_ARCH;

    if (get_program_header_window(ehdr, len) != ELF_OK)
        return ELF_ERR_PHDR;
    {
        int rc = validate_load_segments(data, len, ehdr);
        if (rc != ELF_OK) return rc;
    }

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
        if (ehdr->e_phoff + i * (unsigned long)ehdr->e_phentsize + sizeof(Elf64_Phdr) > len)
            return ELF_ERR_PHDR;

        if (phdr->p_memsz == 0) continue;

        vaddr_start = phdr->p_vaddr & ~(MEMORY_PAGE_SIZE - 1);
        vaddr_end   = align_up_page(phdr->p_vaddr + phdr->p_memsz);

        /* Map writable while loading so segment bytes can be copied in. */
        flags = PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_USER;

        /* Map one page at a time, copy or zero fill as needed */
        for (page = vaddr_start; page < vaddr_end; page += MEMORY_PAGE_SIZE)
        {
            unsigned long phys;
            unsigned char *dest;
            unsigned long page_off;
            unsigned long copy_off;
            unsigned long copy_end;
            unsigned long copy_len;

            if (paging_get_phys(page) != 0)
            {
                rollback_mapped_pages(mapped_virt, mapped_phys, mapped_count);
                return ELF_ERR_MAP;
            }

            phys = phys_alloc_page();
            if (phys == 0)
            {
                rollback_mapped_pages(mapped_virt, mapped_phys, mapped_count);
                return ELF_ERR_MAP;
            }

            if (paging_map_page(page, phys, flags) != 0)
            {
                phys_free_page(phys);
                rollback_mapped_pages(mapped_virt, mapped_phys, mapped_count);
                return ELF_ERR_MAP;
            }
            if (mapped_count >= (sizeof(mapped_virt) / sizeof(mapped_virt[0])))
            {
                paging_unmap_page(page);
                phys_free_page(phys);
                rollback_mapped_pages(mapped_virt, mapped_phys, mapped_count);
                return ELF_ERR_MAP;
            }
            mapped_virt[mapped_count] = page;
            mapped_phys[mapped_count] = phys;
            mapped_count++;

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

            {
                unsigned long final_flags = PAGE_FLAG_PRESENT;
                final_flags |= PAGE_FLAG_USER;
                if (phdr->p_flags & PF_W) final_flags |= PAGE_FLAG_WRITABLE;
                if (!(phdr->p_flags & PF_X)) final_flags |= PAGE_FLAG_NO_EXECUTE;
                if (paging_set_page_flags(page, final_flags) != 0)
                {
                    rollback_mapped_pages(mapped_virt, mapped_phys, mapped_count);
                    return ELF_ERR_MAP;
                }
            }
        }
    }

    out->entry = (unsigned long)ehdr->e_entry;
    {
        unsigned long load_base = 0;
        unsigned long load_end = 0;
        collect_load_range(data, ehdr, &load_base, &load_end);
        register_active_image(data, len, load_base, load_end);
    }
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
        get_program_header_window(ehdr, len) != ELF_OK)
        return;

    unregister_active_image(data, len);

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
