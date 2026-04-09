#!/usr/bin/env python3
"""Fix kernel/elf.c: replace the corrupted validate_elf64_image region
with two clean functions: validate_section_headers + validate_elf64_image."""

import sys, os, re

PATH = "/mnt/u/Projects/TG11-OS/kernel/elf.c"

with open(PATH, "rb") as f:
    raw = f.read()

text = raw.replace(b"\r\n", b"\n").decode("utf-8")

# Anchor: the corrupt section starts at "static int validate_elf64_image"
# and the clean code starts at "static const Elf64_Shdr *section_header_at"

START_MARKER = "static int validate_elf64_image("
END_MARKER   = "static const Elf64_Shdr *section_header_at("

start_idx = text.find(START_MARKER)
end_idx   = text.find(END_MARKER)

if start_idx == -1:
    print("ERROR: could not find validate_elf64_image", file=sys.stderr)
    sys.exit(1)
if end_idx == -1:
    print("ERROR: could not find section_header_at", file=sys.stderr)
    sys.exit(1)
if start_idx >= end_idx:
    print("ERROR: markers appear in wrong order", file=sys.stderr)
    sys.exit(1)

CLEAN = """\
static int validate_section_headers(const Elf64_Ehdr *ehdr, unsigned long len)
{
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0 || ehdr->e_shentsize < sizeof(Elf64_Shdr))
        return ELF_ERR_SHDR;
    if (ehdr->e_shoff >= len) return ELF_ERR_SHDR;
    if ((unsigned long)ehdr->e_shnum > ((len - ehdr->e_shoff) / (unsigned long)ehdr->e_shentsize))
        return ELF_ERR_SHDR;
    return ELF_OK;
}

static int validate_elf64_image(const unsigned char *data, unsigned long len, const Elf64_Ehdr **out_ehdr)
{
    const Elf64_Ehdr *ehdr;

    if (data == (void *)0 || out_ehdr == (void *)0) return ELF_ERR_NULL;
    if (len < sizeof(Elf64_Ehdr)) return ELF_ERR_MAGIC;

    ehdr = (const Elf64_Ehdr *)data;

    if (!check_magic(ehdr->e_ident)) return ELF_ERR_MAGIC;
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) return ELF_ERR_CLASS;
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) return ELF_ERR_CLASS;
    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT) return ELF_ERR_CLASS;
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return ELF_ERR_TYPE;
    if (ehdr->e_machine != EM_X86_64) return ELF_ERR_ARCH;
    if (ehdr->e_version != EV_CURRENT) return ELF_ERR_ARCH;
    if (ehdr->e_ehsize < sizeof(Elf64_Ehdr)) return ELF_ERR_CLASS;

    *out_ehdr = ehdr;
    return ELF_OK;
}

"""

new_text = text[:start_idx] + CLEAN + text[end_idx:]

# Write back (preserve original line endings style — use LF since this is a C file built in WSL)
with open(PATH, "wb") as f:
    f.write(new_text.encode("utf-8"))

print("Done. Replaced lines", text[:start_idx].count('\n')+1, "through",
      text[:end_idx].count('\n'), "with clean functions.")
print("New line count:", new_text.count('\n'))
