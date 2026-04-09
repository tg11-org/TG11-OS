#ifndef TG11_ELF_H
#define TG11_ELF_H

/* Result codes */
#define ELF_OK              0
#define ELF_ERR_NULL       -1   /* null pointer argument */
#define ELF_ERR_MAGIC      -2   /* bad ELF magic */
#define ELF_ERR_CLASS      -3   /* not ELF64 */
#define ELF_ERR_TYPE       -4   /* not ET_EXEC or ET_DYN */
#define ELF_ERR_ARCH       -5   /* not x86-64 */
#define ELF_ERR_PHDR       -6   /* bad / missing program headers */
#define ELF_ERR_MAP        -7   /* paging_map_page failed */
#define ELF_ERR_RANGE      -8   /* segment out of supported range */
#define ELF_ERR_SHDR       -9   /* bad / missing section headers */
#define ELF_ERR_NOSYM     -10   /* no usable symbol table present */
#define ELF_ERR_OVERLAP   -11   /* PT_LOAD page windows overlap */
#define ELF_ERR_ENTRY     -12   /* entry point not inside a loadable segment */
#define ELF_ERR_INTERP    -13   /* PT_INTERP found: dynamic linking not supported */

/* elf_exec_t –– describes a loaded image ready to call */
typedef struct
{
    unsigned long entry;        /* virtual entry point */
    unsigned long load_base;    /* mapped PT_LOAD floor after bias */
    unsigned long load_end;     /* mapped PT_LOAD ceiling after bias */
} elf_exec_t;

typedef struct
{
    unsigned short type;
    unsigned short machine;
    unsigned short phnum;
    unsigned short shnum;
    unsigned long entry;
    unsigned long load_base;
    unsigned long load_end;
    unsigned long symbol_count;
} elf_info_t;

typedef struct
{
    const char *name;           /* points into the provided ELF data buffer */
    unsigned long value;
    unsigned long size;
    unsigned char info;
    unsigned short shndx;
} elf_symbol_t;

typedef int (*elf_symbol_visit_fn)(const elf_symbol_t *sym, void *ctx);

typedef struct
{
    unsigned int index;
    unsigned int type;
    unsigned int flags;
    unsigned long offset;
    unsigned long vaddr;
    unsigned long filesz;
    unsigned long memsz;
    unsigned long align;
} elf_program_header_info_t;

typedef struct
{
    unsigned int index;
    const char *name;
    unsigned int type;
    unsigned long flags;
    unsigned long addr;
    unsigned long offset;
    unsigned long size;
    unsigned long entsize;
    unsigned int link;
    unsigned int info;
} elf_section_info_t;

typedef int (*elf_program_header_visit_fn)(const elf_program_header_info_t *ph, void *ctx);
typedef int (*elf_section_visit_fn)(const elf_section_info_t *section, void *ctx);

/*
 * elf_load() –– parse an ELF64 executable from `data` (size `len`),
 * map its PT_LOAD segments using paging_map_page, and fill in *out.
 * Returns ELF_OK on success or a negative ELF_ERR_* code.
 *
 * The caller is responsible for calling elf_unload() when done.
 */
int elf_load(const unsigned char *data, unsigned long len, elf_exec_t *out);

int elf_get_info(const unsigned char *data, unsigned long len, elf_info_t *out);

int elf_visit_symbols(const unsigned char *data, unsigned long len,
                      elf_symbol_visit_fn visit, void *ctx);

int elf_find_symbol_by_addr(const unsigned char *data, unsigned long len,
                            unsigned long addr, elf_symbol_t *out_symbol,
                            unsigned long *out_offset);

int elf_visit_program_headers(const unsigned char *data, unsigned long len,
                              elf_program_header_visit_fn visit, void *ctx);

int elf_visit_sections(const unsigned char *data, unsigned long len,
                       elf_section_visit_fn visit, void *ctx);

int elf_symbolize_active_addr(unsigned long addr, elf_symbol_t *out_symbol,
                              unsigned long *out_offset,
                              unsigned long *out_image_base,
                              unsigned long *out_image_end);

/*
 * elf_call() –– jump to out->entry with no arguments and return its
 * return value.  The function image must still be mapped.
 */
long elf_call(const elf_exec_t *out);

/*
 * elf_unload() –– unmap all pages that elf_load() mapped.
 * Pass the same `data` and `len` used for elf_load().
 */
void elf_unload(const unsigned char *data, unsigned long len);

#endif
