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

/* elf_exec_t –– describes a loaded image ready to call */
typedef struct
{
    unsigned long entry;        /* virtual entry point */
} elf_exec_t;

/*
 * elf_load() –– parse an ELF64 executable from `data` (size `len`),
 * map its PT_LOAD segments using paging_map_page, and fill in *out.
 * Returns ELF_OK on success or a negative ELF_ERR_* code.
 *
 * The caller is responsible for calling elf_unload() when done.
 */
int elf_load(const unsigned char *data, unsigned long len, elf_exec_t *out);

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
